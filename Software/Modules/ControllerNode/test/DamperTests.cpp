/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "FakeAdc.h"
#include "FakeClock.h"
#include "FakePwm.h"
#include "Test.h"
#include "Tick.h"

#include "Damper.h"

// Damper's slewed moves and stall detection (Node-Bus-Power-Path-Spec.md
// §3.1.1). Every other suite that touches Damper holds FakeAdc at 0
// specifically to keep stall detection from engaging.
namespace
{
    void ResetWorld()
    {
        FakeClock::Reset();
        FakeAdc::Reset();
        FakePwm::Reset();
    }

    // Damper's timing constants are private; mirrored here from Damper.h so
    // the tests read as "past/under the threshold" rather than magic numbers.
    const uint16_t stallThresholdCounts = 500;
    const uint32_t stallConfirmMs       = 200;
    const uint32_t fullStrokeMs         = 4000; // 0..100 %, i.e. 2000 us of pulse
    const uint32_t settleMs             = 300;

    // Slew time for a move of the given size, in percent.
    uint32_t SlewMs(const uint32_t percent)
    {
        return fullStrokeMs * percent / 100;
    }

    // Main-loop passes, 10 ms apart.
    void RunFor(Damper& damper, const uint32_t ms)
    {
        for (uint32_t t = 0; t < ms; t += 10)
        {
            FakeClock::Advance(10);
            damper.Loop();
        }
    }

    void Stall(Damper& damper)
    {
        FakeAdc::SetValue(stallThresholdCounts);
        damper.Loop(); // current over threshold, first time seen -- arms the confirm timer
        FakeClock::Advance(stallConfirmMs + 1);
        damper.Loop(); // confirm timer elapsed while still over threshold -- stall declared
    }
} // namespace

CC_TEST(Damper, StartsOffAtNeutral)
{
    ResetWorld();
    Damper damper;
    damper.Init();

    CC_CHECK(damper.GetState() == Damper::State::Off);
    CC_CHECK_EQ(damper.Actual(), 50);
    CC_CHECK_EQ(FakePwm::PulseUs(), 0);
    CC_CHECK(FakePwm::Initialised());
}

CC_TEST(Damper, SetTargetSlewsSettlesAndPowersDown)
{
    ResetWorld();
    Damper damper;
    damper.Init();

    damper.SetTarget(80);
    CC_CHECK(damper.GetState() == Damper::State::Moving);
    CC_CHECK(damper.Moving());

    RunFor(damper, SlewMs(30) - 20);
    CC_CHECK(damper.GetState() == Damper::State::Moving); // not there yet
    CC_CHECK_EQ(damper.Actual(), 50); // unchanged until the move settles

    RunFor(damper, 20);
    CC_CHECK(damper.GetState() == Damper::State::Settling);
    CC_CHECK_EQ(FakePwm::PulseUs(), 2100); // pulse at the target, servo still powered

    RunFor(damper, settleMs);
    CC_CHECK(damper.GetState() == Damper::State::Off);
    CC_CHECK_EQ(damper.Actual(), 80);
    CC_CHECK_EQ(FakePwm::PulseUs(), 0); // signal low with the rail off
}

CC_TEST(Damper, PowerUpStartsOnTheLastCommandedPulse)
{
    ResetWorld();
    Damper damper;
    damper.Init();

    damper.SetTarget(100);
    CC_CHECK_EQ(FakePwm::PulseUs(), 1500); // where the horn is (neutral), not the target

    RunFor(damper, SlewMs(50) + settleMs);
    CC_CHECK(!damper.Moving());

    damper.SetTarget(0);
    CC_CHECK_EQ(FakePwm::PulseUs(), 2500); // the next move picks up where the last one ended
}

CC_TEST(Damper, SlewsAtTheFullStrokeRate)
{
    ResetWorld();
    Damper damper;
    damper.Init();

    damper.SetTarget(100);
    RunFor(damper, fullStrokeMs / 4); // a quarter stroke's worth of time
    CC_CHECK_EQ(FakePwm::PulseUs(), 2000); // 1500 + 2000 us / 4

    damper.SetTarget(0);
    RunFor(damper, fullStrokeMs / 8);
    CC_CHECK_EQ(FakePwm::PulseUs(), 1750); // same rate, other direction
}

CC_TEST(Damper, ANewTargetMidMoveReAimsWithoutAPowerCycle)
{
    ResetWorld();
    Damper damper;
    damper.Init();

    damper.SetTarget(100);
    RunFor(damper, SlewMs(20)); // 1500 -> 1900 us
    CC_CHECK_EQ(FakePwm::PulseUs(), 1900);

    damper.SetTarget(30);
    CC_CHECK(damper.GetState() == Damper::State::Moving);
    CC_CHECK_EQ(FakePwm::PulseUs(), 1900); // no jump, no rail drop

    RunFor(damper, SlewMs(40) + settleMs); // 70 % -> 30 %
    CC_CHECK(damper.GetState() == Damper::State::Off);
    CC_CHECK_EQ(damper.Actual(), 30);
}

CC_TEST(Damper, ANewTargetWhileSettlingMovesAgain)
{
    ResetWorld();
    Damper damper;
    damper.Init();

    damper.SetTarget(60);
    RunFor(damper, SlewMs(10));
    CC_CHECK(damper.GetState() == Damper::State::Settling);

    damper.SetTarget(70);
    CC_CHECK(damper.GetState() == Damper::State::Moving);
    RunFor(damper, settleMs); // the old settle window must not end the new move
    CC_CHECK(damper.Moving());

    RunFor(damper, SlewMs(10) + settleMs);
    CC_CHECK_EQ(damper.Actual(), 70);
    CC_CHECK(!damper.Moving());
}

CC_TEST(Damper, SustainedOvercurrentDeclaresAStallBeforeTheMoveEnds)
{
    ResetWorld();
    Damper damper;

    damper.SetTarget(80);
    Stall(damper);

    CC_CHECK(damper.Stalled());
    CC_CHECK(damper.GetState() == Damper::State::Stalled);
    CC_CHECK(!damper.Moving()); // cut early, well before the move would end
    CC_CHECK_EQ(damper.Actual(), 50); // never settled -- actual never adopted target
}

CC_TEST(Damper, CurrentDroppingBackBelowThresholdCancelsTheStallTimer)
{
    ResetWorld();
    Damper damper;

    damper.SetTarget(80);
    FakeAdc::SetValue(stallThresholdCounts);
    damper.Loop(); // arms the confirm timer

    FakeAdc::SetValue(0); // current drops back down -- not a sustained stall
    damper.Loop(); // stops the confirm timer

    FakeAdc::SetValue(stallThresholdCounts);
    FakeClock::Advance(stallConfirmMs + 1); // would have been enough time for the FIRST arm
    damper.Loop(); // but the timer was cancelled and only just re-armed this call

    CC_CHECK(!damper.Stalled());
    CC_CHECK(damper.Moving());
}

CC_TEST(Damper, AStallRewindsToWhereTheOvercurrentBegan)
{
    ResetWorld();
    Damper damper;
    damper.Init();

    damper.SetTarget(100);
    RunFor(damper, SlewMs(10)); // 1500 -> 1700 us: jammed here
    FakeAdc::SetValue(stallThresholdCounts);
    damper.Loop(); // arms the confirm timer at 1700 us
    RunFor(damper, stallConfirmMs + 10); // the slew runs on while the stall confirms
    CC_CHECK(damper.Stalled());

    FakeAdc::SetValue(0);
    damper.SetTarget(0);
    CC_CHECK_EQ(FakePwm::PulseUs(), 1700); // retry starts at the jam, not past it
}

CC_TEST(Damper, AFreshSetTargetClearsAPriorStallAndTriesAgain)
{
    ResetWorld();
    Damper damper;

    damper.SetTarget(80);
    Stall(damper);
    CC_CHECK(damper.Stalled());

    FakeAdc::SetValue(0);
    damper.SetTarget(30);

    CC_CHECK(!damper.Stalled());
    CC_CHECK(damper.Moving());
}

CC_TEST(Damper, ReportedModeShowsStalledCodeOnlyWhileStalled)
{
    ResetWorld();
    Damper damper;

    damper.SetMode(Damper::Mode::Auto);
    CC_CHECK_EQ(damper.ReportedMode(), static_cast<uint8_t>(Damper::Mode::Auto));

    damper.SetTarget(80);
    Stall(damper);

    CC_CHECK_EQ(damper.ReportedMode(), 4); // StalledCode -- one past the highest real Mode value
}

CC_TEST(Damper, NeverStallsWhileUnpowered)
{
    ResetWorld();
    Damper damper;

    FakeAdc::SetValue(4095); // pegged high, but nothing has ever called SetTarget()
    FakeClock::Advance(stallConfirmMs + 1);
    damper.Loop();

    CC_CHECK(!damper.Stalled());
    CC_CHECK(!damper.Moving());
}

CC_TEST(Damper, SignalDropsLowOnAStall)
{
    ResetWorld();
    Damper damper;
    damper.Init();

    damper.SetTarget(80);
    Stall(damper);

    CC_CHECK(damper.Stalled());
    CC_CHECK_EQ(FakePwm::PulseUs(), 0);
}

CC_TEST(Damper, SetModeClosedAndOpenDriveTheTargetDirectly)
{
    ResetWorld();
    Damper damper;

    damper.SetMode(Damper::Mode::Closed);
    CC_CHECK_EQ(damper.Target(), 0);

    damper.SetMode(Damper::Mode::Open);
    CC_CHECK_EQ(damper.Target(), 100);
}

CC_TEST(Damper, TargetMapsLinearlyOntoTheDefaultPulseRange)
{
    ResetWorld();
    Damper damper;
    damper.Init();

    const uint8_t  percents[] = {0, 100, 50, 33};
    const uint16_t pulses[]   = {500, 2500, 1500, 1160};
    for (int i = 0; i < 4; i++)
    {
        damper.SetTarget(percents[i]);
        for (uint32_t t = 0; t < fullStrokeMs + 20 && damper.GetState() == Damper::State::Moving; t += 10)
        {
            RunFor(damper, 10);
        }
        CC_CHECK(damper.GetState() == Damper::State::Settling);
        CC_CHECK_EQ(FakePwm::PulseUs(), pulses[i]);
    }
}

CC_TEST(Damper, ParkNeutralSlewsToFiftyPercentBeforeCuttingPower)
{
    ResetWorld();
    Damper damper;
    damper.Init();
    damper.SetTarget(90);
    RunFor(damper, SlewMs(40) + settleMs); // settled at 90, unpowered
    CC_CHECK_EQ(damper.Actual(), 90);

    const uint32_t start = Hal::Tick::Millis();
    damper.ParkNeutral();

    CC_CHECK(Hal::Tick::Millis() - start >= SlewMs(40) + settleMs); // blocked for the whole move
    CC_CHECK_EQ(damper.Target(), 50);
    CC_CHECK_EQ(damper.Actual(), 50);
    CC_CHECK(damper.GetState() == Damper::State::Off);
    CC_CHECK_EQ(FakePwm::PulseUs(), 0);
}

CC_TEST(Damper, ParkNeutralTakesOverAMoveInProgress)
{
    ResetWorld();
    Damper damper;
    damper.Init();
    damper.SetTarget(90);
    CC_CHECK(damper.Moving());

    damper.ParkNeutral();

    CC_CHECK_EQ(damper.Target(), 50);
    CC_CHECK_EQ(damper.Actual(), 50);
    CC_CHECK(!damper.Moving());
}

CC_TEST(Damper, ParkNeutralReturnsAtOnceWhenAlreadyParked)
{
    ResetWorld();
    Damper damper;
    damper.Init(); // starts at neutral, unpowered

    const uint32_t start = Hal::Tick::Millis();
    damper.ParkNeutral();

    CC_CHECK_EQ(Hal::Tick::Millis() - start, 0u);
    CC_CHECK(!damper.Moving());
}

CC_TEST(Damper, ParkNeutralStopsEarlyOnAStall)
{
    ResetWorld();
    Damper damper;
    damper.Init();
    damper.SetTarget(90);
    RunFor(damper, SlewMs(40) + settleMs);

    FakeAdc::SetValue(stallThresholdCounts);
    const uint32_t start = Hal::Tick::Millis();
    damper.ParkNeutral();

    CC_CHECK(damper.Stalled());
    CC_CHECK(!damper.Moving());
    CC_CHECK(Hal::Tick::Millis() - start < SlewMs(40));
    CC_CHECK_EQ(damper.Actual(), 90); // never reached neutral
}

CC_TEST(Damper, ResendingTheSameTargetAfterAStallRetriesTheMove)
{
    ResetWorld();
    Damper damper;
    damper.Init();

    damper.SetTarget(77);
    Stall(damper);
    CC_CHECK(damper.Stalled());
    CC_CHECK(!damper.Moving());

    FakeAdc::SetValue(0);
    damper.SetTarget(77); // same value -- still a fresh attempt after a stall

    CC_CHECK(!damper.Stalled());
    CC_CHECK(damper.Moving());
}

CC_TEST(Damper, ResendingTheSameTargetDoesNotRestartTheMove)
{
    ResetWorld();
    Damper damper;
    damper.Init();

    damper.SetTarget(77);
    RunFor(damper, SlewMs(10));
    const uint16_t midway = FakePwm::PulseUs();
    damper.SetTarget(77); // mid-move: carries on
    CC_CHECK_EQ(FakePwm::PulseUs(), midway);

    RunFor(damper, SlewMs(17) + settleMs);
    CC_CHECK(!damper.Moving());

    damper.SetTarget(77);
    CC_CHECK(!damper.Moving()); // already there -- no pointless servo power-up
}
