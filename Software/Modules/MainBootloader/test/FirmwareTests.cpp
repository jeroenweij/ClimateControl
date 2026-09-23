/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "BoardPins.h"
#include "Crc.h"
#include "MemoryMap.h"

#include "FakeBackup.h"
#include "FakeClock.h"
#include "FakeFlash.h"
#include "Test.h"

#include "Firmware.h"

using Boot::Firmware;
using NodeLib::Endpoint;
using NodeLib::Id;
using NodeLib::Message;
using NodeLib::Operation;

// Two things NOT covered here, deliberately, both matching boundaries
// Bootloader/test/FirmwareSlaveTests.cpp's own notes already draw for the
// flash state machine this was ported from:
//   - HandleEnd()'s CRC-match path (success or mismatch) -- it reads the
//     flashed image in place via Hal::Crc::Compute32() on the *real* MCU
//     address (Board::Flash::AppBase), not through Hal::Flash::Read()
//     (buffering up to 50 KB through Read() would blow the real MCU's RAM
//     budget for no production benefit), so it segfaults on host the moment
//     any test actually completes an image and reaches it. Only the
//     completeness precondition ahead of that point is exercised below.
//   - HandleActivate()'s success path calls Hal::System::Reset(), which
//     FakeSystem.cpp deliberately hangs forever (matching real hardware's
//     "never returns").
namespace
{
    void ResetWorld()
    {
        FakeFlash::Reset();
        FakeBackup::Reset();
    }

    void WriteU32(uint8_t* const p, const uint32_t v)
    {
        p[0] = static_cast<uint8_t>(v);
        p[1] = static_cast<uint8_t>(v >> 8);
        p[2] = static_cast<uint8_t>(v >> 16);
        p[3] = static_cast<uint8_t>(v >> 24);
    }

    void WriteU16(uint8_t* const p, const uint16_t v)
    {
        p[0] = static_cast<uint8_t>(v);
        p[1] = static_cast<uint8_t>(v >> 8);
    }

    // ControlOp values, mirrored from Firmware.h's private enum -- OnControl()
    // only exposes them via the wire byte, so tests build that byte directly.
    enum : uint8_t
    {
        OpBegin    = 1,
        OpAbort    = 2,
        OpEnd      = 3,
        OpActivate = 4,
    };

    Message MakeBegin(const uint32_t size, const uint32_t imageCrc32, const uint16_t fwVersion)
    {
        Message m(Id(0, Endpoint::OtaControl, Operation::Set));
        m.data[0] = OpBegin;
        WriteU32(&m.data[1], size);
        WriteU32(&m.data[5], imageCrc32);
        m.data[9]  = static_cast<uint8_t>(fwVersion);
        m.data[10] = static_cast<uint8_t>(fwVersion >> 8);
        m.len      = 11;
        return m;
    }

    Message MakeData(const uint16_t offset, const uint8_t* const bytes, const uint8_t count)
    {
        Message m(Id(0, Endpoint::OtaData, Operation::Set));
        WriteU16(&m.data[0], offset);
        for (uint8_t i = 0; i < count; i++)
        {
            m.data[2 + i] = bytes[i];
        }
        m.len = static_cast<uint8_t>(2 + count);
        return m;
    }

    Message MakeControl(const uint8_t op)
    {
        Message m(Id(0, Endpoint::OtaControl, Operation::Set));
        m.data[0] = op;
        m.len     = 1;
        return m;
    }

    uint16_t ChunkCrc16(const uint8_t* const data, const uint8_t count)
    {
        Hal::Crc crc(Hal::Crc::Poly::Ccitt16);
        return crc.Compute(data, count);
    }
} // namespace

CC_TEST(Firmware, BeginRejectsAnImageSmallerThanTheDescriptorFloor)
{
    ResetWorld();
    Firmware fw;
    fw.Init();

    fw.OnControl(MakeBegin(4, 0, 1));

    Message reply;
    CC_CHECK(fw.PopReply(reply));
    CC_CHECK(reply.id.operation == Operation::Nack);
    CC_CHECK(reply.id.endpoint == Endpoint::OtaControl);
}

CC_TEST(Firmware, BeginErasesTheSlotAndAcksOnOtaControl)
{
    ResetWorld();
    FakeFlash::Data(); // sanity: fake flash exists before we touch it

    Firmware fw;
    fw.Init();

    const uint32_t size = Board::Flash::AppDescriptorOffset + 32 + 4 + 32;
    fw.OnControl(MakeBegin(size, 0x12345678u, 7));

    Message reply;
    CC_CHECK(fw.PopReply(reply));
    CC_CHECK(reply.id.endpoint == Endpoint::OtaControl);
    CC_CHECK(reply.id.operation == Operation::Ack);

    // Erased -- FakeFlash::Reset() already leaves it at 0xFF, but Begin must
    // have actually gone through EraseAppSlot() rather than bailing out
    // early; a status Get should now report State::Receiving (=3).
    fw.OnControl(Message(Id(0, Endpoint::OtaControl, Operation::Get)));
    CC_CHECK(fw.PopReply(reply));
    CC_CHECK_EQ(reply.data[0], 3);
}

CC_TEST(Firmware, DataProgramsFlashAndAcksWithTheChunksCrc)
{
    ResetWorld();
    Firmware fw;
    fw.Init();

    const uint32_t size = Board::Flash::AppDescriptorOffset + 32 + 4 + 32;
    Message        reply;
    fw.OnControl(MakeBegin(size, 0, 1));
    fw.PopReply(reply);

    uint8_t chunk[32];
    for (uint8_t i = 0; i < 32; i++)
    {
        chunk[i] = static_cast<uint8_t>(0xA0 + i);
    }
    fw.OnData(MakeData(0, chunk, 32));

    CC_CHECK(fw.PopReply(reply));
    CC_CHECK(reply.id.endpoint == Endpoint::OtaData);
    CC_CHECK(reply.id.operation == Operation::Ack);
    CC_CHECK_EQ(reply.data[0], 0); // offset lo
    CC_CHECK_EQ(reply.data[1], 0); // offset hi
    const uint16_t expectedCrc = ChunkCrc16(chunk, 32);
    CC_CHECK_EQ(reply.data[2], static_cast<uint8_t>(expectedCrc));
    CC_CHECK_EQ(reply.data[3], static_cast<uint8_t>(expectedCrc >> 8));
    CC_CHECK_EQ(reply.data[4], 0); // programFailed

    CC_CHECK_EQ(FakeFlash::Data()[0], 0xA0);
    CC_CHECK_EQ(FakeFlash::Data()[31], 0xA0 + 31);
}

CC_TEST(Firmware, DataAheadOfExpectedOffsetIsNackedWithTheRealOffset)
{
    ResetWorld();
    Firmware fw;
    fw.Init();

    const uint32_t size = Board::Flash::AppDescriptorOffset + 32 + 4 + 64;
    Message        reply;
    fw.OnControl(MakeBegin(size, 0, 1));
    fw.PopReply(reply);

    uint8_t chunk[32] = {};
    fw.OnData(MakeData(32, chunk, 32)); // expected offset is 0, not 32

    CC_CHECK(fw.PopReply(reply));
    CC_CHECK(reply.id.operation == Operation::Nack);
    CC_CHECK_EQ(reply.data[0], 0);
    CC_CHECK_EQ(reply.data[1], 0);
}

CC_TEST(Firmware, DuplicateDataBelowExpectedOffsetIsAckedFromFlashNotReprogrammed)
{
    ResetWorld();
    Firmware fw;
    fw.Init();

    const uint32_t size = Board::Flash::AppDescriptorOffset + 32 + 4 + 32;
    Message        reply;
    fw.OnControl(MakeBegin(size, 0, 1));
    fw.PopReply(reply);

    uint8_t chunk[32];
    for (uint8_t i = 0; i < 32; i++)
    {
        chunk[i] = static_cast<uint8_t>(i);
    }
    fw.OnData(MakeData(0, chunk, 32));
    fw.PopReply(reply);

    // Resend the same (already-committed) chunk -- simulates a lost ack.
    fw.OnData(MakeData(0, chunk, 32));

    CC_CHECK(fw.PopReply(reply));
    CC_CHECK(reply.id.operation == Operation::Ack);
    const uint16_t expectedCrc = ChunkCrc16(chunk, 32);
    CC_CHECK_EQ(reply.data[2], static_cast<uint8_t>(expectedCrc));
    CC_CHECK_EQ(reply.data[3], static_cast<uint8_t>(expectedCrc >> 8));
}

CC_TEST(Firmware, DataOverrunIsNackedNotSilentlyDropped)
{
    ResetWorld();
    Firmware fw;
    fw.Init();

    const uint32_t size = Board::Flash::AppDescriptorOffset + 32 + 4 + 32;
    Message        reply;
    fw.OnControl(MakeBegin(size, 0, 1));
    fw.PopReply(reply);

    uint8_t chunk[33] = {};
    fw.OnData(MakeData(0, chunk, 33)); // one byte past ChunkDataLen (32)

    CC_CHECK(fw.PopReply(reply));
    CC_CHECK(reply.id.operation == Operation::Nack);
}

CC_TEST(Firmware, EndFaultsWhenNotAllBytesHaveArrivedYet)
{
    ResetWorld();
    Firmware fw;
    fw.Init();

    const uint32_t size = Board::Flash::AppDescriptorOffset + 32 + 4 + 64;
    Message        reply;
    fw.OnControl(MakeBegin(size, 0, 1));
    fw.PopReply(reply);

    fw.OnControl(MakeControl(OpEnd));

    CC_CHECK(fw.PopReply(reply));
    CC_CHECK(reply.id.operation == Operation::Nack);
}

CC_TEST(Firmware, AbortReturnsToIdle)
{
    ResetWorld();
    Firmware fw;
    fw.Init();

    const uint32_t size = Board::Flash::AppDescriptorOffset + 32 + 4 + 32;
    Message        reply;
    fw.OnControl(MakeBegin(size, 0, 1));
    fw.PopReply(reply);

    fw.OnControl(MakeControl(OpAbort));
    CC_CHECK(fw.PopReply(reply));
    CC_CHECK(reply.id.operation == Operation::Ack);

    fw.OnControl(Message(Id(0, Endpoint::OtaControl, Operation::Get)));
    CC_CHECK(fw.PopReply(reply));
    CC_CHECK_EQ(reply.data[0], 1); // State::Idle
}

CC_TEST(Firmware, ActivateFaultsWithoutReplyWhenNoValidImageIsStaged)
{
    ResetWorld();
    Firmware fw;
    fw.Init();

    fw.OnControl(MakeControl(OpActivate)); // never Begin'd -- still Idle

    Message reply;
    CC_CHECK(!fw.PopReply(reply)); // ported verbatim from FirmwareSlave's HandleActivate(): Fault() only, no queued reply
}

CC_TEST(Firmware, StatusGetReportsExpectedOffsetAndFwVersion)
{
    ResetWorld();
    Firmware fw;
    fw.Init();

    const uint32_t size = Board::Flash::AppDescriptorOffset + 32 + 4 + 64;
    Message        reply;
    fw.OnControl(MakeBegin(size, 0, 99));
    fw.PopReply(reply);

    uint8_t chunk[32] = {};
    fw.OnData(MakeData(0, chunk, 32));
    fw.PopReply(reply);

    fw.OnControl(Message(Id(0, Endpoint::OtaControl, Operation::Get)));
    CC_CHECK(fw.PopReply(reply));
    CC_CHECK(reply.id.operation == Operation::Report);
    CC_CHECK_EQ(reply.data[0], 3); // State::Receiving
    CC_CHECK_EQ(reply.data[1], 32); // expectedOffset lo
    CC_CHECK_EQ(reply.data[2], 0);
    CC_CHECK_EQ(reply.data[6], 99); // fwVersion lo
    CC_CHECK_EQ(reply.data[7], 0);
}

namespace
{
    bool LedOn(const Hal::Pin& led)
    {
        return (led.port->ODR & led.pin) != 0;
    }
} // namespace

CC_TEST(Firmware, HeartbeatBlinksTheActivityLedSoTheBootloaderIsRecognisable)
{
    ResetWorld();
    FakeClock::Reset();
    Firmware fw;
    fw.Init();
    CC_CHECK(!LedOn(Board::ActivityLed));

    fw.Loop(); // heartbeat period not elapsed yet
    CC_CHECK(!LedOn(Board::ActivityLed));

    FakeClock::Advance(500);
    fw.Loop();
    CC_CHECK(LedOn(Board::ActivityLed));

    FakeClock::Advance(500);
    fw.Loop();
    CC_CHECK(!LedOn(Board::ActivityLed));
    CC_CHECK(!LedOn(Board::ErrorLed));
}

CC_TEST(Firmware, HeartbeatBlinksFasterWhileAnImageIsBeingReceived)
{
    ResetWorld();
    FakeClock::Reset();
    Firmware fw;
    fw.Init();

    Message reply;
    fw.OnControl(MakeBegin(Board::Flash::AppDescriptorOffset + 32 + 4 + 32, 0, 1));
    fw.PopReply(reply);

    FakeClock::Advance(500); // first tick still uses the idle period, then arms the fast one
    fw.Loop();
    const bool afterFirst = LedOn(Board::ActivityLed);
    FakeClock::Advance(80);
    fw.Loop();
    CC_CHECK(LedOn(Board::ActivityLed) != afterFirst);
}

CC_TEST(Firmware, AFaultStopsTheActivityBlinkAndBlinksTheErrorLedInstead)
{
    ResetWorld();
    FakeClock::Reset();
    Firmware fw;
    fw.Init();

    fw.OnControl(MakeBegin(4, 0, 1)); // too small -> Error state
    FakeClock::Advance(500);
    fw.Loop();
    CC_CHECK(LedOn(Board::ErrorLed));
    CC_CHECK(!LedOn(Board::ActivityLed));

    FakeClock::Advance(500);
    fw.Loop();
    CC_CHECK(!LedOn(Board::ErrorLed)); // blinking, not stuck on
}
