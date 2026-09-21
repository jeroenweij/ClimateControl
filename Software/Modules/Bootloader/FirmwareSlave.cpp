/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include <string.h>

#include "Backup.h"
#include "BoardPins.h"
#include "BootHealth.h"
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
    constexpr uint32_t NodeSpacingMs = 10; // must match NodeLib::Node::nodeSpacing
    constexpr uint32_t HeartbeatMs   = 500;

    // FirmwareOp::Status payload shape (Node-Flash-Layout-and-Bootloader-Spec.md
    // §6.2): op(1) state(1) expectedOffset(4 LE) lastError(1) fwVersion(2 LE).
    constexpr uint8_t StatusLen = 9;

    // FirmwareOp::Begin payload: op(1) module(1) imageSize(4) imageCrc32(4) fwVersion(2).
    constexpr uint8_t BeginLen = 12;

    // Write payload (v2, Node-Flash-Layout-and-Bootloader-Spec.md §6.2.1):
    // op(1) byteOffset(2 LE) data(<=32) -- every chunk is a whole number of
    // 8-byte double-words except possibly the image's final one, so each
    // Write is independently, immediately programmable with no RAM staging.
    constexpr uint8_t WriteHeaderLen = 3;
    constexpr uint8_t ChunkDataLen   = 32;

    // Ack/Nack reply to a Write: byteOffset(2 LE) chunkCrc16(2 LE) programFailed(1).
    constexpr uint8_t WriteReplyLen = 5;

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

    uint16_t ReadU16(const uint8_t* const p)
    {
        return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
    }

    void WriteU16(uint8_t* const p, const uint16_t v)
    {
        p[0] = static_cast<uint8_t>(v);
        p[1] = static_cast<uint8_t>(v >> 8);
    }
} // namespace

FirmwareSlave::FirmwareSlave(const uint8_t nodeId, const uint8_t module) :
    uart(),
    crc(Hal::Crc::Poly::Ccitt16),
    frame(crc),
    nodeId(nodeId),
    module(module),
    state(State::Idle),
    lastError(ErrNone),
    imageSize(0),
    imageCrc32(0),
    fwVersion(0),
    expectedOffset(0),
    partialCommitted(0),
    statusPending(false),
    writeReplyPending(false),
    writeReplyNack(false),
    writeReplyOffset(0),
    writeReplyCrc16(0),
    writeReplyProgramFailed(false),
    activityLed(Board::ActivityLed, Hal::Gpio::Mode::Output), // TEMP DEBUG -- revert after triage
    errorLed(Board::ErrorLed, Hal::Gpio::Mode::Output),
    heartbeatTimer()
{
}

void FirmwareSlave::Init()
{
    uart.Init(Board::BusBaudRate, module);
    heartbeatTimer.Start(HeartbeatMs);
    activityLed.Write(false);
    errorLed.Write(false);
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
    activityLed.Write(!activityLed.Read()); // TEMP DEBUG -- revert after triage: toggles on every parsed frame

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
            if (writeReplyPending)
            {
                SendWriteReply();
                writeReplyPending = false;
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

    // A newly-received image deserves a full boot-fail budget of its own,
    // not whatever was left over from the image it's replacing.
    Tools::BootHealth::ResetFailedBootCount();

    imageSize         = size;
    imageCrc32        = imageCrc;
    fwVersion         = version;
    expectedOffset    = 0;
    partialCommitted  = 0;
    lastError         = ErrNone;
    state             = State::Receiving;
    statusPending     = true;
    writeReplyPending = false;
}

void FirmwareSlave::HandleWrite(const Message& m)
{
    if (state != State::Receiving || m.len < WriteHeaderLen)
    {
        return; // malformed -- no correlation to reply to, drop silently
    }

    const uint16_t offset = ReadU16(&m.data[1]);
    const uint8_t  count  = static_cast<uint8_t>(m.len - WriteHeaderLen);

    if (offset < expectedOffset)
    {
        // Duplicate -- the write already landed, only its ack was lost.
        // Never call Hal::Flash::Program() again for it (STM32G0 PROGERR on
        // a second write to an already-programmed double-word, even
        // identical data) -- just read back what's already there.
        AckFromFlash(offset, count);
        return;
    }

    if (offset > expectedOffset)
    {
        // Genuine gap -- should not happen if the master only ever sends the
        // next expected chunk, but must be handled: Nack naming where we
        // actually are so the master can resync without guessing.
        QueueWriteReply(true, static_cast<uint16_t>(expectedOffset), 0, false);
        return;
    }

    // offset == expectedOffset: new data, or resuming a chunk that only
    // partially programmed last time (a genuine Hal::Flash::Program()
    // failure, not a lost ack -- a lost ack always shows up as offset <
    // expectedOffset instead, since expectedOffset only advances once a
    // chunk fully commits).
    if (count == 0 || count > ChunkDataLen || expectedOffset + count > imageSize)
    {
        Fault(ErrOverrun);
        return;
    }

    const uint32_t address   = Board::Flash::AppBase + expectedOffset;
    uint8_t        committed = partialCommitted;

    if (committed < count)
    {
        Hal::Flash::Unlock();
        const size_t done = Hal::Flash::Program(
            address + committed, &m.data[WriteHeaderLen + committed], count - committed);
        Hal::Flash::Lock();
        committed = static_cast<uint8_t>(committed + done);
    }

    partialCommitted = committed;

    if (committed < count)
    {
        // Program() stopped partway -- a real hardware failure. Report it,
        // leave expectedOffset/partialCommitted exactly where they are so a
        // retry of this same offset resumes from this point, not the start.
        QueueWriteReply(true, offset, ChunkCrc(address, committed), true);
        return;
    }

    expectedOffset += count;
    partialCommitted = 0;
    QueueWriteReply(false, offset, ChunkCrc(address, count), false);
}

void FirmwareSlave::AckFromFlash(const uint16_t offset, const uint8_t count)
{
    if (count == 0 || static_cast<uint32_t>(offset) + count > expectedOffset)
    {
        // Can't vouch for bytes beyond what's actually been committed.
        QueueWriteReply(true, static_cast<uint16_t>(expectedOffset), 0, false);
        return;
    }
    const uint32_t address = Board::Flash::AppBase + offset;
    QueueWriteReply(false, offset, ChunkCrc(address, count), false);
}

uint16_t FirmwareSlave::ChunkCrc(const uint32_t address, const uint8_t count)
{
    uint8_t buf[ChunkDataLen];
    Hal::Flash::Read(address, buf, count);
    return crc.Compute(buf, count);
}

void FirmwareSlave::QueueWriteReply(
    const bool     nack,
    const uint16_t offset,
    const uint16_t chunkCrc16,
    const bool     programFailed)
{
    writeReplyNack          = nack;
    writeReplyOffset        = offset;
    writeReplyCrc16         = chunkCrc16;
    writeReplyProgramFailed = programFailed;
    writeReplyPending       = true;
}

void FirmwareSlave::HandleEnd()
{
    if (state != State::Receiving || expectedOffset != imageSize)
    {
        Fault(ErrBadState);
        return;
    }

    uint32_t computed;
    {
        Hal::Crc image(Hal::Crc::Poly::Ieee32);
        computed = image.Compute32(reinterpret_cast<const uint8_t*>(Board::Flash::AppBase), imageSize - 4U);
    }
    Hal::Crc restore(Hal::Crc::Poly::Ccitt16); // put the peripheral back for Frame

    uint8_t trailingBytes[4];
    Hal::Flash::Read(Board::Flash::AppBase + imageSize - 4U, trailingBytes, sizeof(trailingBytes));
    const uint32_t trailing = ReadU32(trailingBytes);

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
    state             = State::Idle;
    lastError         = ErrNone;
    expectedOffset    = 0;
    partialCommitted  = 0;
    statusPending     = true;
    writeReplyPending = false;
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

void FirmwareSlave::SendWriteReply()
{
    Message m(Id(nodeId, Endpoint::Firmware, writeReplyNack ? Operation::Nack : Operation::Ack));
    WriteU16(&m.data[0], writeReplyOffset);
    WriteU16(&m.data[2], writeReplyCrc16);
    m.data[4] = writeReplyProgramFailed ? 1 : 0;
    m.len     = WriteReplyLen;
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
        errorLed.Write(state == State::Error ? !errorLed.Read() : false);
        activityLed.Write(state == State::Error ? false : !activityLed.Read());
        heartbeatTimer.Start(state == State::Receiving ? 80 : HeartbeatMs);
    }
}
