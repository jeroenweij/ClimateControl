/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "EEndpoint.h"
#include "EOperation.h"
#include "LinkMaster.h"

#include "BusHelpers.h"
#include "FakeBus.h"
#include "FakeClock.h"
#include "FakeConfigStore.h"
#include "Test.h"

using NodeLib::Endpoint;
using NodeLib::Id;
using NodeLib::LinkMaster;
using NodeLib::Message;
using NodeLib::Operation;

namespace
{
    const uint32_t baud   = 250000;
    const uint8_t  peerId = 6; // ControllerNode id, shared with its Thermostat

    struct RecordingHandler : NodeLib::INodeHandler
    {
        int     received = 0;
        int     lost     = 0;
        Message last{};

        void ReceivedMessage(const Message& m) override
        {
            received++;
            last = m;
        }
        void ConnectionLost() override
        {
            lost++;
        }
    };

    void ResetWorld()
    {
        FakeBus::Reset();
        FakeClock::Reset();
        FakeConfig::Reset();
        FakeConfig::SetValid(true);
        FakeConfig::SetNodeId(peerId);
        FakeConfig::SetModule(NodeLib::ConfigStore::Module::ControllerNode);
    }

    int LastTx(Message* out, int maxOut)
    {
        const int n = bus::DecodeTx(out, maxOut);
        return n;
    }
} // namespace

CC_TEST(LinkMaster, IsAMasterAndDiscoversOnInit)
{
    ResetWorld();
    LinkMaster       link(baud);
    RecordingHandler handler;
    link.RegisterHandler(&handler);
    link.Init();

    CC_CHECK_EQ(link.GetId(), 0);
    CC_CHECK_EQ(link.PeerId(), peerId);

    Message   tx[4];
    const int n = LastTx(tx, 4);
    CC_CHECK_EQ(n, 1);
    CC_CHECK(tx[0].id.operation == Operation::Discover);
}

CC_TEST(LinkMaster, PollsThePeerOnTheInterval)
{
    ResetWorld();
    LinkMaster link(baud);
    link.Init();

    FakeBus::Reset();
    link.Loop(); // interval not elapsed yet
    Message tx[4];
    CC_CHECK_EQ(LastTx(tx, 4), 0);

    FakeClock::Advance(250);
    link.Loop();
    CC_CHECK_EQ(LastTx(tx, 4), 1);
    CC_CHECK_EQ(tx[0].id.node, peerId);
    CC_CHECK(tx[0].id.operation == Operation::Poll);
}

CC_TEST(LinkMaster, ForwardsAPeerReportToTheHandler)
{
    ResetWorld();
    LinkMaster       link(baud);
    RecordingHandler handler;
    link.RegisterHandler(&handler);
    link.Init();

    Message report(Id(peerId, Endpoint::RoomTemp, Operation::Report));
    report.data[0] = 0x2C; // 21.00 degC = 2100 = 0x0834
    report.data[1] = 0x08;
    report.len     = 2;
    bus::InjectFrame(report);
    link.Loop();

    CC_CHECK_EQ(handler.received, 1);
    CC_CHECK(handler.last.id.endpoint == Endpoint::RoomTemp);
    CC_CHECK(link.LinkUp());
}

CC_TEST(LinkMaster, LearnsBootloaderStateFromAnnounce)
{
    ResetWorld();
    LinkMaster link(baud);
    link.Init();

    Message announce(peerId, Operation::Announce);
    announce.data[0] = 4; // module = Thermostat
    announce.data[1] = 2; // bootloader state (non-zero)
    announce.len     = 2;
    bus::InjectFrame(announce);
    link.Loop();

    CC_CHECK(link.PeerInBootloader());
}

CC_TEST(LinkMaster, DropsTheLinkAfterMissedPolls)
{
    ResetWorld();
    LinkMaster       link(baud);
    RecordingHandler handler;
    link.RegisterHandler(&handler);
    link.Init();

    bus::InjectFrame(Message(peerId, Operation::Done));
    link.Loop();
    CC_CHECK(link.LinkUp());

    // No further peer traffic -> link times out.
    FakeClock::Advance(1000);
    link.Loop();

    CC_CHECK(!link.LinkUp());
    CC_CHECK_EQ(handler.lost, 1);
}

CC_TEST(LinkMaster, InjectedSetReachesThePeerOnTheNextPoll)
{
    ResetWorld();
    LinkMaster link(baud);
    link.Init();

    const uint8_t open = 100;
    link.SendToPeer(Endpoint::DamperActual, Operation::Set, &open, 1);

    FakeBus::Reset();
    FakeClock::Advance(250);
    link.Loop();

    Message   tx[4];
    const int n = LastTx(tx, 4);
    CC_CHECK(n >= 1);
    bool sawSet = false;
    for (int i = 0; i < n; i++)
    {
        if (tx[i].id.endpoint == Endpoint::DamperActual && tx[i].id.operation == Operation::Set)
        {
            sawSet = true;
            CC_CHECK_EQ(tx[i].id.node, peerId);
            CC_CHECK_EQ(tx[i].data[0], open);
        }
    }
    CC_CHECK(sawSet);
}
