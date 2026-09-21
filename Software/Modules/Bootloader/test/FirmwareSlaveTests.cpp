/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "Crc.h"
#include "EEndpoint.h"
#include "EFirmware.h"
#include "EOperation.h"
#include "MemoryMap.h"
#include "Uart.h"

#include "FakeBus.h"
#include "FakeClock.h"
#include "FakeFlash.h"
#include "FakeOtaUart.h"
#include "Test.h"

#include "FirmwareSlave.h"

using Boot::FirmwareSlave;
using NodeLib::Endpoint;
using NodeLib::FirmwareOp;
using NodeLib::Frame;
using NodeLib::Id;
using NodeLib::Message;
using NodeLib::Operation;

// FirmwareSlave.cpp used to read flash via raw pointer casts on
// Board::Flash::AppBase -- a real MCU address, unsafe to dereference on
// host. Hal::Flash::Read() (Lib/HAL/Flash.h) was added specifically to make
// this testable; FakeFlash.cpp backs it with a host buffer. NOT covered
// here, deliberately: HandleEnd()'s whole-image CRC match (still reads
// flash in place via Hal::Crc::Compute32() -- buffering up to 50 KB through
// Read() would blow the real MCU's RAM budget for no production benefit)
// and HandleActivate()'s success path (calls Hal::System::Reset(), which
// FakeSystem.cpp deliberately hangs forever, matching real hardware's
// "never returns" -- same boundary as NodeTests.cpp's SystemControl notes).
namespace
{
    const uint8_t kNodeId = 5;
    const uint8_t kModule = 1;

    void ResetWorld()
    {
        FakeBus::Reset();
        FakeOtaUart::Reset();
        FakeClock::Reset();
        FakeFlash::Reset();
    }

    // Reuses the real NodeLib::Frame codec, same trick BusHelpers.cpp uses --
    // encode onto FakeBus's Tx as a scratchpad, copy the bytes into
    // FakeOtaUart's Rx, then discard them from FakeBus so it never actually
    // participates in the test.
    void InjectOtaFrame(const Message& message)
    {
        Hal::Crc  crc;
        Frame     frame(crc);
        Hal::Uart scratchUart;

        const size_t before = FakeBus::TxLen();
        frame.Write(scratchUart, message);
        const size_t after = FakeBus::TxLen();

        FakeOtaUart::InjectRx(FakeBus::Tx() + before, after - before);
        FakeBus::TruncateTx(before);
    }

    int DecodeOtaTx(Message* const out, const int maxOut)
    {
        Hal::Crc crc;
        Frame    frame(crc);

        const uint8_t* const tx  = FakeOtaUart::Tx();
        const size_t         len = FakeOtaUart::TxLen();

        int count = 0;
        for (size_t i = 0; i < len && count < maxOut; i++)
        {
            if (frame.FeedByte(tx[i], out[count]))
            {
                count++;
            }
        }
        return count;
    }

    Message MakeFirmwareSet(const FirmwareOp op)
    {
        Message m(Id(kNodeId, Endpoint::Firmware, Operation::Set));
        m.data[0] = static_cast<uint8_t>(op);
        m.len     = 1;
        return m;
    }

    Message MakeBegin(const uint8_t module, const uint32_t size, const uint32_t imageCrc32, const uint16_t fwVersion)
    {
        Message m(Id(kNodeId, Endpoint::Firmware, Operation::Set));
        m.data[0]  = static_cast<uint8_t>(FirmwareOp::Begin);
        m.data[1]  = module;
        m.data[2]  = static_cast<uint8_t>(size);
        m.data[3]  = static_cast<uint8_t>(size >> 8);
        m.data[4]  = static_cast<uint8_t>(size >> 16);
        m.data[5]  = static_cast<uint8_t>(size >> 24);
        m.data[6]  = static_cast<uint8_t>(imageCrc32);
        m.data[7]  = static_cast<uint8_t>(imageCrc32 >> 8);
        m.data[8]  = static_cast<uint8_t>(imageCrc32 >> 16);
        m.data[9]  = static_cast<uint8_t>(imageCrc32 >> 24);
        m.data[10] = static_cast<uint8_t>(fwVersion);
        m.data[11] = static_cast<uint8_t>(fwVersion >> 8);
        m.len      = 12;
        return m;
    }

    Message MakeWrite(const uint16_t offset, const uint8_t* const chunk, const uint8_t chunkLen)
    {
        Message m(Id(kNodeId, Endpoint::Firmware, Operation::Set));
        m.data[0] = static_cast<uint8_t>(FirmwareOp::Write);
        m.data[1] = static_cast<uint8_t>(offset);
        m.data[2] = static_cast<uint8_t>(offset >> 8);
        for (uint8_t i = 0; i < chunkLen; i++)
        {
            m.data[3 + i] = chunk[i];
        }
        m.len = static_cast<uint8_t>(3 + chunkLen);
        return m;
    }

    void Poll(FirmwareSlave& slave)
    {
        InjectOtaFrame(Message(kNodeId, Operation::Poll));
        slave.Loop();
    }

    uint32_t ReadU32(const uint8_t* const p)
    {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
            (static_cast<uint32_t>(p[3]) << 24);
    }

    uint16_t ChunkCrc16(const uint8_t* const data, const uint8_t len)
    {
        Hal::Crc crc(Hal::Crc::Poly::Ccitt16);
        return crc.Compute(data, len);
    }

    bool FindMessage(Message* const tx, const int n, const Endpoint endpoint, const Operation op, int* const outIndex)
    {
        for (int i = 0; i < n; i++)
        {
            if (tx[i].id.endpoint == endpoint && tx[i].id.operation == op)
            {
                if (outIndex)
                {
                    *outIndex = i;
                }
                return true;
            }
        }
        return false;
    }

    // 228 is HandleBegin's own floor (Board::Flash::AppDescriptorOffset + 32 + 4).
    const uint32_t validImageSize = 256;
} // namespace

CC_TEST(FirmwareSlave, AnnouncesOnDiscoverWithModuleAndIdleState)
{
    ResetWorld();
    FirmwareSlave slave(kNodeId, kModule);
    slave.Init();

    InjectOtaFrame(Message(NodeLib::BROADCAST_NODE, Operation::Discover));
    slave.Loop();

    Message   tx[4];
    const int n = DecodeOtaTx(tx, 4);
    int       idx;
    CC_CHECK(FindMessage(tx, n, Endpoint::Transport, Operation::Announce, &idx));
    CC_CHECK_EQ(tx[idx].data[0], kModule);
    CC_CHECK_EQ(tx[idx].data[1], 1); // State::Idle
}

CC_TEST(FirmwareSlave, IgnoresAMessageAddressedToAnotherNode)
{
    ResetWorld();
    FirmwareSlave slave(kNodeId, kModule);
    slave.Init();

    Message wrongNode(Id(kNodeId + 1, Endpoint::Firmware, Operation::Set));
    wrongNode.data[0] = static_cast<uint8_t>(FirmwareOp::Begin);
    wrongNode.len     = 1;
    InjectOtaFrame(wrongNode);
    Poll(slave);

    Message   tx[4];
    const int n = DecodeOtaTx(tx, 4);
    CC_CHECK(!FindMessage(tx, n, Endpoint::Firmware, Operation::Report, nullptr)); // no status queued -- never processed
}

CC_TEST(FirmwareSlave, BeginRejectsAMismatchedModuleWithoutTouchingFlash)
{
    ResetWorld();
    FirmwareSlave slave(kNodeId, kModule);
    slave.Init();

    InjectOtaFrame(MakeBegin(kModule + 1, validImageSize, 0, 0));
    Poll(slave);

    Message   tx[4];
    const int n = DecodeOtaTx(tx, 4);
    int       idx;
    CC_CHECK(FindMessage(tx, n, Endpoint::Firmware, Operation::Report, &idx));
    CC_CHECK_EQ(tx[idx].data[1], 5); // State::Error
    CC_CHECK_EQ(tx[idx].data[6], 1); // ErrWrongModule
}

CC_TEST(FirmwareSlave, BeginRejectsAnImageSmallerThanTheDescriptorFloor)
{
    ResetWorld();
    FirmwareSlave slave(kNodeId, kModule);
    slave.Init();

    InjectOtaFrame(MakeBegin(kModule, 40, 0, 0)); // well under the 228-byte floor
    Poll(slave);

    Message   tx[4];
    const int n = DecodeOtaTx(tx, 4);
    int       idx;
    CC_CHECK(FindMessage(tx, n, Endpoint::Firmware, Operation::Report, &idx));
    CC_CHECK_EQ(tx[idx].data[6], 2); // ErrBadSize
}

CC_TEST(FirmwareSlave, BeginErasesTheSlotAndMovesToReceiving)
{
    ResetWorld();
    FirmwareSlave slave(kNodeId, kModule);
    slave.Init();

    InjectOtaFrame(MakeBegin(kModule, validImageSize, 0xDEADBEEF, 0x0102));
    Poll(slave);

    Message   tx[4];
    const int n = DecodeOtaTx(tx, 4);
    int       idx;
    CC_CHECK(FindMessage(tx, n, Endpoint::Firmware, Operation::Report, &idx));
    CC_CHECK_EQ(tx[idx].data[1], 3); // State::Receiving
    CC_CHECK_EQ(tx[idx].data[6], 0); // ErrNone
    CC_CHECK_EQ(ReadU32(&tx[idx].data[2]), 0); // expectedOffset

    CC_CHECK_EQ(FakeFlash::Data()[0], 0xFF); // slot actually erased
}

CC_TEST(FirmwareSlave, WriteProgramsFlashAndAcksWithTheChunksCrc)
{
    ResetWorld();
    FirmwareSlave slave(kNodeId, kModule);
    slave.Init();
    InjectOtaFrame(MakeBegin(kModule, validImageSize, 0, 0));
    Poll(slave);
    FakeOtaUart::Reset();

    const uint8_t chunk[8] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};
    InjectOtaFrame(MakeWrite(0, chunk, sizeof(chunk)));
    Poll(slave);

    Message   tx[4];
    const int n = DecodeOtaTx(tx, 4);
    int       idx;
    CC_CHECK(FindMessage(tx, n, Endpoint::Firmware, Operation::Ack, &idx));
    CC_CHECK_EQ(static_cast<uint16_t>(tx[idx].data[0] | (tx[idx].data[1] << 8)), 0); // offset
    CC_CHECK_EQ(
        static_cast<uint16_t>(tx[idx].data[2] | (tx[idx].data[3] << 8)), ChunkCrc16(chunk, sizeof(chunk)));
    CC_CHECK_EQ(tx[idx].data[4], 0); // programFailed

    for (uint8_t i = 0; i < sizeof(chunk); i++)
    {
        CC_CHECK_EQ(FakeFlash::Data()[i], chunk[i]); // actually landed in "flash"
    }
}

CC_TEST(FirmwareSlave, WriteAheadOfExpectedOffsetIsNackedWithTheRealOffset)
{
    ResetWorld();
    FirmwareSlave slave(kNodeId, kModule);
    slave.Init();
    InjectOtaFrame(MakeBegin(kModule, validImageSize, 0, 0));
    Poll(slave);
    FakeOtaUart::Reset();

    const uint8_t chunk[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    InjectOtaFrame(MakeWrite(16, chunk, sizeof(chunk))); // expects 0, not 16 -- a gap
    Poll(slave);

    Message   tx[4];
    const int n = DecodeOtaTx(tx, 4);
    int       idx;
    CC_CHECK(FindMessage(tx, n, Endpoint::Firmware, Operation::Nack, &idx));
    CC_CHECK_EQ(static_cast<uint16_t>(tx[idx].data[0] | (tx[idx].data[1] << 8)), 0); // names the actual expected offset
}

CC_TEST(FirmwareSlave, DuplicateWriteBelowExpectedOffsetIsAckedFromFlashNotReprogrammed)
{
    ResetWorld();
    FirmwareSlave slave(kNodeId, kModule);
    slave.Init();
    InjectOtaFrame(MakeBegin(kModule, validImageSize, 0, 0));
    Poll(slave);
    FakeOtaUart::Reset();

    const uint8_t chunk[8] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22};
    InjectOtaFrame(MakeWrite(0, chunk, sizeof(chunk)));
    Poll(slave); // expectedOffset now 8
    FakeOtaUart::Reset();

    InjectOtaFrame(MakeWrite(0, chunk, sizeof(chunk))); // resend -- master never saw the first Ack
    Poll(slave);

    Message   tx[4];
    const int n = DecodeOtaTx(tx, 4);
    int       idx;
    CC_CHECK(FindMessage(tx, n, Endpoint::Firmware, Operation::Ack, &idx));
    CC_CHECK_EQ(static_cast<uint16_t>(tx[idx].data[0] | (tx[idx].data[1] << 8)), 0);
    CC_CHECK_EQ(
        static_cast<uint16_t>(tx[idx].data[2] | (tx[idx].data[3] << 8)), ChunkCrc16(chunk, sizeof(chunk)));
}

CC_TEST(FirmwareSlave, PartialProgramFailureReportsProgressAndRetrySucceeds)
{
    ResetWorld();
    FirmwareSlave slave(kNodeId, kModule);
    slave.Init();
    InjectOtaFrame(MakeBegin(kModule, validImageSize, 0, 0));
    Poll(slave);
    FakeOtaUart::Reset();

    const uint8_t chunk[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    FakeFlash::SetNextProgramLimit(8); // simulate a hardware Program() failure after the first double-word
    InjectOtaFrame(MakeWrite(0, chunk, sizeof(chunk)));
    Poll(slave);

    Message tx[4];
    int     n = DecodeOtaTx(tx, 4);
    int     idx;
    CC_CHECK(FindMessage(tx, n, Endpoint::Firmware, Operation::Nack, &idx));
    CC_CHECK_EQ(tx[idx].data[4], 1); // programFailed
    CC_CHECK_EQ(FakeFlash::Data()[0], 1); // the first 8 bytes did land
    CC_CHECK_EQ(FakeFlash::Data()[8], 0xFF); // the rest didn't

    // Retry the same offset, this time uncapped -- should resume from byte 8,
    // not re-program the first double-word (FakeFlash doesn't model PROGERR,
    // but a correct retry writes disjoint ranges either way).
    FakeOtaUart::Reset();
    InjectOtaFrame(MakeWrite(0, chunk, sizeof(chunk)));
    Poll(slave);

    n = DecodeOtaTx(tx, 4);
    CC_CHECK(FindMessage(tx, n, Endpoint::Firmware, Operation::Ack, &idx));
    CC_CHECK_EQ(tx[idx].data[4], 0);
    for (uint8_t i = 0; i < sizeof(chunk); i++)
    {
        CC_CHECK_EQ(FakeFlash::Data()[i], chunk[i]);
    }
}

CC_TEST(FirmwareSlave, EndFaultsWhenNotAllBytesHaveArrivedYet)
{
    ResetWorld();
    FirmwareSlave slave(kNodeId, kModule);
    slave.Init();
    InjectOtaFrame(MakeBegin(kModule, validImageSize, 0, 0));
    Poll(slave);
    FakeOtaUart::Reset();

    // Never wrote anything -- expectedOffset (0) != imageSize (256). This
    // guard runs before HandleEnd() ever reads flash, so it's safe to test.
    InjectOtaFrame(MakeFirmwareSet(FirmwareOp::End));
    Poll(slave);

    Message   tx[4];
    const int n = DecodeOtaTx(tx, 4);
    int       idx;
    CC_CHECK(FindMessage(tx, n, Endpoint::Firmware, Operation::Report, &idx));
    CC_CHECK_EQ(tx[idx].data[1], 5); // State::Error
    CC_CHECK_EQ(tx[idx].data[6], 7); // ErrBadState
}

CC_TEST(FirmwareSlave, AbortReturnsToIdle)
{
    ResetWorld();
    FirmwareSlave slave(kNodeId, kModule);
    slave.Init();
    InjectOtaFrame(MakeBegin(kModule, validImageSize, 0, 0));
    Poll(slave);
    FakeOtaUart::Reset();

    InjectOtaFrame(MakeFirmwareSet(FirmwareOp::Abort));
    Poll(slave);

    Message   tx[4];
    const int n = DecodeOtaTx(tx, 4);
    int       idx;
    CC_CHECK(FindMessage(tx, n, Endpoint::Firmware, Operation::Report, &idx));
    CC_CHECK_EQ(tx[idx].data[1], 1); // State::Idle
    CC_CHECK_EQ(tx[idx].data[6], 0); // ErrNone
}
