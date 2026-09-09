/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include <string.h>

#include "Backup.h"
#include "BoardPins.h"
#include "MemoryMap.h"
#include "System.h"
#include "Tick.h"

#include "EEndpoint.h"
#include "EFirmware.h"
#include "EOperation.h"
#include "FirmwareSlave.h"

using Boot::FirmwareSlave;
using NodeLib::Endpoint;
using NodeLib::FirmwareOp;
using NodeLib::Id;
using NodeLib::MAX_DATA;
using NodeLib::Message;
using NodeLib::Operation;

namespace
{
    constexpr uint32_t NodeSpacingMs = 25;
    constexpr uint32_t HeartbeatMs   = 500;

    // FirmwareOp::Status payload shape (Node-Flash-Layout-and-Bootloader-Spec.md
    // §6.2): op(1) state(1) expectedOffset(4 LE) lastError(1) fwVersion(2 LE).
    constexpr uint8_t StatusLen = 9;

    // FirmwareOp::Begin payload: op(1) module(1) imageSize(4) imageCrc32(4) fwVersion(2).
    constexpr uint8_t BeginLen = 12;

    // Local lastError codes (surfaced verbatim to the master).
    enum : uint8_t
    {
        ErrNone        = 0,
        ErrWrongModule = 1,
        ErrBadSize     = 2,
        ErrEraseFailed = 3,
        ErrProgramFail = 4,
        ErrOverrun     = 5,
        ErrCrcMismatch = 6,
        ErrBadState    = 7,
    };

    // The single 2 KB page staging buffer (spec §6.2). File-scope so it lands
    // in .bss where the linker accounts for it, not on the stack.
    uint8_t pageBuffer[Hal::Flash::PageSize];

    uint32_t ReadU32(const uint8_t* const p)
    {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
            (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    }

    void WriteU32(uint8_t* const p, const uint32_t v)
    {
        p[0] = static_cast<uint8_t>(v);
        p[1] = static_cast<uint8_t>(v >> 8);
        p[2] = static_cast<uint8_t>(v >> 16);
        p[3] = static_cast<uint8_t>(v >> 24);
    }
} // namespace

FirmwareSlave::FirmwareSlave(const uint32_t baudRate, const uint8_t nodeId, const uint8_t module) :
    uart(),
    crc(Hal::Crc::Poly::Ccitt16),
    frame(crc),
    baudRate(baudRate),
    nodeId(nodeId),
    module(module),
    state(State::Idle),
    lastError(ErrNone),
    imageSize(0),
    imageCrc32(0),
    fwVersion(0),
    expectedOffset(0),
    pageBase(Board::Flash::AppBase),
    pageLen(0),
    statusPending(false),
    led(Board::ErrorLed, Hal::Gpio::Mode::Output),
    heartbeatTimer()
{
}

void FirmwareSlave::Init()
{
    uart.Init(baudRate);
    heartbeatTimer.Start(HeartbeatMs);
    led.Write(true);
}

void FirmwareSlave::Loop()
{
    while (uart.Available())
    {
        Message received;
        if (frame.FeedByte(uart.Read(), received))
        {
            OnMessage(received);
        }
    }
    frame.Update();
    Heartbeat();
}

void FirmwareSlave::OnMessage(const Message& m)
{
    if (m.id.operation == Operation::Discover)
    {
        SendAnnounce();
        return;
    }

    if (m.id.node != nodeId && m.id.node != NodeLib::BROADCAST_NODE)
    {
        return;
    }

    if (m.id.endpoint == Endpoint::Transport)
    {
        if (m.id.operation == Operation::Poll)
        {
            if (statusPending)
            {
                SendStatus();
                statusPending = false;
            }
            SendDone();
        }
        return;
    }

    if (m.id.endpoint == Endpoint::Firmware)
    {
        OnFirmware(m);
    }
    // Every other endpoint is unserved while in the bootloader.
}

void FirmwareSlave::OnFirmware(const Message& m)
{
    if (m.id.operation == Operation::Get)
    {
        statusPending = true; // "are you in the bootloader?" probe
        return;
    }
    if (m.id.operation != Operation::Set || m.len < 1)
    {
        return;
    }

    switch (static_cast<FirmwareOp>(m.data[0]))
    {
        case FirmwareOp::Begin:
            HandleBegin(m);
            break;
        case FirmwareOp::Write:
            HandleWrite(m);
            break;
        case FirmwareOp::End:
            HandleEnd();
            break;
        case FirmwareOp::Activate:
            HandleActivate();
            break;
        case FirmwareOp::Abort:
            HandleAbort();
            break;
        default:
            break;
    }
}

void FirmwareSlave::HandleBegin(const Message& m)
{
    if (m.len < BeginLen)
    {
        Fault(ErrBadSize);
        return;
    }

    const uint8_t  wantModule = m.data[1];
    const uint32_t size       = ReadU32(&m.data[2]);
    const uint32_t imageCrc   = ReadU32(&m.data[6]);
    const uint16_t version    = static_cast<uint16_t>(m.data[10] | (m.data[11] << 8));

    if (wantModule != module)
    {
        Fault(ErrWrongModule);
        return;
    }
    if (size < (Board::Flash::AppDescriptorOffset + 32 + 4) || size > Board::Flash::AppSize)
    {
        Fault(ErrBadSize);
        return;
    }

    state = State::Erasing;
    if (!EraseAppSlot())
    {
        Fault(ErrEraseFailed);
        return;
    }

    imageSize      = size;
    imageCrc32     = imageCrc;
    fwVersion      = version;
    expectedOffset = 0;
    pageBase       = Board::Flash::AppBase;
    pageLen        = 0;
    lastError      = ErrNone;
    state          = State::Receiving;
    statusPending  = true;
}

void FirmwareSlave::HandleWrite(const Message& m)
{
    if (state != State::Receiving || m.len < 5)
    {
        return;
    }

    const uint32_t offset = ReadU32(&m.data[1]);
    const uint8_t  count  = static_cast<uint8_t>(m.len - 5);

    if (offset != expectedOffset)
    {
        return; // out of order -- master will rewind from our reported offset
    }
    if (expectedOffset + count > imageSize)
    {
        Fault(ErrOverrun);
        return;
    }

    if (!AppendImageBytes(&m.data[5], count))
    {
        Fault(ErrProgramFail);
        return;
    }
    statusPending = true;
}

void FirmwareSlave::HandleEnd()
{
    if (state != State::Receiving)
    {
        Fault(ErrBadState);
        return;
    }
    if (!FlushPage())
    {
        Fault(ErrProgramFail);
        return;
    }

    uint32_t computed;
    {
        Hal::Crc image(Hal::Crc::Poly::Ieee32);
        computed = image.Compute32(reinterpret_cast<const uint8_t*>(Board::Flash::AppBase), imageSize - 4U);
    }
    Hal::Crc restore(Hal::Crc::Poly::Ccitt16); // put the peripheral back for Frame

    const uint32_t trailing =
        *reinterpret_cast<const volatile uint32_t*>(Board::Flash::AppBase + imageSize - 4U);

    if (computed == trailing && computed == imageCrc32)
    {
        state     = State::Valid;
        lastError = ErrNone;
    }
    else
    {
        Fault(ErrCrcMismatch);
        return;
    }
    statusPending = true;
}

void FirmwareSlave::HandleActivate()
{
    if (state != State::Valid)
    {
        Fault(ErrBadState);
        return;
    }
    // Clear the stay-resident flag so the bootloader jumps to the new app.
    Hal::Backup::Write(Hal::Backup::Reg::Boot, 0);
    Hal::System::Reset(); // never returns
}

void FirmwareSlave::HandleAbort()
{
    state          = State::Idle;
    lastError      = ErrNone;
    expectedOffset = 0;
    pageLen        = 0;
    statusPending  = true;
}

bool FirmwareSlave::EraseAppSlot()
{
    Hal::Flash::Unlock();
    bool ok = true;
    for (uint32_t offset = 0; offset < Board::Flash::AppSize && ok; offset += Hal::Flash::PageSize)
    {
        ok = Hal::Flash::ErasePage(Board::Flash::AppBase + offset);
    }
    Hal::Flash::Lock();
    return ok;
}

bool FirmwareSlave::AppendImageBytes(const uint8_t* const bytes, const uint8_t count)
{
    for (uint8_t i = 0; i < count; i++)
    {
        const uint32_t address = Board::Flash::AppBase + expectedOffset;
        const uint32_t pageOf  = address & ~(Hal::Flash::PageSize - 1);

        if (pageOf != pageBase)
        {
            if (!FlushPage())
            {
                return false;
            }
            pageBase = pageOf;
            pageLen  = 0;
        }

        pageBuffer[pageLen] = bytes[i];
        pageLen++;
        expectedOffset++;
    }
    return true;
}

bool FirmwareSlave::FlushPage()
{
    if (pageLen == 0)
    {
        return true;
    }
    Hal::Flash::Unlock();
    const bool ok = Hal::Flash::Program(pageBase, pageBuffer, pageLen);
    Hal::Flash::Lock();
    pageLen = 0;
    return ok;
}

void FirmwareSlave::SendAnnounce()
{
    Message m(nodeId, Operation::Announce);
    m.data[0] = module;
    m.data[1] = static_cast<uint8_t>(state); // != app -> master knows we are in the bootloader
    m.len     = 2;
    Hal::Tick::DelayMs((nodeId - 1) * NodeSpacingMs);
    SendFrame(m);
}

void FirmwareSlave::SendStatus()
{
    Message m(Id(nodeId, Endpoint::Firmware, Operation::Report));
    m.data[0] = static_cast<uint8_t>(FirmwareOp::Status);
    m.data[1] = static_cast<uint8_t>(state);
    WriteU32(&m.data[2], expectedOffset);
    m.data[6] = lastError;
    m.data[7] = static_cast<uint8_t>(fwVersion);
    m.data[8] = static_cast<uint8_t>(fwVersion >> 8);
    m.len     = StatusLen;
    SendFrame(m);
}

void FirmwareSlave::SendDone()
{
    SendFrame(Message(nodeId, Operation::Done));
}

void FirmwareSlave::SendFrame(const Message& m)
{
    // v2 wire format (RS485-Node-Protocol-Spec-STM32G030.md §3), matching
    // NodeLib::Frame::Write: SYNC(2) LEN(1) NODE ENDPOINT OPERATION DATA CRC16.
    uint8_t frameBytes[2 + 1 + 3 + MAX_DATA + 2];
    frameBytes[0] = 0xEE;
    frameBytes[1] = 0x42;
    frameBytes[2] = m.len;
    frameBytes[3] = m.id.node;
    frameBytes[4] = static_cast<uint8_t>(m.id.endpoint);
    frameBytes[5] = static_cast<uint8_t>(m.id.operation);
    for (uint8_t i = 0; i < m.len; i++)
    {
        frameBytes[6 + i] = m.data[i];
    }

    const uint8_t  headerAndDataLen = static_cast<uint8_t>(3 + m.len);
    const uint16_t frameCrc         = crc.Compute(&frameBytes[3], headerAndDataLen);
    frameBytes[6 + m.len]           = static_cast<uint8_t>(frameCrc);
    frameBytes[7 + m.len]           = static_cast<uint8_t>(frameCrc >> 8);

    uart.Write(frameBytes, static_cast<size_t>(8 + m.len));
}

void FirmwareSlave::Fault(const uint8_t error)
{
    state         = State::Error;
    lastError     = error;
    statusPending = true;
}

void FirmwareSlave::Heartbeat()
{
    if (heartbeatTimer.Finished())
    {
        led.Write(state == State::Error ? true : !led.Read());
        heartbeatTimer.Start(state == State::Receiving ? 80 : HeartbeatMs);
    }
}
