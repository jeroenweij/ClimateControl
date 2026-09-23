/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include <string.h>

#include "Backup.h"
#include "BoardPins.h"
#include "BootHealth.h"
#include "MemoryMap.h"
#include "System.h"

#include "EEndpoint.h"
#include "EFirmware.h"
#include "Firmware.h"

using Boot::Firmware;
using NodeLib::Endpoint;
using NodeLib::FirmwareError;
using NodeLib::Id;
using NodeLib::Message;
using NodeLib::Operation;

namespace
{
    // OtaControl Set payload for ControlOp::Begin: op(1) imageSize(4)
    // imageCrc32(4) fwVersion(2). No module byte -- MainController only ever
    // has the one application slot, unlike the bus's Endpoint::Firmware.
    constexpr uint8_t BeginLen = 11;

    // OtaData Set payload: byteOffset(2 LE) data(<=32) -- every chunk is a
    // whole number of 8-byte double-words except possibly the image's final
    // one, so each write is independently, immediately programmable with no
    // RAM staging (Node-Flash-Layout-and-Bootloader-Spec.md §6.2.1's bus
    // design, minus its op byte -- Endpoint::OtaData itself means "write").
    constexpr uint8_t DataHeaderLen = 2;
    constexpr uint8_t ChunkDataLen  = 32;

    // OtaData Ack/Nack reply: byteOffset(2 LE) chunkCrc16(2 LE) programFailed(1).
    constexpr uint8_t DataReplyLen = 5;

    // OtaControl status Report (Operation::Get): state(1) expectedOffset(4 LE)
    // lastError(1) fwVersion(2 LE).
    constexpr uint8_t StatusLen = 8;

    // lastError codes, surfaced verbatim to the server -- the shared
    // NodeLib::FirmwareError numbering, so one error vocabulary covers the bus
    // bootloader and this one.
    constexpr uint8_t ErrNone        = static_cast<uint8_t>(FirmwareError::None);
    constexpr uint8_t ErrBadSize     = static_cast<uint8_t>(FirmwareError::BadSize);
    constexpr uint8_t ErrEraseFailed = static_cast<uint8_t>(FirmwareError::EraseFailed);
    constexpr uint8_t ErrOverrun     = static_cast<uint8_t>(FirmwareError::Overrun);
    constexpr uint8_t ErrCrcMismatch = static_cast<uint8_t>(FirmwareError::CrcMismatch);
    constexpr uint8_t ErrBadState    = static_cast<uint8_t>(FirmwareError::BadState);

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

Firmware::Firmware() :
    crc(Hal::Crc::Poly::Ccitt16),
    state(State::Idle),
    lastError(ErrNone),
    imageSize(0),
    imageCrc32(0),
    fwVersion(0),
    expectedOffset(0),
    partialCommitted(0),
    replyPending(false),
    pendingReply(),
    errorLed(Board::ErrorLed, Hal::Gpio::Mode::Output)
{
}

void Firmware::Init()
{
    errorLed.Write(false);
}

bool Firmware::PopReply(Message& out)
{
    if (!replyPending)
    {
        return false;
    }
    out          = pendingReply;
    replyPending = false;
    return true;
}

void Firmware::OnControl(const Message& m)
{
    if (m.id.operation == Operation::Get)
    {
        QueueStatus();
        return;
    }
    if (m.id.operation != Operation::Set || m.len < 1)
    {
        return;
    }

    switch (static_cast<ControlOp>(m.data[0]))
    {
        case ControlOp::Begin:
            HandleBegin(m);
            break;
        case ControlOp::Abort:
            HandleAbort();
            break;
        case ControlOp::End:
            HandleEnd();
            break;
        case ControlOp::Activate:
            HandleActivate();
            break;
        default:
            break;
    }
}

void Firmware::OnData(const Message& m)
{
    if (m.id.operation != Operation::Set)
    {
        return;
    }
    if (state != State::Receiving || m.len < DataHeaderLen)
    {
        return; // malformed -- no correlation to reply to, drop silently
    }

    const uint16_t offset = ReadU16(&m.data[0]);
    const uint8_t  count  = static_cast<uint8_t>(m.len - DataHeaderLen);

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
        // Genuine gap -- should not happen if the server only ever sends the
        // next expected chunk, but must be handled: Nack naming where we
        // actually are so it can resync without guessing.
        QueueDataReply(true, static_cast<uint16_t>(expectedOffset), 0, false);
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
        QueueDataReply(true, static_cast<uint16_t>(expectedOffset), 0, false);
        return;
    }

    const uint32_t address   = Board::Flash::AppBase + expectedOffset;
    uint8_t        committed = partialCommitted;

    if (committed < count)
    {
        Hal::Flash::Unlock();
        const size_t done = Hal::Flash::Program(
            address + committed, &m.data[DataHeaderLen + committed], count - committed);
        Hal::Flash::Lock();
        committed = static_cast<uint8_t>(committed + done);
    }

    partialCommitted = committed;

    if (committed < count)
    {
        // Program() stopped partway -- a real hardware failure. Report it,
        // leave expectedOffset/partialCommitted exactly where they are so a
        // retry of this same offset resumes from this point, not the start.
        QueueDataReply(true, offset, ChunkCrc(address, committed), true);
        return;
    }

    expectedOffset += count;
    partialCommitted = 0;
    QueueDataReply(false, offset, ChunkCrc(address, count), false);
}

void Firmware::AckFromFlash(const uint16_t offset, const uint8_t count)
{
    if (count == 0 || static_cast<uint32_t>(offset) + count > expectedOffset)
    {
        // Can't vouch for bytes beyond what's actually been committed.
        QueueDataReply(true, static_cast<uint16_t>(expectedOffset), 0, false);
        return;
    }
    const uint32_t address = Board::Flash::AppBase + offset;
    QueueDataReply(false, offset, ChunkCrc(address, count), false);
}

uint16_t Firmware::ChunkCrc(const uint32_t address, const uint8_t count)
{
    uint8_t buf[ChunkDataLen];
    Hal::Flash::Read(address, buf, count);
    return crc.Compute(buf, count);
}

void Firmware::HandleBegin(const Message& m)
{
    if (m.len < BeginLen)
    {
        FaultControl(ErrBadSize);
        return;
    }

    const uint32_t size     = ReadU32(&m.data[1]);
    const uint32_t imageCrc = ReadU32(&m.data[5]);
    const uint16_t version  = static_cast<uint16_t>(m.data[9] | (m.data[10] << 8));

    if (size < (Board::Flash::AppDescriptorOffset + 32 + 4) || size > Board::Flash::AppSize)
    {
        FaultControl(ErrBadSize);
        return;
    }

    state = State::Erasing;
    if (!EraseAppSlot())
    {
        FaultControl(ErrEraseFailed);
        return;
    }

    // A newly-received image deserves a full boot-fail budget of its own,
    // not whatever was left over from the image it's replacing.
    Tools::BootHealth::ResetFailedBootCount();

    imageSize        = size;
    imageCrc32       = imageCrc;
    fwVersion        = version;
    expectedOffset   = 0;
    partialCommitted = 0;
    lastError        = ErrNone;
    state            = State::Receiving;
    QueueControlReply(false, ErrNone);
}

void Firmware::HandleAbort()
{
    state            = State::Idle;
    lastError        = ErrNone;
    expectedOffset   = 0;
    partialCommitted = 0;
    QueueControlReply(false, ErrNone);
}

void Firmware::HandleEnd()
{
    if (state != State::Receiving || expectedOffset != imageSize)
    {
        FaultControl(ErrBadState);
        return;
    }

    uint32_t computed;
    {
        Hal::Crc image(Hal::Crc::Poly::Ieee32);
        computed = image.Compute32(reinterpret_cast<const uint8_t*>(Board::Flash::AppBase), imageSize - 4U);
    }
    Hal::Crc restore(Hal::Crc::Poly::Ccitt16); // put the peripheral back -- UplinkHandler's frame CRC shares it

    uint8_t trailingBytes[4];
    Hal::Flash::Read(Board::Flash::AppBase + imageSize - 4U, trailingBytes, sizeof(trailingBytes));
    const uint32_t trailing = ReadU32(trailingBytes);

    if (computed == trailing && computed == imageCrc32)
    {
        state     = State::Valid;
        lastError = ErrNone;
        QueueControlReply(false, ErrNone);
    }
    else
    {
        FaultControl(ErrCrcMismatch);
    }
}

void Firmware::HandleActivate()
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

bool Firmware::EraseAppSlot()
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

void Firmware::QueueDataReply(
    const bool     nack,
    const uint16_t offset,
    const uint16_t chunkCrc16,
    const bool     programFailed)
{
    Message m(Id(0, Endpoint::OtaData, nack ? Operation::Nack : Operation::Ack));
    WriteU16(&m.data[0], offset);
    WriteU16(&m.data[2], chunkCrc16);
    m.data[4]    = programFailed ? 1 : 0;
    m.len        = DataReplyLen;
    pendingReply = m;
    replyPending = true;
}

void Firmware::QueueControlReply(const bool nack, const uint8_t error)
{
    Message m(Id(0, Endpoint::OtaControl, nack ? Operation::Nack : Operation::Ack));
    m.data[0]    = error;
    m.len        = 1;
    pendingReply = m;
    replyPending = true;
}

void Firmware::QueueStatus()
{
    Message m(Id(0, Endpoint::OtaControl, Operation::Report));
    m.data[0] = static_cast<uint8_t>(state);
    WriteU32(&m.data[1], expectedOffset);
    m.data[5]    = lastError;
    m.data[6]    = static_cast<uint8_t>(fwVersion);
    m.data[7]    = static_cast<uint8_t>(fwVersion >> 8);
    m.len        = StatusLen;
    pendingReply = m;
    replyPending = true;
}

void Firmware::Fault(const uint8_t error)
{
    // Marks the node's persistent state only -- a status Get after this
    // still sees the fault. Does not queue any reply on its own; callers
    // reply on whichever channel (OtaData's Ack/Nack, or FaultControl()
    // below for Begin/End) actually correlates to the request that failed.
    state     = State::Error;
    lastError = error;
    errorLed.Write(true);
}

void Firmware::FaultControl(const uint8_t error)
{
    Fault(error);
    QueueControlReply(true, error);
}
