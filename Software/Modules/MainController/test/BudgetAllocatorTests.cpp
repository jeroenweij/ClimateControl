/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "EEndpoint.h"
#include "EOperation.h"
#include "NodeMaster.h"

#include "BusHelpers.h"
#include "FakeBus.h"
#include "FakeClock.h"
#include "FakeConfigStore.h"
#include "Test.h"

#include "BudgetAllocator.h"

using NodeLib::ConfigStore;
using NodeLib::Endpoint;
using NodeLib::Id;
using NodeLib::MAX_NODES;
using NodeLib::Message;
using NodeLib::ModuleType;
using NodeLib::NodeMaster;
using NodeLib::Operation;

namespace
{
    // Same shape as NodeMaster's own detection window (NodeMasterTests.cpp).
    const uint32_t discoveryWindowMs = 250;

    // SupplyTemp comes from the duct TemperatureNode -- a slave id like any
    // other, never the master's 0. Observe() doesn't check the sender's
    // module, so it needn't be announced.
    const uint8_t supplyNodeId = 5;

    void ResetWorld()
    {
        FakeBus::Reset();
        FakeClock::Reset();
        FakeConfig::Reset();
    }

    void Announce(const uint8_t nodeId, const ModuleType module)
    {
        Message m(nodeId, Operation::Announce);
        m.data[0] = static_cast<uint8_t>(module);
        m.len     = 1;
        bus::InjectFrame(m);
    }

    // Announce from a ControllerNode resident in its bootloader: data[1] is the
    // (always-nonzero) bootloader state, as in NodeMasterTests.cpp.
    void AnnounceFromBootloader(const uint8_t nodeId)
    {
        Message m(nodeId, Operation::Announce);
        m.data[0] = static_cast<uint8_t>(ModuleType::ControllerNode);
        m.data[1] = 1; // bl-idle
        m.len     = 2;
        bus::InjectFrame(m);
    }

    // The node being polled never answers: after NodeMaster.h's pollTimeoutMs
    // it is declared lost (an app node gets no grace) and the master moves on
    // to Flush.
    void MissPoll(NodeMaster& master)
    {
        master.Loop(); // make sure the Poll is out -- StartPolling() may leave it in Flush
        FakeClock::Advance(200);
        master.Loop();
    }

    void PackI16(uint8_t* const out, const int16_t v)
    {
        out[0] = static_cast<uint8_t>(v);
        out[1] = static_cast<uint8_t>(static_cast<uint16_t>(v) >> 8);
    }

    void ObserveReport(BudgetAllocator& allocator, const uint8_t nodeId, const Endpoint endpoint, const int16_t value)
    {
        Message m(Id(nodeId, endpoint, Operation::Report));
        PackI16(m.data, value);
        m.len = 2;
        allocator.Observe(m);
    }

    // master.Init() broadcasts Discover immediately -- reset the (fake) bus
    // right after so replies can be injected cleanly, same order as
    // NodeMasterTests.cpp. FakeBus::Reset() clears RX too, so Announce()
    // calls must come AFTER this, not before.
    void InitAndClearDiscover(NodeMaster& master)
    {
        master.Init();
        FakeBus::Reset();
    }

    // Brings the master through Detect -> first Poll, mirroring
    // NodeMasterTests.cpp's FinishDetectionAndStartPolling. Call after
    // InitAndClearDiscover() + the test's Announce() calls.
    void StartPolling(NodeMaster& master)
    {
        FakeClock::Advance(discoveryWindowMs);
        master.Loop(); // Detecting -> Flush -> (queue empty so far) -> Polling, first node
    }

    // Reaches the master's next Flush state, where any messages
    // BudgetAllocator has queued via master.QueueMessage() actually go out
    // (Node-Message-Model-Spec.md §6.1 -- queued master Sets go out "in the
    // gaps", not the instant they're queued).
    void FlushQueuedBudgets(NodeMaster& master, const uint8_t pendingPollNode)
    {
        FakeBus::Reset();
        bus::InjectFrame(Message(pendingPollNode, Operation::Done));
        master.Loop();
    }

    bool FindBudget(Message* const tx, const int n, const uint8_t nodeId, uint8_t& percent)
    {
        for (int i = 0; i < n; i++)
        {
            if (tx[i].id.node == nodeId && tx[i].id.endpoint == Endpoint::DamperBudget &&
                tx[i].id.operation == Operation::Set && tx[i].len >= 1)
            {
                percent = tx[i].data[0];
                return true;
            }
        }
        return false;
    }

    // Every DamperBudget Set to nodeId, in the order sent -- for tests that
    // queue several recompute rounds and flush them together.
    int FindBudgets(Message* const tx, const int n, const uint8_t nodeId, uint8_t* const out, const int max)
    {
        int count = 0;
        for (int i = 0; i < n && count < max; i++)
        {
            if (tx[i].id.node == nodeId && tx[i].id.endpoint == Endpoint::DamperBudget &&
                tx[i].id.operation == Operation::Set && tx[i].len >= 1)
            {
                out[count++] = tx[i].data[0];
            }
        }
        return count;
    }

    // The min byte of the DamperBudget Set to nodeId (0 if it carried none).
    bool FindBudgetMin(Message* const tx, const int n, const uint8_t nodeId, uint8_t& min)
    {
        for (int i = 0; i < n; i++)
        {
            if (tx[i].id.node == nodeId && tx[i].id.endpoint == Endpoint::DamperBudget &&
                tx[i].id.operation == Operation::Set)
            {
                min = tx[i].len >= 2 ? tx[i].data[1] : 0;
                return true;
            }
        }
        return false;
    }

    void ObserveByte(BudgetAllocator& allocator, const uint8_t nodeId, const Endpoint endpoint, const uint8_t value)
    {
        Message m(Id(nodeId, endpoint, Operation::Report));
        m.data[0] = value;
        m.len     = 1;
        allocator.Observe(m);
    }

    void ObserveBudget(BudgetAllocator& allocator, const uint8_t nodeId, const uint8_t percent)
    {
        Message m(Id(nodeId, Endpoint::DamperBudget, Operation::Report));
        m.data[0] = percent;
        m.len     = 1;
        allocator.Observe(m);
    }

    int CountBudgetMessages(Message* const tx, const int n)
    {
        int count = 0;
        for (int i = 0; i < n; i++)
        {
            if (tx[i].id.endpoint == Endpoint::DamperBudget && tx[i].id.operation == Operation::Set)
            {
                count++;
            }
        }
        return count;
    }
} // namespace

CC_TEST(BudgetAllocator, SendsNothingWithNoOnlineControllerNodes)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);

    allocator.Loop();

    // No node ever announced -> master never leaves EMasterState::Start ->
    // Recompute() itself also bails out (onlineCount == 0) so nothing is even
    // queued. Confirms both: nothing crashes, and no queue was silently
    // building up.
    Message tx[8];
    CC_CHECK_EQ(bus::DecodeTx(tx, 8), 0);
}

CC_TEST(BudgetAllocator, SplitsThePoolEvenlyWithNoDemandData)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);

    InitAndClearDiscover(master);
    Announce(2, ModuleType::ControllerNode);
    Announce(3, ModuleType::ControllerNode);
    StartPolling(master); // polls node 2 first (lowest id)

    allocator.Loop(); // no Observe() calls yet -> weightSum == 0 -> even split
    FlushQueuedBudgets(master, 2);

    Message   tx[8];
    const int n = bus::DecodeTx(tx, 8);
    uint8_t   p2, p3;
    CC_CHECK(FindBudget(tx, n, 2, p2));
    CC_CHECK(FindBudget(tx, n, 3, p3));
    CC_CHECK_EQ(p2, 50);
    CC_CHECK_EQ(p3, 50);
}

CC_TEST(BudgetAllocator, WeightsByEachRoomsDemand)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);

    InitAndClearDiscover(master);
    Announce(2, ModuleType::ControllerNode);
    Announce(3, ModuleType::ControllerNode);
    StartPolling(master);

    ObserveReport(allocator, supplyNodeId, Endpoint::SupplyTemp, 1500); // 15.0C, colder than either room
    ObserveReport(allocator, 2, Endpoint::RoomTemp, 2400); // 24.0C, wants cooling, saturates demand
    ObserveReport(allocator, 2, Endpoint::RoomSetpoint, 1800);
    ObserveReport(allocator, 3, Endpoint::RoomTemp, 2000); // already at setpoint -- zero demand
    ObserveReport(allocator, 3, Endpoint::RoomSetpoint, 2000);

    allocator.Loop();
    FlushQueuedBudgets(master, 2);

    Message   tx[8];
    const int n = bus::DecodeTx(tx, 8);
    uint8_t   p2, p3;
    CC_CHECK(FindBudget(tx, n, 2, p2));
    CC_CHECK(FindBudget(tx, n, 3, p3));
    CC_CHECK_EQ(p2, 100); // all of the 2-node, 100%-total pool
    CC_CHECK_EQ(p3, 0); // a max of 0 -- room 2 alone keeps enough open, so no min either (§5.5)
}

CC_TEST(BudgetAllocator, SplitRoundsToNearestRatherThanFlooring)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);

    InitAndClearDiscover(master);
    Announce(2, ModuleType::ControllerNode);
    Announce(3, ModuleType::ControllerNode);
    StartPolling(master);

    // Weights land at 1 and 2 (RoomDemandPercent's own default deadband/full-
    // authority scale) -- pool is 100 (2 nodes), so the exact split is
    // 33.33/66.67. Flooring both would give 33/66 (summing to 99, one point
    // short of the pool); rounding to nearest gives 33/67, using the whole
    // pool.
    ObserveReport(allocator, supplyNodeId, Endpoint::SupplyTemp, 1500); // 15.0C, colder than either room
    ObserveReport(allocator, 2, Endpoint::RoomTemp, 2033); // 0.33C past the 0.30C deadband -- weight 1
    ObserveReport(allocator, 2, Endpoint::RoomSetpoint, 2000);
    ObserveReport(allocator, 3, Endpoint::RoomTemp, 2035); // 0.35C past deadband -- weight 2
    ObserveReport(allocator, 3, Endpoint::RoomSetpoint, 2000);

    allocator.Loop();
    FlushQueuedBudgets(master, 2);

    Message   tx[8];
    const int n = bus::DecodeTx(tx, 8);
    uint8_t   p2, p3;
    CC_CHECK(FindBudget(tx, n, 2, p2));
    CC_CHECK(FindBudget(tx, n, 3, p3));
    CC_CHECK_EQ(p2, 33);
    CC_CHECK_EQ(p3, 67);
    CC_CHECK_EQ(static_cast<int>(p2) + p3, 100); // the whole pool actually gets used
}

CC_TEST(BudgetAllocator, WaterFillsOverflowFromAClampedNodeIntoTheRest)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);

    InitAndClearDiscover(master);
    Announce(2, ModuleType::ControllerNode);
    Announce(3, ModuleType::ControllerNode);
    Announce(4, ModuleType::ControllerNode);
    StartPolling(master);

    ObserveReport(allocator, supplyNodeId, Endpoint::SupplyTemp, 1500);
    ObserveReport(allocator, 2, Endpoint::RoomTemp, 2400); // full demand -- alone would want 150% of a 3-node pool
    ObserveReport(allocator, 2, Endpoint::RoomSetpoint, 1800);
    ObserveReport(allocator, 3, Endpoint::RoomTemp, 2000); // no demand
    ObserveReport(allocator, 3, Endpoint::RoomSetpoint, 2000);
    ObserveReport(allocator, 4, Endpoint::RoomTemp, 2000); // no demand
    ObserveReport(allocator, 4, Endpoint::RoomSetpoint, 2000);

    allocator.Loop();
    FlushQueuedBudgets(master, 2);

    Message   tx[8];
    const int n = bus::DecodeTx(tx, 8);
    uint8_t   p2, p3, p4;
    CC_CHECK(FindBudget(tx, n, 2, p2));
    CC_CHECK(FindBudget(tx, n, 3, p3));
    CC_CHECK(FindBudget(tx, n, 4, p4));
    CC_CHECK_EQ(p2, 100); // clamped -- one node can never claim more than 100%
    CC_CHECK_EQ(p3, 25); // the 50-point overflow split evenly (both zero-weight)
    CC_CHECK_EQ(p4, 25);
    CC_CHECK_EQ(static_cast<int>(p2) + p3 + p4, 150); // the full 3-node pool, nothing lost
}

CC_TEST(BudgetAllocator, IgnoresNonControllerNodeModules)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);

    InitAndClearDiscover(master);
    Announce(2, ModuleType::ControllerNode);
    Announce(7, ModuleType::TemperatureNode);
    StartPolling(master);

    allocator.Loop();
    FlushQueuedBudgets(master, 2);

    Message   tx[8];
    const int n = bus::DecodeTx(tx, 8);
    CC_CHECK_EQ(CountBudgetMessages(tx, n), 1); // only node 2 -- node 7 never gets a budget at all
    uint8_t p2;
    CC_CHECK(FindBudget(tx, n, 2, p2));
    CC_CHECK_EQ(p2, 50); // sole online node -- the whole 50%-per-node pool
}

CC_TEST(BudgetAllocator, IncludesTheHighestNodeId)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);

    InitAndClearDiscover(master);
    Announce(2, ModuleType::ControllerNode);
    Announce(MAX_NODES, ModuleType::ControllerNode);
    StartPolling(master);

    // Only the top node has demand, so it takes the whole pool -- which it
    // can only do if both its room data and its budget reach room[MAX_NODES - 1].
    ObserveReport(allocator, supplyNodeId, Endpoint::SupplyTemp, 1500);
    ObserveReport(allocator, 2, Endpoint::RoomTemp, 2000); // at setpoint -- zero demand
    ObserveReport(allocator, 2, Endpoint::RoomSetpoint, 2000);
    ObserveReport(allocator, MAX_NODES, Endpoint::RoomTemp, 2400); // wants cooling, saturates demand
    ObserveReport(allocator, MAX_NODES, Endpoint::RoomSetpoint, 1800);

    allocator.Loop();
    FlushQueuedBudgets(master, 2);

    Message   tx[8];
    const int n = bus::DecodeTx(tx, 8);
    uint8_t   p2, pTop;
    CC_CHECK(FindBudget(tx, n, 2, p2));
    CC_CHECK(FindBudget(tx, n, MAX_NODES, pTop));
    CC_CHECK_EQ(p2, 0);
    CC_CHECK_EQ(pTop, 100);
}

// Node 0 is the master itself -- nothing on the bus ever reports under it.
CC_TEST(BudgetAllocator, IgnoresSupplyTempFromTheMasterNodeId)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);

    InitAndClearDiscover(master);
    Announce(2, ModuleType::ControllerNode);
    Announce(3, ModuleType::ControllerNode);
    StartPolling(master);

    // Room demand that would give node 2 the whole pool -- if the supply
    // reading from node 0 were taken as valid.
    ObserveReport(allocator, 0, Endpoint::SupplyTemp, 1500);
    ObserveReport(allocator, 2, Endpoint::RoomTemp, 2400);
    ObserveReport(allocator, 2, Endpoint::RoomSetpoint, 1800);
    ObserveReport(allocator, 3, Endpoint::RoomTemp, 2000);
    ObserveReport(allocator, 3, Endpoint::RoomSetpoint, 2000);

    allocator.Loop();
    FlushQueuedBudgets(master, 2);

    Message   tx[8];
    const int n = bus::DecodeTx(tx, 8);
    uint8_t   p2, p3;
    CC_CHECK(FindBudget(tx, n, 2, p2));
    CC_CHECK(FindBudget(tx, n, 3, p3));
    CC_CHECK_EQ(p2, 50); // no valid supply -> no weights -> even split
    CC_CHECK_EQ(p3, 50);
}

CC_TEST(BudgetAllocator, IgnoresRoomReportsFromTheMasterNodeId)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);

    InitAndClearDiscover(master);
    Announce(1, ModuleType::ControllerNode);
    Announce(2, ModuleType::ControllerNode);
    StartPolling(master); // polls node 1 first

    // Full demand reported under node 0, none by the real rooms: node 1 has
    // no room data of its own and node 2 sits at its setpoint. Node 0's data
    // landing on any room -- node 1's slot is the one right next to it --
    // would hand that room the pool.
    ObserveReport(allocator, supplyNodeId, Endpoint::SupplyTemp, 1500);
    ObserveReport(allocator, 0, Endpoint::RoomTemp, 2400);
    ObserveReport(allocator, 0, Endpoint::RoomSetpoint, 1800);
    ObserveReport(allocator, 2, Endpoint::RoomTemp, 2000);
    ObserveReport(allocator, 2, Endpoint::RoomSetpoint, 2000);

    allocator.Loop();
    FlushQueuedBudgets(master, 1);

    Message   tx[8];
    const int n = bus::DecodeTx(tx, 8);
    uint8_t   p1, p2;
    CC_CHECK(FindBudget(tx, n, 1, p1));
    CC_CHECK(FindBudget(tx, n, 2, p2));
    CC_CHECK_EQ(p1, 50);
    CC_CHECK_EQ(p2, 50);
}

CC_TEST(BudgetAllocator, DropsALostNodeFromTheAllocation)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);

    InitAndClearDiscover(master);
    Announce(2, ModuleType::ControllerNode);
    Announce(3, ModuleType::ControllerNode);
    StartPolling(master); // polls node 2 first

    // Node 2 has all the demand -- while it counted, it would take the pool.
    ObserveReport(allocator, supplyNodeId, Endpoint::SupplyTemp, 1500);
    ObserveReport(allocator, 2, Endpoint::RoomTemp, 2400);
    ObserveReport(allocator, 2, Endpoint::RoomSetpoint, 1800);
    ObserveReport(allocator, 3, Endpoint::RoomTemp, 2000);
    ObserveReport(allocator, 3, Endpoint::RoomSetpoint, 2000);

    MissPoll(master); // node 2 lost; master now in Flush
    CC_CHECK(!master.NodeActive(2));

    allocator.Loop();
    FakeBus::Reset();
    master.Loop(); // Flush sends the queued budget

    Message   tx[8];
    const int n = bus::DecodeTx(tx, 8);
    CC_CHECK_EQ(CountBudgetMessages(tx, n), 1); // nothing for the lost node
    uint8_t p3;
    CC_CHECK(FindBudget(tx, n, 3, p3));
    CC_CHECK_EQ(p3, 50); // the pool of the one node left
}

CC_TEST(BudgetAllocator, ExcludesANodeInItsBootloader)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);

    InitAndClearDiscover(master);
    Announce(2, ModuleType::ControllerNode);
    AnnounceFromBootloader(3);
    StartPolling(master);
    CC_CHECK(master.NodeInBootloader(3));

    // Stale room data from before the node went into its bootloader.
    ObserveReport(allocator, supplyNodeId, Endpoint::SupplyTemp, 1500);
    ObserveReport(allocator, 3, Endpoint::RoomTemp, 2400);
    ObserveReport(allocator, 3, Endpoint::RoomSetpoint, 1800);

    allocator.Loop();
    FlushQueuedBudgets(master, 2);

    Message   tx[8];
    const int n = bus::DecodeTx(tx, 8);
    CC_CHECK_EQ(CountBudgetMessages(tx, n), 1);
    uint8_t p2;
    CC_CHECK(FindBudget(tx, n, 2, p2));
    CC_CHECK_EQ(p2, 50);
}

CC_TEST(BudgetAllocator, ARejoiningNodeStartsWithoutItsOldRoomData)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);

    InitAndClearDiscover(master);
    Announce(2, ModuleType::ControllerNode);
    Announce(3, ModuleType::ControllerNode);
    StartPolling(master);

    ObserveReport(allocator, supplyNodeId, Endpoint::SupplyTemp, 1500);
    ObserveReport(allocator, 2, Endpoint::RoomTemp, 2400); // full demand, then lost
    ObserveReport(allocator, 2, Endpoint::RoomSetpoint, 1800);
    ObserveReport(allocator, 3, Endpoint::RoomTemp, 2000); // no demand
    ObserveReport(allocator, 3, Endpoint::RoomSetpoint, 2000);

    MissPoll(master);
    allocator.Loop(); // node 2 doesn't count -> its room data is dropped
    FakeBus::Reset();
    master.Loop(); // Flush

    // Node 2 comes back but hasn't re-reported its room yet: no demand is
    // known for it, so nobody has any and the pool splits evenly -- instead
    // of node 2 taking all of it on its pre-loss demand.
    Announce(2, ModuleType::ControllerNode);
    master.Loop();
    CC_CHECK(master.NodeActive(2));

    FakeClock::Advance(30000); // recomputeIntervalMs
    allocator.Loop();
    FakeBus::Reset();
    for (int i = 0; i < 4; i++) // through the next poll(s) to a Flush
    {
        bus::InjectFrame(Message(2, Operation::Done));
        bus::InjectFrame(Message(3, Operation::Done));
        master.Loop();
    }

    Message   tx[16];
    const int n = bus::DecodeTx(tx, 16);
    uint8_t   p2, p3;
    CC_CHECK(FindBudget(tx, n, 2, p2));
    CC_CHECK(FindBudget(tx, n, 3, p3));
    CC_CHECK_EQ(p2, 50);
    CC_CHECK_EQ(p3, 50);
}

CC_TEST(BudgetAllocator, WithoutASupplyReadingStepsEachBudgetTowardTheDefault)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);

    InitAndClearDiscover(master);
    Announce(2, ModuleType::ControllerNode);
    Announce(3, ModuleType::ControllerNode);
    Announce(4, ModuleType::ControllerNode);
    StartPolling(master);

    // Where each node's budget stands, from its own reports; node 4 hasn't
    // reported one.
    ObserveBudget(allocator, 2, 20);
    ObserveBudget(allocator, 3, 90);

    allocator.Loop();
    FakeClock::Advance(30000); // recomputeIntervalMs
    allocator.Loop(); // no report in between: steps on from what was sent
    FlushQueuedBudgets(master, 2);

    Message   tx[16];
    const int n = bus::DecodeTx(tx, 16);
    uint8_t   b[4];
    CC_CHECK_EQ(FindBudgets(tx, n, 2, b, 4), 2);
    CC_CHECK_EQ(b[0], 21);
    CC_CHECK_EQ(b[1], 22);
    CC_CHECK_EQ(FindBudgets(tx, n, 3, b, 4), 2);
    CC_CHECK_EQ(b[0], 89);
    CC_CHECK_EQ(b[1], 88);
    CC_CHECK_EQ(FindBudgets(tx, n, 4, b, 4), 2); // unknown: starts at the default and stays
    CC_CHECK_EQ(b[0], 50);
    CC_CHECK_EQ(b[1], 50);
}

CC_TEST(BudgetAllocator, AStaleSupplyReadingStartsTheWalkBackFromTheLastAllocation)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);

    InitAndClearDiscover(master);
    Announce(2, ModuleType::ControllerNode);
    Announce(3, ModuleType::ControllerNode);
    StartPolling(master);

    ObserveReport(allocator, supplyNodeId, Endpoint::SupplyTemp, 1500);
    ObserveReport(allocator, 2, Endpoint::RoomTemp, 2400); // full demand
    ObserveReport(allocator, 2, Endpoint::RoomSetpoint, 1800);
    ObserveReport(allocator, 3, Endpoint::RoomTemp, 2000); // none
    ObserveReport(allocator, 3, Endpoint::RoomSetpoint, 2000);
    allocator.Loop(); // 100 / 0

    // The TemperatureNode goes quiet. Recomputes keep allocating from the
    // last reading until it is 5 minutes old (NodeLib::SupplyTemp's
    // staleTimeoutMs), then walk back instead of jumping to an even split.
    FakeClock::Advance(5 * 60 * 1000);
    allocator.Loop();
    FlushQueuedBudgets(master, 2);

    Message   tx[16];
    const int n = bus::DecodeTx(tx, 16);
    uint8_t   b[4];
    CC_CHECK_EQ(FindBudgets(tx, n, 2, b, 4), 2);
    CC_CHECK_EQ(b[0], 100);
    CC_CHECK_EQ(b[1], 99);
    CC_CHECK_EQ(FindBudgets(tx, n, 3, b, 4), 2);
    CC_CHECK_EQ(b[0], 0);
    CC_CHECK_EQ(b[1], 1);
}

CC_TEST(BudgetAllocator, RecomputeDebouncesWithinTheInterval)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);

    InitAndClearDiscover(master);
    Announce(2, ModuleType::ControllerNode);
    Announce(3, ModuleType::ControllerNode);
    StartPolling(master);

    allocator.Loop(); // first call recomputes immediately
    allocator.Loop(); // no clock advance -- must not recompute (or queue) again
    FlushQueuedBudgets(master, 2);

    Message   tx[8];
    const int n = bus::DecodeTx(tx, 8);
    CC_CHECK_EQ(CountBudgetMessages(tx, n), 2); // one Set per node, not two rounds' worth
}

namespace
{
    // Three online ControllerNodes (2, 3, 4), cold supply air, every room
    // exactly at setpoint -- nobody wants air, every loop would close.
    void ThreeSatisfiedRooms(NodeMaster& master, BudgetAllocator& allocator)
    {
        InitAndClearDiscover(master);
        Announce(2, ModuleType::ControllerNode);
        Announce(3, ModuleType::ControllerNode);
        Announce(4, ModuleType::ControllerNode);
        StartPolling(master);

        ObserveReport(allocator, supplyNodeId, Endpoint::SupplyTemp, 1500);
        for (uint8_t id = 2; id <= 4; id++)
        {
            ObserveReport(allocator, id, Endpoint::RoomTemp, 2000);
            ObserveReport(allocator, id, Endpoint::RoomSetpoint, 2000);
        }
    }
} // namespace

CC_TEST(BudgetAllocator, WithEveryRoomSatisfiedTheDumpRoomTakesTheAir)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);
    ThreeSatisfiedRooms(master, allocator);
    ObserveByte(allocator, 3, Endpoint::DumpRoom, 1);

    allocator.Loop();
    FlushQueuedBudgets(master, 2);

    Message   tx[8];
    const int n    = bus::DecodeTx(tx, 8);
    uint8_t   min2 = 0xFF, min3 = 0xFF, min4 = 0xFF;
    CC_CHECK(FindBudgetMin(tx, n, 2, min2));
    CC_CHECK(FindBudgetMin(tx, n, 3, min3));
    CC_CHECK(FindBudgetMin(tx, n, 4, min4));
    CC_CHECK_EQ(min3, 100); // the dump room first, fully open
    CC_CHECK_EQ(min2, 100); // the other 100 of the 200 total: rooms 2 and 4 tie, the lower id goes first
    CC_CHECK_EQ(min4, 0);
}

CC_TEST(BudgetAllocator, WhatTheDumpRoomCantTakeGoesToTheLeastHarmedRoom)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);
    ThreeSatisfiedRooms(master, allocator);
    // The dump room is held at 30% by hand (Manual): it counts, but takes no min.
    ObserveByte(allocator, 3, Endpoint::DumpRoom, 1);
    ObserveByte(allocator, 3, Endpoint::DamperMode, 3);
    ObserveByte(allocator, 3, Endpoint::DamperTarget, 30);
    // Room 4 is already below setpoint -- more cold air hurts it more than room 2.
    ObserveReport(allocator, 4, Endpoint::RoomTemp, 1950);

    allocator.Loop();
    FlushQueuedBudgets(master, 2);

    Message   tx[8];
    const int n    = bus::DecodeTx(tx, 8);
    uint8_t   min2 = 0xFF, min3 = 0xFF, min4 = 0xFF;
    CC_CHECK(FindBudgetMin(tx, n, 2, min2));
    CC_CHECK(FindBudgetMin(tx, n, 3, min3));
    CC_CHECK(FindBudgetMin(tx, n, 4, min4));
    CC_CHECK_EQ(min3, 0);
    CC_CHECK_EQ(min2, 100); // 200 total - 30 held open by hand: room 2 fills up first,
    CC_CHECK_EQ(min4, 70); // and the colder room 4 takes the remaining 70
}

CC_TEST(BudgetAllocator, NoMinimumWhileTheRoomsKeepEnoughOpenThemselves)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);

    const uint8_t ids[] = {2, 3, 4, 6}; // 5 is the supply TemperatureNode
    InitAndClearDiscover(master);
    for (const uint8_t id : ids)
    {
        Announce(id, ModuleType::ControllerNode);
    }
    StartPolling(master);

    ObserveReport(allocator, supplyNodeId, Endpoint::SupplyTemp, 1500);
    for (const uint8_t id : ids)
    {
        ObserveReport(allocator, id, Endpoint::RoomTemp, 2500); // all far too warm -- full demand
        ObserveReport(allocator, id, Endpoint::RoomSetpoint, 2000);
    }
    ObserveByte(allocator, 3, Endpoint::DumpRoom, 1);

    allocator.Loop();
    FlushQueuedBudgets(master, 2);

    Message   tx[12];
    const int n = bus::DecodeTx(tx, 12);
    for (const uint8_t id : ids)
    {
        uint8_t min = 0xFF;
        CC_CHECK(FindBudgetMin(tx, n, id, min));
        CC_CHECK_EQ(min, 0); // 4 x 50 open already meets the 200 total
    }
}
