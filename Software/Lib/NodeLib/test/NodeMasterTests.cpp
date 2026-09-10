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
    const uint32_t baud = 115200;

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
} // namespace

CC_TEST(NodeMaster, StartsWithTheReservedMasterAddress)
{
    ResetWorld();
    NodeMaster master(baud);
    CC_CHECK_EQ(master.GetId(), 0);
}

CC_TEST(NodeMaster, BuildsATypedRosterFromAnnounces)
{
    ResetWorld();
    NodeMaster master(baud);

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
    NodeMaster master(baud);

    Announce(2, 2);
    Announce(5, 2);
    master.Loop();

    FakeBus::Reset();
    master.StartPollingNodes(); // polls the first active node

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

CC_TEST(NodeMaster, IgnoresAnnounceForAnOutOfRangeNode)
{
    ResetWorld();
    NodeMaster master(baud);

    Announce(0, 2);
    Announce(240, 2);
    master.Loop();

    FakeBus::Reset();
    master.StartPollingNodes();

    // No node was actually registered -> nothing to poll.
    Message tx[4];
    CC_CHECK_EQ(bus::DecodeTx(tx, 4), 0);
}
