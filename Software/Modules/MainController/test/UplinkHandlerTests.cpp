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
#include "UplinkHandler.h"

using NodeLib::ConfigStore;
using NodeLib::Endpoint;
using NodeLib::Id;
using NodeLib::Message;
using NodeLib::NodeMaster;
using NodeLib::Operation;

// Deliberately narrow scope: UplinkHandler::Init()/Loop() drive a multi-state
// NINA AT bring-up (probe / Wi-Fi join / peer connect / data mode) that has
// no fake at the AT-protocol level yet -- simulating it is a separate,
// larger undertaking than this feature. What's exercised here is the one
// piece of new, this-feature-relevant logic: ReceivedMessage() must feed
// BudgetAllocator::Observe() unconditionally, even with the uplink down
// (UplinkHandler.cpp's class comment / Damper-Budget-Spec.md §5.4) --
// regression coverage for that specific ordering, not full uplink coverage.
namespace
{
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

    Message ReportI16(const uint8_t nodeId, const Endpoint endpoint, const int16_t value)
    {
        Message m(Id(nodeId, endpoint, Operation::Report));
        PackI16(m.data, value);
        m.len = 2;
        return m;
    }

    // Same order requirement as BudgetAllocatorTests.cpp: Init() broadcasts
    // Discover, so clear the fake bus (which also clears RX) before injecting
    // replies.
    void InitAndClearDiscover(NodeMaster& master)
    {
        master.Init();
        FakeBus::Reset();
    }

    void StartPolling(NodeMaster& master)
    {
        FakeClock::Advance(discoveryWindowMs);
        master.Loop();
    }

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
} // namespace

CC_TEST(UplinkHandler, ObservesBusTrafficThroughReceivedMessageEvenWithTheUplinkDown)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);
    UplinkHandler   uplink(master, allocator);
    // uplink.Init()/.Loop() never called -- nina stays out of data mode,
    // i.e. exactly "uplink down" (NinaAt::InDataMode() defaults to false).

    InitAndClearDiscover(master);
    Announce(2, ConfigStore::Module::ControllerNode);
    Announce(3, ConfigStore::Module::ControllerNode);
    StartPolling(master); // polls node 2 first (lowest id)

    // Two nodes with deliberately different demand: if Observe() were a
    // no-op (e.g. gated behind the uplink's InDataMode() check), weightSum
    // would stay 0 and BOTH nodes would land on the even-split default (25
    // each, pool = 50*2 = 100) -- indistinguishable from "no data yet". The
    // asymmetric 100/0 split below is only reachable if every
    // ReceivedMessage() call actually reached BudgetAllocator::Observe().
    uplink.ReceivedMessage(ReportI16(2, Endpoint::RoomTemp, 2400)); // wants cooling, saturates demand
    uplink.ReceivedMessage(ReportI16(2, Endpoint::RoomSetpoint, 1800));
    uplink.ReceivedMessage(ReportI16(3, Endpoint::RoomTemp, 2000)); // already at setpoint -- zero demand
    uplink.ReceivedMessage(ReportI16(3, Endpoint::RoomSetpoint, 2000));
    uplink.ReceivedMessage(ReportI16(9, Endpoint::SupplyTemp, 1500)); // colder than either room -- helps cooling

    allocator.Loop();
    FlushQueuedBudgets(master, 2);

    Message   tx[8];
    const int n = bus::DecodeTx(tx, 8);
    uint8_t   p2, p3;
    CC_CHECK(FindBudget(tx, n, 2, p2));
    CC_CHECK(FindBudget(tx, n, 3, p3));
    CC_CHECK_EQ(p2, 100);
    CC_CHECK_EQ(p3, 0);
}
