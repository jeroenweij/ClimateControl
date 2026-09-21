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
using NodeLib::NodeMaster;
using NodeLib::Operation;

namespace
{
    // Same shape as NodeMaster's own detection window (NodeMasterTests.cpp).
    const uint32_t discoveryWindowMs = 250;

    void ResetWorld()
    {
        FakeBus::Reset();
        FakeClock::Reset();
        FakeConfig::Reset();
    }

    void Announce(const uint8_t nodeId, const ConfigStore::Module module)
    {
        Message m(nodeId, Operation::Announce);
        m.data[0] = static_cast<uint8_t>(module);
        m.len     = 1;
        bus::InjectFrame(m);
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
    Announce(2, ConfigStore::Module::ControllerNode);
    Announce(3, ConfigStore::Module::ControllerNode);
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
    Announce(2, ConfigStore::Module::ControllerNode);
    Announce(3, ConfigStore::Module::ControllerNode);
    StartPolling(master);

    ObserveReport(allocator, 0, Endpoint::SupplyTemp, 1500); // 15.0C, colder than either room
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
    CC_CHECK_EQ(p3, 0); // no floor -- Damper-Budget-Spec.md §5.2
}

CC_TEST(BudgetAllocator, WaterFillsOverflowFromAClampedNodeIntoTheRest)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);

    InitAndClearDiscover(master);
    Announce(2, ConfigStore::Module::ControllerNode);
    Announce(3, ConfigStore::Module::ControllerNode);
    Announce(4, ConfigStore::Module::ControllerNode);
    StartPolling(master);

    ObserveReport(allocator, 0, Endpoint::SupplyTemp, 1500);
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
    Announce(2, ConfigStore::Module::ControllerNode);
    Announce(7, ConfigStore::Module::TemperatureNode);
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

CC_TEST(BudgetAllocator, RecomputeDebouncesWithinTheInterval)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);

    InitAndClearDiscover(master);
    Announce(2, ConfigStore::Module::ControllerNode);
    Announce(3, ConfigStore::Module::ControllerNode);
    StartPolling(master);

    allocator.Loop(); // first call recomputes immediately
    allocator.Loop(); // no clock advance -- must not recompute (or queue) again
    FlushQueuedBudgets(master, 2);

    Message   tx[8];
    const int n = bus::DecodeTx(tx, 8);
    CC_CHECK_EQ(CountBudgetMessages(tx, n), 2); // one Set per node, not two rounds' worth
}
