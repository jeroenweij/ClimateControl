/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "EEndpoint.h"
#include "EFirmware.h"
#include "EModuleType.h"
#include "EOperation.h"
#include "NodeMaster.h"

#include "BusHelpers.h"
#include "FakeBus.h"
#include "FakeClock.h"
#include "FakeConfigStore.h"
#include "Test.h"

#include "ThermostatMonitor.h"

using NodeLib::Endpoint;
using NodeLib::FirmwareOp;
using NodeLib::Id;
using NodeLib::Message;
using NodeLib::ModuleType;
using NodeLib::NodeMaster;
using NodeLib::Operation;

namespace
{
    const uint32_t discoveryWindowMs = 250;
    const uint32_t pollStepMs        = 2000; // ThermostatMonitor::pollStepMs
    const uint32_t keepaliveMs       = 60000; // ThermostatMonitor::keepaliveMs

    const uint8_t cnId   = 3;
    const uint8_t tempId = 5;

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

    // Init() broadcasts Discover -- clear the bus before injecting replies,
    // then let the master see them.
    void Bring(NodeMaster& master)
    {
        master.Init();
        FakeBus::Reset();
        Announce(cnId, ModuleType::ControllerNode);
        Announce(tempId, ModuleType::TemperatureNode);
        master.Loop();
    }

    Message RoomLink(const uint8_t nodeId, const bool up)
    {
        Message m(Id(nodeId, Endpoint::RoomLink, Operation::Report));
        m.data[0] = up ? 1 : 0;
        m.len     = 1;
        return m;
    }

    Message FirmwareStatus(const uint8_t nodeId, const uint8_t state, const uint16_t fwVersion)
    {
        Message m(Id(nodeId, Endpoint::ThermostatFirmware, Operation::Report));
        m.data[0] = static_cast<uint8_t>(FirmwareOp::Status);
        m.data[1] = state;
        m.data[7] = static_cast<uint8_t>(fwVersion);
        m.data[8] = static_cast<uint8_t>(fwVersion >> 8);
        m.len     = 9;
        return m;
    }

    // Lets the master flush what's queued: start polling, then answer the
    // poll with Done, which is when queued frames go out.
    int FlushGets(NodeMaster& master, Message* const tx, const int maxTx)
    {
        FakeClock::Advance(discoveryWindowMs);
        master.Loop(); // polls cnId (the first active node)
        FakeBus::Reset();
        bus::InjectFrame(Message(cnId, Operation::Done));
        master.Loop();
        return bus::DecodeTx(tx, maxTx);
    }

    bool HasGet(const Message* const tx, const int n, const uint8_t nodeId, const Endpoint endpoint)
    {
        for (int i = 0; i < n; i++)
        {
            if (tx[i].id.node == nodeId && tx[i].id.endpoint == endpoint && tx[i].id.operation == Operation::Get)
            {
                return true;
            }
        }
        return false;
    }
} // namespace

CC_TEST(ThermostatMonitor, SendsNothingUntilLinkAndFirmwareAreBothKnown)
{
    ResetWorld();
    NodeMaster        master;
    ThermostatMonitor monitor(master);
    Bring(master);

    Message out;
    monitor.Observe(RoomLink(cnId, true));
    CC_CHECK(!monitor.NextStatus(out));

    monitor.Observe(FirmwareStatus(cnId, 0, 0x0102));
    CC_CHECK(monitor.NextStatus(out));
    CC_CHECK(!monitor.NextStatus(out)); // sent -- not due again until something changes
}

CC_TEST(ThermostatMonitor, StatusCarriesTheSpecPayload)
{
    ResetWorld();
    NodeMaster        master;
    ThermostatMonitor monitor(master);
    Bring(master);

    monitor.Observe(RoomLink(cnId, true));
    monitor.Observe(FirmwareStatus(cnId, 0, 0x0102)); // 1.2, running the application

    Message out;
    CC_CHECK(monitor.NextStatus(out));
    CC_CHECK_EQ(out.id.node, 0);
    CC_CHECK(out.id.endpoint == Endpoint::ThermostatStatus);
    CC_CHECK(out.id.operation == Operation::Report);
    CC_CHECK_EQ(out.len, 17);
    CC_CHECK_EQ(out.data[0], cnId);
    CC_CHECK_EQ(out.data[1], 1); // linkUp
    CC_CHECK_EQ(out.data[2], 0); // blState: application
    CC_CHECK_EQ(out.data[3], 1); // fwMajor
    CC_CHECK_EQ(out.data[4], 2); // fwMinor
    for (int i = 5; i < 17; i++)
    {
        CC_CHECK_EQ(out.data[i], 0); // uid not known
    }
}

CC_TEST(ThermostatMonitor, AChangeMakesItDueAgainAnUnchangedReportDoesNot)
{
    ResetWorld();
    NodeMaster        master;
    ThermostatMonitor monitor(master);
    Bring(master);
    monitor.Observe(RoomLink(cnId, true));
    monitor.Observe(FirmwareStatus(cnId, 0, 0x0102));
    Message out;
    monitor.NextStatus(out);

    monitor.Observe(RoomLink(cnId, true)); // same
    CC_CHECK(!monitor.NextStatus(out));

    monitor.Observe(RoomLink(cnId, false)); // link lost
    CC_CHECK(monitor.NextStatus(out));
    CC_CHECK_EQ(out.data[1], 0);

    monitor.Observe(FirmwareStatus(cnId, 0, 0x0103)); // new firmware
    CC_CHECK(monitor.NextStatus(out));
    CC_CHECK_EQ(out.data[4], 3);
}

CC_TEST(ThermostatMonitor, IgnoresReportsFromNodesThatAreNotControllerNodes)
{
    ResetWorld();
    NodeMaster        master;
    ThermostatMonitor monitor(master);
    Bring(master);

    monitor.Observe(RoomLink(tempId, true));
    monitor.Observe(FirmwareStatus(tempId, 0, 0x0102));

    Message out;
    CC_CHECK(!monitor.NextStatus(out));
}

CC_TEST(ThermostatMonitor, ResendAllAndTheKeepaliveMakeKnownThermostatsDue)
{
    ResetWorld();
    NodeMaster        master;
    ThermostatMonitor monitor(master);
    Bring(master);
    monitor.Observe(RoomLink(cnId, true));
    monitor.Observe(FirmwareStatus(cnId, 0, 0x0102));
    Message out;
    monitor.NextStatus(out);

    monitor.ResendAll(); // uplink reconnected
    CC_CHECK(monitor.NextStatus(out));
    CC_CHECK(!monitor.NextStatus(out));

    FakeClock::Advance(keepaliveMs + 1);
    monitor.Loop();
    CC_CHECK(monitor.NextStatus(out));
}

CC_TEST(ThermostatMonitor, PollsAControllerNodeForLinkAndFirmware)
{
    ResetWorld();
    NodeMaster        master;
    ThermostatMonitor monitor(master);
    Bring(master);

    FakeClock::Advance(pollStepMs + 1);
    monitor.Loop(); // queues the Gets on the master

    Message   tx[16];
    const int n = FlushGets(master, tx, 16);
    CC_CHECK(HasGet(tx, n, cnId, Endpoint::RoomLink));
    CC_CHECK(HasGet(tx, n, cnId, Endpoint::ThermostatFirmware));
    CC_CHECK(!HasGet(tx, n, tempId, Endpoint::RoomLink)); // not a ControllerNode
}

CC_TEST(ThermostatMonitor, DoesNotPollWhileTheThermostatIsInItsBootloader)
{
    ResetWorld();
    NodeMaster        master;
    ThermostatMonitor monitor(master);
    Bring(master);
    monitor.Observe(FirmwareStatus(cnId, 2, 0x0102)); // a push is running

    FakeClock::Advance(pollStepMs + 1);
    monitor.Loop();

    Message   tx[16];
    const int n = FlushGets(master, tx, 16);
    CC_CHECK(!HasGet(tx, n, cnId, Endpoint::ThermostatFirmware));
}
