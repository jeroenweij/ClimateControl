/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "EEndpoint.h"
#include "EOperation.h"

#include "FakeAdc.h"
#include "FakeClock.h"
#include "Test.h"

#include "Damper.h"
#include "RoomControlLoop.h"
#include "SupplyTemp.h"
#include "ThermostatLink.h"

using NodeLib::Endpoint;
using NodeLib::Id;
using NodeLib::LinkMaster;
using NodeLib::Message;
using NodeLib::Operation;

namespace
{
    void ResetWorld()
    {
        FakeClock::Reset();
        FakeAdc::Reset(); // 0 counts -- Damper never reads as stalled unless a test says otherwise
    }

    void PackI16(uint8_t* const out, const int16_t v)
    {
        out[0] = static_cast<uint8_t>(v);
        out[1] = static_cast<uint8_t>(static_cast<uint16_t>(v) >> 8);
    }

    // Seeds ThermostatLink's Room* cache directly -- ReceivedMessage() is a
    // pure cache update (ThermostatLink.cpp), no link/Uart traffic involved,
    // so this doesn't need a live LinkMaster poll cycle.
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

    void SeedSupply(SupplyTemp& supply, const int16_t centiC)
    {
        Message m(Id(9, Endpoint::SupplyTemp, Operation::Report)); // source node id irrelevant, unaddressed
        PackI16(m.data, centiC);
        m.len = 2;
        supply.Snoop(m);
    }
} // namespace

CC_TEST(RoomControlLoop, DefaultsToTheFiftyPercentBudget)
{
    ResetWorld();
    LinkMaster      link;
    Damper          damper;
    ThermostatLink  thermostatLink(link, damper);
    SupplyTemp      supplyTemp;
    RoomControlLoop loop(thermostatLink, supplyTemp, damper);

    CC_CHECK_EQ(loop.Budget(), 50);
}

CC_TEST(RoomControlLoop, DoesNothingUntilTheRoomIsValid)
{
    ResetWorld();
    LinkMaster      link;
    Damper          damper;
    ThermostatLink  thermostatLink(link, damper);
    SupplyTemp      supplyTemp;
    RoomControlLoop loop(thermostatLink, supplyTemp, damper);

    const uint8_t before = damper.Target();
    loop.Loop();
    CC_CHECK_EQ(damper.Target(), before);
}

CC_TEST(RoomControlLoop, StaysClosedWithoutASupplyReading)
{
    ResetWorld();
    LinkMaster      link;
    Damper          damper;
    ThermostatLink  thermostatLink(link, damper);
    SupplyTemp      supplyTemp;
    RoomControlLoop loop(thermostatLink, supplyTemp, damper);

    SeedRoom(thermostatLink, 2400, 1800); // 24.0C room, 18.0C setpoint -- wants cooling
    loop.Loop();

    CC_CHECK_EQ(damper.Target(), 0); // no supply reading -- don't guess
}

CC_TEST(RoomControlLoop, DemandBelowBudgetDoesNotPinTheDamperAtTheCeiling)
{
    ResetWorld();
    LinkMaster      link;
    Damper          damper;
    ThermostatLink  thermostatLink(link, damper);
    SupplyTemp      supplyTemp;
    RoomControlLoop loop(thermostatLink, supplyTemp, damper);

    SeedRoom(thermostatLink, 1850, 1800); // 0.5C above setpoint -- small demand
    SeedSupply(supplyTemp, 1500); // colder than the room -- helps cooling
    loop.Loop();

    CC_CHECK(damper.Target() > 0);
    CC_CHECK(damper.Target() < loop.Budget()); // "any value between 0 and budget", not always at budget
}

CC_TEST(RoomControlLoop, FullDemandIsClampedToTheBudgetCeiling)
{
    ResetWorld();
    LinkMaster      link;
    Damper          damper;
    ThermostatLink  thermostatLink(link, damper);
    SupplyTemp      supplyTemp;
    RoomControlLoop loop(thermostatLink, supplyTemp, damper);

    SeedRoom(thermostatLink, 2400, 1800); // 6.0C above setpoint -- saturates demand at 100%
    SeedSupply(supplyTemp, 1500);
    loop.SetBudget(30);
    loop.Loop();

    CC_CHECK_EQ(damper.Target(), 30);
}

CC_TEST(RoomControlLoop, SetBudgetReclampsAnAlreadyOpenDamperImmediately)
{
    ResetWorld();
    LinkMaster      link;
    Damper          damper;
    ThermostatLink  thermostatLink(link, damper);
    SupplyTemp      supplyTemp;
    RoomControlLoop loop(thermostatLink, supplyTemp, damper);

    SeedRoom(thermostatLink, 2400, 1800);
    SeedSupply(supplyTemp, 1500);
    loop.Loop();
    CC_CHECK_EQ(damper.Target(), 50); // default budget, full demand

    loop.SetBudget(20);
    CC_CHECK_EQ(damper.Target(), 20); // reclamped without a further Loop() call
}

CC_TEST(RoomControlLoop, BudgetNeverReclampsAManualOverride)
{
    ResetWorld();
    LinkMaster      link;
    Damper          damper;
    ThermostatLink  thermostatLink(link, damper);
    SupplyTemp      supplyTemp;
    RoomControlLoop loop(thermostatLink, supplyTemp, damper);

    damper.SetTarget(77);
    damper.SetMode(Damper::Mode::Manual);

    loop.SetBudget(10);
    CC_CHECK_EQ(damper.Target(), 77); // Manual is an explicit override, budget doesn't touch it

    SeedRoom(thermostatLink, 2400, 1800);
    SeedSupply(supplyTemp, 1500);
    loop.Loop();
    CC_CHECK_EQ(damper.Target(), 77); // still untouched -- Loop() only drives Auto
}

CC_TEST(RoomControlLoop, DisconnectRampReturnsTowardDefaultOnePointPerInterval)
{
    ResetWorld();
    LinkMaster      link;
    Damper          damper;
    ThermostatLink  thermostatLink(link, damper);
    SupplyTemp      supplyTemp;
    RoomControlLoop loop(thermostatLink, supplyTemp, damper);

    loop.SetBudget(90);
    loop.ConnectionLost();

    FakeClock::Advance(36000);
    loop.Loop();
    CC_CHECK_EQ(loop.Budget(), 89);

    FakeClock::Advance(36000);
    loop.Loop();
    CC_CHECK_EQ(loop.Budget(), 88);
}

CC_TEST(RoomControlLoop, RampNeverOvershootsPastTheDefault)
{
    ResetWorld();
    LinkMaster      link;
    Damper          damper;
    ThermostatLink  thermostatLink(link, damper);
    SupplyTemp      supplyTemp;
    RoomControlLoop loop(thermostatLink, supplyTemp, damper);

    loop.SetBudget(51);
    loop.ConnectionLost();

    FakeClock::Advance(36000);
    loop.Loop();
    CC_CHECK_EQ(loop.Budget(), 50);

    FakeClock::Advance(360000); // plenty more time
    loop.Loop();
    CC_CHECK_EQ(loop.Budget(), 50); // stays put, doesn't ramp past the default
}

CC_TEST(RoomControlLoop, AFreshSetBudgetCancelsTheRamp)
{
    ResetWorld();
    LinkMaster      link;
    Damper          damper;
    ThermostatLink  thermostatLink(link, damper);
    SupplyTemp      supplyTemp;
    RoomControlLoop loop(thermostatLink, supplyTemp, damper);

    loop.SetBudget(90);
    loop.ConnectionLost();
    FakeClock::Advance(36000);
    loop.Loop();
    CC_CHECK_EQ(loop.Budget(), 89);

    loop.SetBudget(70); // a live MainController is proven by this arriving at all
    FakeClock::Advance(360000);
    loop.Loop();
    CC_CHECK_EQ(loop.Budget(), 70); // unchanged -- the ramp was cancelled, not just paused
}

CC_TEST(RoomControlLoop, RampRunsEvenWithoutRoomData)
{
    ResetWorld();
    LinkMaster      link;
    Damper          damper;
    ThermostatLink  thermostatLink(link, damper);
    SupplyTemp      supplyTemp;
    RoomControlLoop loop(thermostatLink, supplyTemp, damper);

    loop.SetBudget(10);
    loop.ConnectionLost();

    FakeClock::Advance(36000);
    loop.Loop(); // room never seeded -- the ramp must not depend on it
    CC_CHECK_EQ(loop.Budget(), 11);
}
