/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "EEndpoint.h"
#include "EOperation.h"

#include "BusHelpers.h"
#include "FakeAdc.h"
#include "FakeBus.h"
#include "FakeClock.h"
#include "FakeConfigStore.h"
#include "Test.h"

#include "ControllerHandler.h"
#include "Damper.h"
#include "ThermostatLink.h"

using NodeLib::ConfigStore;
using NodeLib::Endpoint;
using NodeLib::Id;
using NodeLib::LinkMaster;
using NodeLib::Message;
using NodeLib::Node;
using NodeLib::Operation;
using NodeLib::SystemStatus;

namespace
{
    const uint8_t kNodeId = 5;

    void ResetWorld()
    {
        FakeBus::Reset();
        FakeClock::Reset();
        FakeAdc::Reset();
        FakeConfig::Reset();
        FakeConfig::SetValid(true);
        FakeConfig::SetNodeId(kNodeId);
        FakeConfig::SetModule(ConfigStore::Module::ControllerNode);
    }

    void PackI16(uint8_t* const out, const int16_t v)
    {
        out[0] = static_cast<uint8_t>(v);
        out[1] = static_cast<uint8_t>(static_cast<uint16_t>(v) >> 8);
    }

    // Seeds ThermostatLink's Room* cache directly, same rationale as
    // RoomControlLoopTests: ReceivedMessage() is a pure cache update, no link
    // Uart traffic -- this ControllerHandler suite never brings the link up
    // (LinkMaster::Init()/Loop() are never called), so LinkUp() stays false
    // throughout, matching a fresh boot before the Thermostat has announced.
    void SeedRoom(ThermostatLink& link, const int16_t tempCentiC, const int16_t setpointCentiC)
    {
        Message temp(Id(0, Endpoint::RoomTemp, Operation::Report));
        PackI16(temp.data, tempCentiC);
        temp.len = 2;
        link.ReceivedMessage(temp);

        Message setpoint(Id(0, Endpoint::RoomSetpoint, Operation::Report));
        PackI16(setpoint.data, setpointCentiC);
        setpoint.len = 2;
        link.ReceivedMessage(setpoint);
    }

    void SeedSupply(ControllerHandler& handler, const int16_t centiC)
    {
        Message m(Id(9, Endpoint::SupplyTemp, Operation::Report)); // unaddressed -- Snoop() doesn't care
        PackI16(m.data, centiC);
        m.len = 2;
        handler.Snoop(m);
    }

    // A slave only transmits its queued replies during its own polled window
    // (RS485-Node-Protocol-Spec-STM32G030.md §6) -- inject the master's Poll
    // and pump once more to flush whatever's queued onto the wire.
    void Flush(Node& node)
    {
        bus::InjectFrame(Message(kNodeId, Operation::Poll));
        node.Loop();
    }

    bool FindReport(Message* const tx, const int n, const Endpoint endpoint, uint8_t* const data, const uint8_t len)
    {
        for (int i = 0; i < n; i++)
        {
            if (tx[i].id.endpoint == endpoint && tx[i].id.operation == Operation::Report && tx[i].len >= len)
            {
                for (uint8_t j = 0; j < len; j++)
                {
                    data[j] = tx[i].data[j];
                }
                return true;
            }
        }
        return false;
    }

    bool AnyNack(Message* const tx, const int n, const Endpoint endpoint)
    {
        for (int i = 0; i < n; i++)
        {
            if (tx[i].id.endpoint == endpoint && tx[i].id.operation == Operation::Nack)
            {
                return true;
            }
        }
        return false;
    }

    // Bundles the four collaborators ControllerHandler needs, wired the same
    // way Modules/ControllerNode/main.cpp does, minus ever bringing the link
    // up (see SeedRoom's comment).
    struct World
    {
        Node              node;
        Damper            damper;
        LinkMaster        link;
        ThermostatLink    thermostatLink;
        ControllerHandler handler;

        World() :
            node(),
            damper(),
            link(),
            thermostatLink(link, damper),
            handler(node, damper, thermostatLink)
        {
            node.RegisterHandler(&handler);
            node.Init();
        }
    };
} // namespace

CC_TEST(ControllerHandler, SetDamperTargetMovesTheDamper)
{
    ResetWorld();
    World w;

    bus::InjectFrame(Message(Id(kNodeId, Endpoint::DamperTarget, Operation::Set), 42));
    w.node.Loop();

    CC_CHECK_EQ(w.damper.Target(), 42);
}

CC_TEST(ControllerHandler, GetDamperTargetReportsTheCurrentValue)
{
    ResetWorld();
    World w;
    w.damper.SetTarget(33);
    FakeBus::Reset();

    bus::InjectFrame(Message(Id(kNodeId, Endpoint::DamperTarget, Operation::Get)));
    Flush(w.node);

    Message   tx[8];
    const int n = bus::DecodeTx(tx, 8);
    uint8_t   value;
    CC_CHECK(FindReport(tx, n, Endpoint::DamperTarget, &value, 1));
    CC_CHECK_EQ(value, 33);
}

CC_TEST(ControllerHandler, SetsAndReportsDamperBudget)
{
    ResetWorld();
    World w;

    bus::InjectFrame(Message(Id(kNodeId, Endpoint::DamperBudget, Operation::Set), 25));
    w.node.Loop();

    FakeBus::Reset();
    bus::InjectFrame(Message(Id(kNodeId, Endpoint::DamperBudget, Operation::Get)));
    Flush(w.node);

    Message   tx[8];
    const int n = bus::DecodeTx(tx, 8);
    uint8_t   value;
    CC_CHECK(FindReport(tx, n, Endpoint::DamperBudget, &value, 1));
    CC_CHECK_EQ(value, 25);
}

CC_TEST(ControllerHandler, SetDamperBudgetWithNoPayloadIsNacked)
{
    ResetWorld();
    World w;

    Message bad(Id(kNodeId, Endpoint::DamperBudget, Operation::Set));
    bad.len = 0;
    bus::InjectFrame(bad);
    Flush(w.node);

    Message   tx[8];
    const int n = bus::DecodeTx(tx, 8);
    CC_CHECK(AnyNack(tx, n, Endpoint::DamperBudget));
}

CC_TEST(ControllerHandler, RoomTempGetReportsTheThermostatLinksCache)
{
    ResetWorld();
    World w;
    SeedRoom(w.thermostatLink, 2150, 2000); // 21.50C

    bus::InjectFrame(Message(Id(kNodeId, Endpoint::RoomTemp, Operation::Get)));
    Flush(w.node);

    Message   tx[8];
    const int n = bus::DecodeTx(tx, 8);
    uint8_t   payload[2];
    CC_CHECK(FindReport(tx, n, Endpoint::RoomTemp, payload, 2));
    CC_CHECK_EQ(static_cast<int16_t>(payload[0] | (payload[1] << 8)), 2150);
}

CC_TEST(ControllerHandler, RoomSetpointSetIsAcceptedNotNacked)
{
    ResetWorld();
    World w;

    Message setpoint(Id(kNodeId, Endpoint::RoomSetpoint, Operation::Set));
    PackI16(setpoint.data, 2200);
    setpoint.len = 2;
    bus::InjectFrame(setpoint);
    Flush(w.node);

    Message   tx[8];
    const int n = bus::DecodeTx(tx, 8);
    CC_CHECK(!AnyNack(tx, n, Endpoint::RoomSetpoint));
}

CC_TEST(ControllerHandler, RoomTempSetIsReadOnlyAndNacked)
{
    ResetWorld();
    World w;

    Message set(Id(kNodeId, Endpoint::RoomTemp, Operation::Set));
    PackI16(set.data, 2200);
    set.len = 2;
    bus::InjectFrame(set);
    Flush(w.node);

    Message   tx[8];
    const int n = bus::DecodeTx(tx, 8);
    CC_CHECK(AnyNack(tx, n, Endpoint::RoomTemp));
}

CC_TEST(ControllerHandler, RoomLinkGetReportsDownBeforeTheLinkEverComesUp)
{
    ResetWorld();
    World w;

    bus::InjectFrame(Message(Id(kNodeId, Endpoint::RoomLink, Operation::Get)));
    Flush(w.node);

    Message   tx[8];
    const int n = bus::DecodeTx(tx, 8);
    uint8_t   value;
    CC_CHECK(FindReport(tx, n, Endpoint::RoomLink, &value, 1));
    CC_CHECK_EQ(value, 0);
}

CC_TEST(ControllerHandler, SnoopedSupplyTempDrivesTheDamperThroughLoop)
{
    ResetWorld();
    World w;
    SeedRoom(w.thermostatLink, 2400, 1800); // wants cooling, full-authority error
    SeedSupply(w.handler, 1500); // colder than the room -- helps

    w.handler.Loop();

    CC_CHECK_EQ(w.damper.Target(), 50); // default budget, saturated demand
}

CC_TEST(ControllerHandler, ConnectionLostStartsTheBudgetRamp)
{
    ResetWorld();
    World w;

    bus::InjectFrame(Message(Id(kNodeId, Endpoint::DamperBudget, Operation::Set), 90));
    w.node.Loop();

    w.handler.ConnectionLost();
    FakeClock::Advance(36000);
    w.handler.Loop();

    FakeBus::Reset();
    bus::InjectFrame(Message(Id(kNodeId, Endpoint::DamperBudget, Operation::Get)));
    Flush(w.node);

    Message   tx[8];
    const int n = bus::DecodeTx(tx, 8);
    uint8_t   value;
    CC_CHECK(FindReport(tx, n, Endpoint::DamperBudget, &value, 1));
    CC_CHECK_EQ(value, 89); // one ramp step toward the 50% default
}

CC_TEST(ControllerHandler, PrepareForResetParksTheDamperAtNeutral)
{
    ResetWorld();
    World w;
    w.damper.SetTarget(90);

    w.handler.PrepareForReset();

    CC_CHECK_EQ(w.damper.Target(), 50);
    CC_CHECK_EQ(w.damper.Actual(), 50);
    CC_CHECK(!w.damper.Moving());
}

CC_TEST(ControllerHandler, FillStatusFlagsThermostatLinkDownBeforeTheLinkComesUp)
{
    ResetWorld();
    World w;

    SystemStatus status{};
    w.handler.FillStatus(status);

    CC_CHECK((status.errorFlags & 0x0001) != 0);
}

CC_TEST(ControllerHandler, LoopReportsDamperActualOnceTheMoveSettles)
{
    ResetWorld();
    World w;
    w.damper.SetTarget(80);

    FakeClock::Advance(1500); // Damper's moveSettleMs
    w.handler.Loop(); // damper.Loop() finishes the move, actual changes -> Report queued
    Flush(w.node);

    Message   tx[8];
    const int n = bus::DecodeTx(tx, 8);
    uint8_t   value;
    CC_CHECK(FindReport(tx, n, Endpoint::DamperActual, &value, 1));
    CC_CHECK_EQ(value, 80);
}
