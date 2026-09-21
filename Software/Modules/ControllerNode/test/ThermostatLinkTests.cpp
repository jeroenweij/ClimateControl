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

#include "Damper.h"
#include "ThermostatLink.h"

using NodeLib::ConfigStore;
using NodeLib::Endpoint;
using NodeLib::FirmwareOp;
using NodeLib::Id;
using NodeLib::LinkMaster;
using NodeLib::Message;
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
    const uint8_t peerId = 6;

    void ResetWorld()
    {
        FakeBus::Reset();
        FakeClock::Reset();
        FakeAdc::Reset();
        FakeConfig::Reset();
        FakeConfig::SetValid(true);
        FakeConfig::SetNodeId(peerId);
        FakeConfig::SetModule(ConfigStore::Module::ControllerNode);
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
        m.data[0] = static_cast<uint8_t>(ConfigStore::Module::Thermostat);
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

    AnnouncePeer(true); // peer's own Announce, once it's actually reset into the bootloader
    w.link.Loop(); // LinkMaster learns PeerInBootloader()
    w.thermostatLink.Loop(); // ThermostatLink::Loop() reacts to it, queues Firmware[Begin]
    Flush(w.link);

    Message   tx[4];
    const int n = bus::DecodeTx(tx, 4);
    int       idx;
    CC_CHECK(FindMessage(tx, n, Endpoint::Firmware, Operation::Set, &idx));
    CC_CHECK_EQ(tx[idx].data[0], static_cast<uint8_t>(FirmwareOp::Begin));
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
    info.data[0] = static_cast<uint8_t>(ConfigStore::Module::Thermostat);
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

CC_TEST(ThermostatLink, ConnectionLostInvalidatesRoomAndFailsAnInFlightTransfer)
{
    ResetWorld();
    World w;
    AnnouncePeer(true);
    w.link.Loop();
    w.thermostatLink.OtaBegin(4, 512, 0, 0, false); // -> Transferring

    w.thermostatLink.ConnectionLost();

    CC_CHECK(!w.thermostatLink.Room().valid);
    uint8_t status[9];
    w.thermostatLink.FillOtaStatus(status);
    CC_CHECK_EQ(status[6], static_cast<uint8_t>(NodeLib::FirmwareError::LinkDown));
}
