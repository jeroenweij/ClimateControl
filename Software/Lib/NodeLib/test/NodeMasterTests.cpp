/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "NodeMaster.h"

#include "BusHelpers.h"
#include "FakeBus.h"
#include "FakeClock.h"
#include "FakeConfigStore.h"
#include "Test.h"

using NodeLib::Endpoint;
using NodeLib::Message;
using NodeLib::NodeMaster;
using NodeLib::Operation;

namespace
{
    // DetectNodes()'s discovery window is nodeSpacing(10) * (maxNodes(21)+1) =
    // 220ms (Node.h / Id.h::MAX_NODES) -- advancing past it lets the
    // Detecting state's timeoutTimer fire.
    const uint32_t discoveryWindowMs = 250;

    void ResetWorld()
    {
        FakeBus::Reset();
        FakeClock::Reset();
        FakeConfig::Reset();
    }

    // Announce from a slave: carries the module type in data[0].
    void Announce(const uint8_t nodeId, const uint8_t module)
    {
        Message m(nodeId, Operation::Announce);
        m.data[0] = module;
        m.len     = 1;
        bus::InjectFrame(m);
    }

    // Announce from a slave currently resident in the bootloader: data[1] is
    // its (always-nonzero) bootloader state, per Node.cpp's HandlePollRequest
    // / Boot::FirmwareSlave::SendAnnounce shape.
    void AnnounceFromBootloader(const uint8_t nodeId, const uint8_t module)
    {
        Message m(nodeId, Operation::Announce);
        m.data[0] = module;
        m.data[1] = 1; // bl-idle
        m.len     = 2;
        bus::InjectFrame(m);
    }

    // Drives one full round-robin lap against a single active node that never
    // replies: a Poll goes out, pollTimeoutMs(200) elapses, Loop() processes
    // the timeout and (if forgiven) sends the next Poll right back at it.
    void TimeOutOnePoll(NodeMaster& master)
    {
        FakeClock::Advance(200); // NodeMaster.h's pollTimeoutMs
        master.Loop(); // Polling -> Flush (timeout seen, inBootloader-- if set)
        master.Loop(); // Flush -> PollNextNode() (re-sends the Poll)
    }

    // DetectNodes() is async: Init() only fires the broadcast and arms
    // timeoutTimer. Advancing the clock past the window and running Loop()
    // twice -- once for Detecting -> Flush, once for Flush -> PollNextNode()
    // -- gets a freshly Init()'d master to actually send its first Poll,
    // mirroring what the real super-loop does one tick at a time.
    void FinishDetectionAndStartPolling(NodeMaster& master)
    {
        FakeClock::Advance(discoveryWindowMs);
        master.Loop(); // Detecting -> Flush
        master.Loop(); // Flush -> PollNextNode()
    }
} // namespace

CC_TEST(NodeMaster, StartsWithTheReservedMasterAddress)
{
    ResetWorld();
    NodeMaster master;
    CC_CHECK_EQ(master.GetId(), 0);
}

CC_TEST(NodeMaster, BuildsATypedRosterFromAnnounces)
{
    ResetWorld();
    NodeMaster master;

    Announce(3, 2); // TemperatureNode
    Announce(1, 1); // ControllerNode
    master.Loop();

    CC_CHECK_EQ(master.NodeModule(3), 2);
    CC_CHECK_EQ(master.NodeModule(1), 1);
    CC_CHECK_EQ(master.NodeModule(2), 0); // never announced
    CC_CHECK_EQ(master.NodeModule(0), 0); // out of range
    CC_CHECK_EQ(master.NodeModule(200), 0); // out of range
}

CC_TEST(NodeMaster, PollsOnlyDiscoveredNodesInOrder)
{
    ResetWorld();
    NodeMaster master;
    master.Init(); // broadcasts Discover, starts the async detection window
    FakeBus::Reset(); // clear the Discover broadcast bytes before injecting replies

    Announce(2, 2);
    Announce(5, 2);

    FinishDetectionAndStartPolling(master); // polls the first active node

    Message tx[4];
    int     n = bus::DecodeTx(tx, 4);
    CC_CHECK_EQ(n, 1);
    CC_CHECK_EQ(tx[0].id.node, 2);
    CC_CHECK(tx[0].id.endpoint == Endpoint::Transport);
    CC_CHECK(tx[0].id.operation == Operation::Poll);

    // Node 2 finishes its slot -> master advances to node 5.
    FakeBus::Reset();
    bus::InjectFrame(Message(2, Operation::Done));
    master.Loop();

    n = bus::DecodeTx(tx, 4);
    CC_CHECK_EQ(n, 1);
    CC_CHECK_EQ(tx[0].id.node, 5);
    CC_CHECK(tx[0].id.operation == Operation::Poll);

    // Node 5 finishes -> wrap back to node 2.
    FakeBus::Reset();
    bus::InjectFrame(Message(5, Operation::Done));
    master.Loop();

    n = bus::DecodeTx(tx, 4);
    CC_CHECK_EQ(n, 1);
    CC_CHECK_EQ(tx[0].id.node, 2);
}

CC_TEST(NodeMaster, ForgivesBootloaderPollTimeoutsWithinTheGraceBudget)
{
    ResetWorld();
    NodeMaster master;
    master.Init();
    FakeBus::Reset();

    AnnounceFromBootloader(4, 1);
    FinishDetectionAndStartPolling(master);
    CC_CHECK_EQ(master.ActiveNodeCount(), 1);

    // Well within the 200-timeout budget: node 4 stays active, still the
    // only node being polled.
    for (int i = 0; i < 50; i++)
    {
        TimeOutOnePoll(master);
    }
    CC_CHECK_EQ(master.ActiveNodeCount(), 1);

    Message tx[4];
    int     n = bus::DecodeTx(tx, 4);
    CC_CHECK(n > 0);
    CC_CHECK_EQ(tx[n - 1].id.node, 4);
    CC_CHECK(tx[n - 1].id.operation == Operation::Poll);
}

CC_TEST(NodeMaster, DeclaresABootloaderNodeLostOnceItsGraceBudgetRunsOut)
{
    ResetWorld();
    NodeMaster master;
    master.Init();
    FakeBus::Reset();

    AnnounceFromBootloader(4, 1);
    FinishDetectionAndStartPolling(master);
    CC_CHECK_EQ(master.ActiveNodeCount(), 1);

    // The grace budget is 200 *missed-poll* events, not 200 TimeOutOnePoll()
    // calls -- this span of simulated time also crosses detectIntervalMs
    // (15s), so some calls land on the periodic re-Discover cycle instead of
    // a real poll timeout and don't consume any budget. Pump well past the
    // worst case (200 real timeouts plus however many re-Discover detours
    // that takes) and just check it eventually gives up, rather than
    // asserting a call count coupled to that unrelated cadence.
    bool declaredLost = false;
    for (int i = 0; i < 2000; i++)
    {
        TimeOutOnePoll(master);
        if (master.ActiveNodeCount() == 0)
        {
            declaredLost = true;
            break;
        }
    }
    CC_CHECK(declaredLost);
}

CC_TEST(NodeMaster, DeclaresAnOrdinaryNodeLostOnItsFirstMissedPoll)
{
    ResetWorld();
    NodeMaster master;
    master.Init();
    FakeBus::Reset();

    Announce(4, 1); // no bootloader flag -- no grace budget at all
    FinishDetectionAndStartPolling(master);
    CC_CHECK_EQ(master.ActiveNodeCount(), 1);

    FakeClock::Advance(200);
    master.Loop();
    CC_CHECK_EQ(master.ActiveNodeCount(), 0);
}

CC_TEST(NodeMaster, RosterGettersReflectAnAppNodesAnnounce)
{
    ResetWorld();
    NodeMaster master;
    master.Init();
    FakeBus::Reset();

    FakeClock::Advance(discoveryWindowMs);
    Announce(4, 2); // TemperatureNode, no bootloader flag
    master.Loop(); // Detecting -> Flush
    master.Loop(); // Flush -> PollNextNode() (processes the Announce on the way)

    CC_CHECK(master.NodeActive(4));
    CC_CHECK(!master.NodeInBootloader(4));
    CC_CHECK_EQ(master.NodeLastContactMs(4), FakeClock::Now());

    // Never announced -- reads as inactive/not-in-bootloader/never-contacted,
    // same as an out-of-range id.
    CC_CHECK(!master.NodeActive(5));
    CC_CHECK(!master.NodeInBootloader(5));
    CC_CHECK_EQ(master.NodeLastContactMs(5), 0);
    CC_CHECK(!master.NodeActive(0));
    CC_CHECK(!master.NodeActive(240));
}

CC_TEST(NodeMaster, NodeInBootloaderReflectsAnAnnouncedBootloaderState)
{
    ResetWorld();
    NodeMaster master;
    master.Init();
    FakeBus::Reset();

    AnnounceFromBootloader(4, 1);
    FinishDetectionAndStartPolling(master);

    CC_CHECK(master.NodeActive(4));
    CC_CHECK(master.NodeInBootloader(4));
}

CC_TEST(NodeMaster, NodeLastContactMsAdvancesOnASuccessfulPollDone)
{
    ResetWorld();
    NodeMaster master;
    master.Init();
    FakeBus::Reset();

    Announce(4, 1);
    FinishDetectionAndStartPolling(master); // sends the first Poll to node 4
    const uint32_t contactAtAnnounce = master.NodeLastContactMs(4);

    FakeClock::Advance(50);
    bus::InjectFrame(Message(4, Operation::Done));
    master.Loop();

    CC_CHECK(master.NodeLastContactMs(4) > contactAtAnnounce);
    CC_CHECK_EQ(master.NodeLastContactMs(4), FakeClock::Now());
}

CC_TEST(NodeMaster, IgnoresAnnounceForAnOutOfRangeNode)
{
    ResetWorld();
    NodeMaster master;
    master.Init();
    FakeBus::Reset(); // clear the Discover broadcast bytes before injecting replies

    Announce(0, 2);
    Announce(240, 2);

    FinishDetectionAndStartPolling(master);

    // No node was actually registered -> nothing to poll.
    Message tx[4];
    CC_CHECK_EQ(bus::DecodeTx(tx, 4), 0);
}
