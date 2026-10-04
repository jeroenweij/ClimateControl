/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "EEndpoint.h"
#include "EFirmware.h"
#include "EOperation.h"

#include "BusHelpers.h"
#include "FakeAdc.h"
#include "FakeBus.h"
#include "FakeClock.h"
#include "FakeConfigStore.h"
#include "Test.h"

#include "RoomDemand.h"

#include "Damper.h"
#include "ThermostatLink.h"

using NodeLib::ConfigStore;
using NodeLib::Endpoint;
using NodeLib::FirmwareOp;
using NodeLib::Id;
using NodeLib::LinkMaster;
using NodeLib::Message;
using NodeLib::ModuleType;
using NodeLib::Operation;

// ThermostatLink's own OTA-relay state machine (ControllerNode-Thermostat-
// Link-Spec.md §5.4) -- previously only ever constructed as a collaborator
// for RoomControlLoopTests/ControllerHandlerTests, never driven through its
// own Begin/Write/End/Activate/Abort logic. Unlike those suites, this one
// deliberately DOES bring the link up (link.Init()/Loop()) -- there's only
// one live Hal::Uart instance in this test binary (the link's), so the
// two-buses-share-one-FakeBus problem that made ControllerHandlerTests avoid
// it doesn't apply here.
namespace
{
    const uint8_t cnId   = 6;
    const uint8_t peerId = NodeLib::THERMOSTAT_NODE_ID;

    void ResetWorld()
    {
        FakeBus::Reset();
        FakeClock::Reset();
        FakeAdc::Reset();
        FakeConfig::Reset();
        FakeConfig::SetValid(true);
        FakeConfig::SetNodeId(cnId);
        FakeConfig::SetModule(ModuleType::ControllerNode);
    }

    void PackI16(uint8_t* const out, const int16_t v)
    {
        out[0] = static_cast<uint8_t>(v);
        out[1] = static_cast<uint8_t>(static_cast<uint16_t>(v) >> 8);
    }

    // Marks the peer alive (any message does, via LinkMaster::NotePeerAlive())
    // and, if bootloader is true, in the bootloader.
    void AnnouncePeer(const bool bootloader)
    {
        Message m(peerId, Operation::Announce);
        m.data[0] = static_cast<uint8_t>(ModuleType::Thermostat);
        m.data[1] = bootloader ? 1 : 0;
        m.len     = 2;
        bus::InjectFrame(m);
    }

    // SendFirmwareOp()/PushSetpoint() only queue -- PollPeerNow() primes the
    // poll timer to fire on the very next Loop() (SendFirmwareOp calls it
    // itself; PushSetpoint doesn't, so that test advances the clock instead).
    void Flush(LinkMaster& link)
    {
        FakeBus::Reset();
        link.Loop();
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

    // Bundles LinkMaster + Damper + ThermostatLink wired the same way
    // Modules/ControllerNode/main.cpp does -- crucially including
    // link.RegisterHandler(&thermostatLink), easy to forget by hand (missing
    // it silently drops every Ack/Nack/Report the peer sends, since
    // LinkMaster::HandleMasterMessage() only forwards to a registered
    // handler).
    struct World
    {
        LinkMaster     link;
        Damper         damper;
        ThermostatLink thermostatLink;

        World() :
            link(),
            damper(),
            thermostatLink(link, damper)
        {
            link.RegisterHandler(&thermostatLink);
            link.Init();
        }
    };
} // namespace

CC_TEST(ThermostatLink, OtaBeginEntersBootloaderFirstWhenPeerIsInTheApp)
{
    ResetWorld();
    World w;
    AnnouncePeer(false); // outbound flush is gated on having confirmed the peer first
    w.link.Loop();

    const bool started = w.thermostatLink.OtaBegin(4, 512, 0x1234, 0x0102, false);
    CC_CHECK(started);
    Flush(w.link);

    Message   tx[4];
    const int n   = bus::DecodeTx(tx, 4);
    int       idx = -1;
    CC_CHECK(FindMessage(tx, n, Endpoint::Firmware, Operation::Set, &idx));
    CC_CHECK(idx >= 0);
    if (idx >= 0)
    {
        CC_CHECK_EQ(tx[idx].data[0], static_cast<uint8_t>(FirmwareOp::EnterBootloader));
    }
}

CC_TEST(ThermostatLink, OtaBeginGoesStraightToTransferWhenPeerAlreadyInBootloader)
{
    ResetWorld();
    World w;
    AnnouncePeer(true);
    w.link.Loop(); // learns PeerInBootloader() == true

    w.thermostatLink.OtaBegin(4, 512, 0x1234, 0x0102, false);
    Flush(w.link);

    Message   tx[4];
    const int n = bus::DecodeTx(tx, 4);
    int       idx;
    CC_CHECK(FindMessage(tx, n, Endpoint::Firmware, Operation::Set, &idx));
    CC_CHECK_EQ(tx[idx].data[0], static_cast<uint8_t>(FirmwareOp::Begin));
    CC_CHECK_EQ(tx[idx].data[1], 4); // module
}

CC_TEST(ThermostatLink, LoopAdvancesToTransferOncePeerAnnouncesBootloaderEntry)
{
    ResetWorld();
    World w;

    w.thermostatLink.OtaBegin(4, 512, 0, 0, false); // peer still in app -> EnteringBootloader
    Flush(w.link); // drain the queued EnterBootloader before the next step queues more

    AnnouncePeer(true); // the app's own Announce, sent just before it resets
    w.link.Loop();
    w.thermostatLink.Loop(); // only a hint -- asks the bootloader itself

    AnnouncePeer(true); // the bootloader's answer to that Discover
    w.link.Loop(); // LinkMaster learns PeerInBootloader()
    w.thermostatLink.Loop(); // ThermostatLink::Loop() reacts to it, queues Firmware[Begin]
    Flush(w.link);

    Message   tx[4];
    const int n = bus::DecodeTx(tx, 4);
    int       idx;
    CC_CHECK(FindMessage(tx, n, Endpoint::Firmware, Operation::Set, &idx));
    CC_CHECK_EQ(tx[idx].data[0], static_cast<uint8_t>(FirmwareOp::Begin));
}

CC_TEST(ThermostatLink, EnteringBootloaderRediscoversWellBeforeTheTimeout)
{
    ResetWorld();
    World w;
    AnnouncePeer(false);
    w.link.Loop();

    w.thermostatLink.OtaBegin(4, 512, 0, 0, false);
    Flush(w.link); // EnterBootloader goes out; the peer resets

    // The real bootloader Announces only in reply to a Discover -- the CN must
    // ask, and well inside enterBlTimeoutMs rather than on the 5 s periodic one.
    FakeBus::Reset();
    FakeClock::Advance(501);
    w.thermostatLink.Loop();

    Message   tx[4];
    const int n = bus::DecodeTx(tx, 4);
    CC_CHECK(FindMessage(tx, n, Endpoint::Transport, Operation::Discover, nullptr));

    AnnouncePeer(true); // the bootloader's answer to that Discover -- taken as the hint
    w.link.Loop();
    w.thermostatLink.Loop(); // asks once more
    AnnouncePeer(true); // and the bootloader answers again
    w.link.Loop();
    w.thermostatLink.Loop(); // queues Firmware[Begin]
    FakeBus::Reset();
    bus::InjectFrame(Message(peerId, Operation::Done)); // answers the poll that went out meanwhile
    w.link.Loop();

    const int n2  = bus::DecodeTx(tx, 4);
    int       idx = 0;
    CC_CHECK(FindMessage(tx, n2, Endpoint::Firmware, Operation::Set, &idx));
    CC_CHECK_EQ(tx[idx].data[0], static_cast<uint8_t>(FirmwareOp::Begin));
}

CC_TEST(ThermostatLink, TheAppsAnnounceBeforeItsResetDoesNotSendBegin)
{
    ResetWorld();
    World w;
    AnnouncePeer(false);
    w.link.Loop();

    w.thermostatLink.OtaBegin(4, 512, 0, 0, false);
    Flush(w.link); // EnterBootloader goes out

    // The app's reply: Ack + an Announce already claiming the bootloader +
    // Done, then it resets. A Begin sent on that Announce would land
    // mid-reset and be lost -- the bench failure "did not enter bootloader /
    // begin" with the Thermostat sitting idle in its bootloader.
    FakeBus::Reset();
    AnnouncePeer(true);
    bus::InjectFrame(Message(peerId, Operation::Done));
    w.link.Loop();
    w.thermostatLink.Loop();
    w.link.Loop();

    Message   tx[8];
    const int n = bus::DecodeTx(tx, 8);
    CC_CHECK(!FindMessage(tx, n, Endpoint::Firmware, Operation::Set, nullptr)); // no Begin yet
    CC_CHECK(FindMessage(tx, n, Endpoint::Transport, Operation::Discover, nullptr)); // asks the bootloader instead
    CC_CHECK(!w.link.PeerInBootloader());
}

CC_TEST(ThermostatLink, LoopFaultsIfThePeerNeverEntersTheBootloaderInTime)
{
    ResetWorld();
    World w;

    w.thermostatLink.OtaBegin(4, 512, 0, 0, false);
    FakeClock::Advance(4001); // past enterBlTimeoutMs (4000)
    w.thermostatLink.Loop();

    uint8_t status[9];
    w.thermostatLink.FillOtaStatus(status);
    CC_CHECK_EQ(status[6], static_cast<uint8_t>(NodeLib::FirmwareError::BadState));
}

CC_TEST(ThermostatLink, OtaBeginSkipsAnAlreadyCurrentVersionUnlessForced)
{
    ResetWorld();
    World w;

    // Seed thermostatFw via the peer's own SystemInfo Report -- ReceivedMessage()
    // is a pure cache update, no need to go through a live poll for this.
    Message info(Id(peerId, Endpoint::SystemInfo, Operation::Report));
    info.data[0] = static_cast<uint8_t>(ModuleType::Thermostat);
    info.data[1] = 0; // hwRev
    info.data[2] = 1; // fwVersionMajor lo
    info.data[3] = 0;
    info.data[4] = 2; // fwVersionMinor lo
    info.data[5] = 0;
    info.len     = 6;
    w.thermostatLink.ReceivedMessage(info);
    CC_CHECK_EQ(w.thermostatLink.ThermostatFwVersion(), 0x0102);

    CC_CHECK(!w.thermostatLink.OtaBegin(4, 512, 0, 0x0102, false));
    uint8_t status[9];
    w.thermostatLink.FillOtaStatus(status);
    CC_CHECK_EQ(status[6], static_cast<uint8_t>(NodeLib::FirmwareError::AlreadyCurrent));

    CC_CHECK(w.thermostatLink.OtaBegin(4, 512, 0, 0x0102, true)); // Force bypasses the guard
}

CC_TEST(ThermostatLink, OtaWriteIsIgnoredUnlessTransferring)
{
    ResetWorld();
    World w;

    const uint8_t chunk[4] = {1, 2, 3, 4};
    w.thermostatLink.OtaWrite(0, chunk, sizeof(chunk)); // otaState == Idle -- must be a no-op
    Flush(w.link);

    Message   tx[4];
    const int n = bus::DecodeTx(tx, 4);
    CC_CHECK(!FindMessage(tx, n, Endpoint::Firmware, Operation::Set, nullptr));
}

CC_TEST(ThermostatLink, OtaWriteSendsTheChunkWhileTransferring)
{
    ResetWorld();
    World w;
    AnnouncePeer(true); // already in the bootloader -> Begin goes straight to Transferring
    w.link.Loop();
    w.thermostatLink.OtaBegin(4, 512, 0, 0, false);
    Flush(w.link); // drain the queued Begin before Write queues its own message

    const uint8_t chunk[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    w.thermostatLink.OtaWrite(16, chunk, sizeof(chunk));
    Flush(w.link);

    Message   tx[4];
    const int n = bus::DecodeTx(tx, 4);
    int       idx;
    CC_CHECK(FindMessage(tx, n, Endpoint::Firmware, Operation::Set, &idx));
    CC_CHECK_EQ(tx[idx].data[0], static_cast<uint8_t>(FirmwareOp::Write));
    CC_CHECK_EQ(static_cast<uint16_t>(tx[idx].data[1] | (tx[idx].data[2] << 8)), 16);
    CC_CHECK_EQ(tx[idx].data[3], 0xAA);
    CC_CHECK_EQ(tx[idx].data[6], 0xDD);
}

CC_TEST(ThermostatLink, OtaWriteRelaysAFull32ByteChunkIntact)
{
    ResetWorld();
    World w;
    AnnouncePeer(true);
    w.link.Loop();
    w.thermostatLink.OtaBegin(4, 512, 0, 0, false);
    Flush(w.link);

    uint8_t chunk[32];
    for (uint8_t i = 0; i < sizeof(chunk); i++)
    {
        chunk[i] = static_cast<uint8_t>(0x40 + i);
    }
    w.thermostatLink.OtaWrite(64, chunk, sizeof(chunk));
    Flush(w.link);

    Message   tx[4];
    const int n   = bus::DecodeTx(tx, 4);
    int       idx = 0;
    CC_CHECK(FindMessage(tx, n, Endpoint::Firmware, Operation::Set, &idx));
    CC_CHECK_EQ(tx[idx].len, 1 + 2 + 32); // op + byteOffset + data
    CC_CHECK_EQ(tx[idx].data[3], 0x40);
    CC_CHECK_EQ(tx[idx].data[3 + 31], 0x40 + 31); // last byte of the chunk
}

CC_TEST(ThermostatLink, ConsumeWriteReplyDeliversAnAckAndThenGoesEmpty)
{
    ResetWorld();
    World w;
    AnnouncePeer(true);
    w.link.Loop();
    w.thermostatLink.OtaBegin(4, 512, 0, 0, false);
    const uint8_t chunk[4] = {1, 2, 3, 4};
    w.thermostatLink.OtaWrite(0, chunk, sizeof(chunk));

    Message ack(Id(peerId, Endpoint::Firmware, Operation::Ack));
    PackI16(&ack.data[0], 0); // offset
    PackI16(&ack.data[2], 0xBEEF); // chunkCrc16
    ack.data[4] = 0; // programFailed
    ack.len     = 5;
    bus::InjectFrame(ack);
    w.link.Loop();

    bool     nack;
    uint16_t offset, crc16;
    bool     programFailed;
    CC_CHECK(w.thermostatLink.ConsumeWriteReply(nack, offset, crc16, programFailed));
    CC_CHECK(!nack);
    CC_CHECK_EQ(offset, 0);
    CC_CHECK_EQ(crc16, 0xBEEF);
    CC_CHECK(!programFailed);

    // Already drained -- a second call finds nothing new pending.
    CC_CHECK(!w.thermostatLink.ConsumeWriteReply(nack, offset, crc16, programFailed));
}

CC_TEST(ThermostatLink, ConsumeWriteReplyDeliversANackAndReportsProgramFailed)
{
    ResetWorld();
    World w;
    AnnouncePeer(true);
    w.link.Loop();
    w.thermostatLink.OtaBegin(4, 512, 0, 0, false);
    const uint8_t chunk[4] = {1, 2, 3, 4};
    w.thermostatLink.OtaWrite(8, chunk, sizeof(chunk));

    Message nackMsg(Id(peerId, Endpoint::Firmware, Operation::Nack));
    PackI16(&nackMsg.data[0], 8);
    PackI16(&nackMsg.data[2], 0);
    nackMsg.data[4] = 1; // programFailed
    nackMsg.len     = 5;
    bus::InjectFrame(nackMsg);
    w.link.Loop();

    bool     nack;
    uint16_t offset, crc16;
    bool     programFailed;
    CC_CHECK(w.thermostatLink.ConsumeWriteReply(nack, offset, crc16, programFailed));
    CC_CHECK(nack);
    CC_CHECK(programFailed);
}

CC_TEST(ThermostatLink, OtaEndOnlySendsWhileTransferring)
{
    ResetWorld();
    World w;

    w.thermostatLink.OtaEnd(); // otaState == Idle -- no-op
    Flush(w.link);
    Message tx[4];
    int     n = bus::DecodeTx(tx, 4);
    CC_CHECK(!FindMessage(tx, n, Endpoint::Firmware, Operation::Set, nullptr));

    AnnouncePeer(true);
    w.link.Loop();
    w.thermostatLink.OtaBegin(4, 512, 0, 0, false);
    Flush(w.link); // drain the queued Begin before End queues its own message
    w.thermostatLink.OtaEnd();
    Flush(w.link);
    n = bus::DecodeTx(tx, 4);
    int idx;
    CC_CHECK(FindMessage(tx, n, Endpoint::Firmware, Operation::Set, &idx));
    CC_CHECK_EQ(tx[idx].data[0], static_cast<uint8_t>(FirmwareOp::End));
}

CC_TEST(ThermostatLink, OtaActivateSendsAndMarksDone)
{
    ResetWorld();
    World w;
    AnnouncePeer(false); // outbound flush is gated on having confirmed the peer first
    w.link.Loop();

    w.thermostatLink.OtaActivate();
    Flush(w.link);

    Message   tx[4];
    const int n   = bus::DecodeTx(tx, 4);
    int       idx = -1;
    CC_CHECK(FindMessage(tx, n, Endpoint::Firmware, Operation::Set, &idx));
    if (idx >= 0)
    {
        CC_CHECK_EQ(tx[idx].data[0], static_cast<uint8_t>(FirmwareOp::Activate));
    }

    uint8_t status[9];
    w.thermostatLink.FillOtaStatus(status);
    CC_CHECK_EQ(status[1], 0); // StateApp -- OtaState::Done reports as "app", not the peer's raw state
}

CC_TEST(ThermostatLink, OtaAbortResetsToIdleAndClearsTheError)
{
    ResetWorld();
    World w;
    AnnouncePeer(false); // outbound flush is gated on having confirmed the peer first
    w.link.Loop();

    w.thermostatLink.OtaBegin(4, 512, 0, 0, false); // -> EnteringBootloader
    Flush(w.link); // drain the queued EnterBootloader before Abort queues its own message
    w.thermostatLink.OtaAbort();
    Flush(w.link);

    Message   tx[4];
    const int n   = bus::DecodeTx(tx, 4);
    int       idx = -1;
    CC_CHECK(FindMessage(tx, n, Endpoint::Firmware, Operation::Set, &idx));
    if (idx >= 0)
    {
        CC_CHECK_EQ(tx[idx].data[0], static_cast<uint8_t>(FirmwareOp::Abort));
    }

    uint8_t status[9];
    w.thermostatLink.FillOtaStatus(status);
    CC_CHECK_EQ(status[1], 0); // StateApp -- back to Idle
    CC_CHECK_EQ(status[6], static_cast<uint8_t>(NodeLib::FirmwareError::None));
}

CC_TEST(ThermostatLink, PushSetpointSendsRoomSetpointToThePeer)
{
    ResetWorld();
    World w;
    AnnouncePeer(false); // outbound flush is gated on having confirmed the peer first
    w.link.Loop();

    w.thermostatLink.PushSetpoint(2150); // 21.50C
    FakeClock::Advance(250); // no PollPeerNow() here -- goes out on the regular poll interval
    Flush(w.link);

    Message   tx[4];
    const int n   = bus::DecodeTx(tx, 4);
    int       idx = -1;
    CC_CHECK(FindMessage(tx, n, Endpoint::RoomSetpoint, Operation::Set, &idx));
    if (idx >= 0)
    {
        CC_CHECK_EQ(static_cast<int16_t>(tx[idx].data[0] | (tx[idx].data[1] << 8)), 2150);
    }
}

CC_TEST(ThermostatLink, ConnectionLostInvalidatesRoomButRidesOutAnInFlightTransfer)
{
    ResetWorld();
    World w;
    AnnouncePeer(true);
    w.link.Loop();
    w.thermostatLink.OtaBegin(4, 512, 0, 0, false); // -> Transferring

    // Begin's app-slot erase silences the bootloader for longer than the link
    // timeout -- that must not abort the transfer.
    w.thermostatLink.ConnectionLost();

    CC_CHECK(!w.thermostatLink.Room().valid);
    uint8_t status[9];
    w.thermostatLink.FillOtaStatus(status);
    CC_CHECK_EQ(status[6], static_cast<uint8_t>(NodeLib::FirmwareError::None));

    uint8_t chunk[32] = {};
    w.thermostatLink.OtaWrite(0, chunk, sizeof(chunk)); // still Transferring
    FakeBus::Reset();
    AnnouncePeer(true);
    w.link.Loop(); // link back up -- flushes Begin, then the Write
    Message   tx[4];
    const int n     = bus::DecodeTx(tx, 4);
    bool      write = false;
    for (int i = 0; i < n; i++)
    {
        write = write || (tx[i].id.endpoint == Endpoint::Firmware && tx[i].data[0] == static_cast<uint8_t>(FirmwareOp::Write));
    }
    CC_CHECK(write);
}

CC_TEST(ThermostatLink, ReportsTheBootloaderStateFromThePeersAnnounceWhileIdle)
{
    ResetWorld();
    World w;
    AnnouncePeer(false);
    w.link.Loop();
    w.thermostatLink.ConsumeStatusChange();

    uint8_t status[9];
    w.thermostatLink.FillOtaStatus(status);
    CC_CHECK_EQ(status[1], 0); // app

    AnnouncePeer(true); // e.g. stuck in the bootloader after an interrupted push
    w.link.Loop();
    CC_CHECK(w.thermostatLink.ConsumeStatusChange());
    w.thermostatLink.FillOtaStatus(status);
    CC_CHECK(status[1] != 0);
    CC_CHECK(!w.thermostatLink.ConsumeStatusChange()); // once per change
}

CC_TEST(ThermostatLink, BeginAndEndAcksAdvanceTheReportedState)
{
    ResetWorld();
    World w;
    AnnouncePeer(true);
    w.link.Loop();
    w.thermostatLink.OtaBegin(4, 512, 0, 0, false); // -> Transferring, Begin sent
    w.thermostatLink.ConsumeStatusChange();

    Message ack(Id(peerId, Endpoint::Firmware, Operation::Ack));
    ack.data[0] = 0; // lastError
    ack.len     = 1;
    w.thermostatLink.ReceivedMessage(ack);
    CC_CHECK(w.thermostatLink.ConsumeStatusChange());
    uint8_t status[9];
    w.thermostatLink.FillOtaStatus(status);
    CC_CHECK_EQ(status[1], 3); // Receiving

    w.thermostatLink.OtaEnd();
    w.thermostatLink.ReceivedMessage(ack);
    w.thermostatLink.FillOtaStatus(status);
    CC_CHECK_EQ(status[1], 4); // Valid
}

CC_TEST(ThermostatLink, ABeginNackReportsTheErrorState)
{
    ResetWorld();
    World w;
    AnnouncePeer(true);
    w.link.Loop();
    w.thermostatLink.OtaBegin(4, 512, 0, 0, false);

    Message nack(Id(peerId, Endpoint::Firmware, Operation::Nack));
    nack.data[0] = static_cast<uint8_t>(NodeLib::FirmwareError::WrongModule);
    nack.len     = 1;
    w.thermostatLink.ReceivedMessage(nack);

    uint8_t status[9];
    w.thermostatLink.FillOtaStatus(status);
    CC_CHECK_EQ(status[1], 5); // Error
    CC_CHECK_EQ(status[6], static_cast<uint8_t>(NodeLib::FirmwareError::WrongModule));
}

namespace
{
    void ReportToLink(ThermostatLink& link, const Endpoint endpoint, const int16_t value)
    {
        Message m(Id(peerId, endpoint, Operation::Report));
        PackI16(m.data, value);
        m.len = 2;
        link.ReceivedMessage(m);
    }
} // namespace

CC_TEST(ThermostatLink, ARoomTemperatureThatStopsArrivingIsDropped)
{
    ResetWorld();
    World w;
    ReportToLink(w.thermostatLink, Endpoint::RoomSetpoint, 2100);
    ReportToLink(w.thermostatLink, Endpoint::RoomTemp, 2050);
    CC_CHECK(w.thermostatLink.Room().valid);

    FakeClock::Advance(NodeLib::RoomTempStaleMs - 1000); // still within the window
    w.thermostatLink.Loop();
    CC_CHECK(w.thermostatLink.Room().valid);

    FakeClock::Advance(1001); // three keepalives missed: the sensor is gone
    w.thermostatLink.Loop();
    CC_CHECK(!w.thermostatLink.Room().valid);
    CC_CHECK(!w.thermostatLink.Room().tempValid);
    CC_CHECK(w.thermostatLink.Room().setpointValid); // the setpoint is still real

    ReportToLink(w.thermostatLink, Endpoint::RoomTemp, 2060); // the sensor is back
    CC_CHECK(w.thermostatLink.Room().valid);
}

CC_TEST(ThermostatLink, ASetpointAloneIsNotAValidRoom)
{
    ResetWorld();
    World w;
    ReportToLink(w.thermostatLink, Endpoint::RoomSetpoint, 2100); // a Thermostat without a sensor
    CC_CHECK(!w.thermostatLink.Room().valid);
    CC_CHECK(w.thermostatLink.Room().setpointValid);
}

CC_TEST(ThermostatLink, AsksTheThermostatForItsVersionUntilItAnswers)
{
    ResetWorld();
    World w;
    AnnouncePeer(false); // the app is up on the link
    w.link.Loop();

    FakeBus::Reset();
    w.thermostatLink.Loop(); // queues Get SystemInfo
    bus::InjectFrame(Message(peerId, Operation::Done));
    w.link.Loop();
    FakeClock::Advance(200); // next poll carries it
    w.link.Loop();

    Message   tx[8];
    const int n = bus::DecodeTx(tx, 8);
    CC_CHECK(FindMessage(tx, n, Endpoint::SystemInfo, Operation::Get, nullptr));

    Message info(Id(peerId, Endpoint::SystemInfo, Operation::Report));
    info.data[0] = static_cast<uint8_t>(ModuleType::Thermostat);
    info.data[1] = 1; // hwRev
    info.data[2] = 0; // fwMajor 0
    info.data[3] = 0;
    info.data[4] = 16; // fwMinor 16
    info.data[5] = 0;
    info.len     = 6;
    w.thermostatLink.ReceivedMessage(info);
    CC_CHECK_EQ(w.thermostatLink.ThermostatFwVersion(), 16);

    FakeBus::Reset();
    FakeClock::Advance(10000); // well past the retry interval: known now, no more asking
    w.thermostatLink.Loop();
    bus::InjectFrame(Message(peerId, Operation::Done));
    w.link.Loop();
    FakeClock::Advance(200);
    w.link.Loop();
    const int n2 = bus::DecodeTx(tx, 8);
    CC_CHECK(!FindMessage(tx, n2, Endpoint::SystemInfo, Operation::Get, nullptr));
}
