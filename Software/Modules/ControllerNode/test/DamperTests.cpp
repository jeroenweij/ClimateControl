/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "FakeAdc.h"
#include "FakeClock.h"
#include "Test.h"

#include "Damper.h"

// Damper's stall detection (Node-Bus-Power-Path-Spec.md §3.1.1) was never
// directly exercised before -- every other suite that touches Damper holds
// FakeAdc at 0 specifically to keep it from engaging.
namespace
{
    void ResetWorld()
    {
        FakeClock::Reset();
        FakeAdc::Reset();
    }

    // Damper::stallThresholdCounts / stallConfirmMs / moveSettleMs are
    // private; mirrored here from Damper.cpp's own constants so the tests
    // read as "past/under the threshold" rather than magic numbers.
    const uint16_t stallThresholdCounts = 500;
    const uint32_t stallConfirmMs       = 200;
    const uint32_t moveSettleMs         = 1500;
} // namespace

CC_TEST(Damper, SetTargetMovesAndSettlesAfterMoveSettleMs)
{
    ResetWorld();
    Damper damper;

    damper.SetTarget(80);
    CC_CHECK(damper.Moving());
    CC_CHECK_EQ(damper.Actual(), 50); // unchanged until the move settles

    FakeClock::Advance(moveSettleMs + 1);
    damper.Loop();

    CC_CHECK_EQ(damper.Actual(), 80);
    CC_CHECK(!damper.Moving());
}

CC_TEST(Damper, SustainedOvercurrentDeclaresAStallBeforeTheMoveSettles)
{
    ResetWorld();
    Damper damper;

    damper.SetTarget(80);
    FakeAdc::SetValue(stallThresholdCounts);
    damper.Loop(); // current over threshold, first time seen -- arms the confirm timer

    FakeClock::Advance(stallConfirmMs + 1);
    damper.Loop(); // confirm timer elapsed while still over threshold -- stall declared

    CC_CHECK(damper.Stalled());
    CC_CHECK(!damper.Moving()); // cut early, well before moveSettleMs
    CC_CHECK_EQ(damper.Actual(), 50); // never reached "settled" -- actual never adopted target
}

CC_TEST(Damper, CurrentDroppingBackBelowThresholdCancelsTheStallTimer)
{
    ResetWorld();
    Damper damper;

    damper.SetTarget(80);
    FakeAdc::SetValue(stallThresholdCounts);
    damper.Loop(); // arms the confirm timer

    FakeAdc::SetValue(0); // current drops back down -- not a sustained stall
    damper.Loop(); // stops the confirm timer (Damper.cpp's own comment on this branch)

    FakeAdc::SetValue(stallThresholdCounts);
    FakeClock::Advance(stallConfirmMs + 1); // would have been enough time for the FIRST arm
    damper.Loop(); // but the timer was cancelled and only just re-armed this call

    CC_CHECK(!damper.Stalled());
    CC_CHECK(damper.Moving());
}

CC_TEST(Damper, AFreshSetTargetClearsAPriorStallAndTriesAgain)
{
    ResetWorld();
    Damper damper;

    damper.SetTarget(80);
    FakeAdc::SetValue(stallThresholdCounts);
    damper.Loop();
    FakeClock::Advance(stallConfirmMs + 1);
    damper.Loop();
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
    FakeAdc::SetValue(stallThresholdCounts);
    damper.Loop();
    FakeClock::Advance(stallConfirmMs + 1);
    damper.Loop();

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

CC_TEST(Damper, ParkNeutralStopsTheServoAtFiftyPercent)
{
    ResetWorld();
    Damper damper;

    damper.SetTarget(90);
    CC_CHECK(damper.Moving());

    damper.ParkNeutral();

    CC_CHECK_EQ(damper.Target(), 50);
    CC_CHECK_EQ(damper.Actual(), 50);
    CC_CHECK(!damper.Moving());
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
