/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "DelayTimer.h"

#include "FakeClock.h"
#include "Test.h"

using Tools::DelayTimer;

CC_TEST(DelayTimer, IdleTimerIsNeverFinished)
{
    FakeClock::Reset();
    DelayTimer timer;

    CC_CHECK(!timer.IsRunning());
    CC_CHECK(!timer.Finished());

    FakeClock::Advance(100000);
    CC_CHECK(!timer.Finished());
}

CC_TEST(DelayTimer, FinishesExactlyAtTheDeadline)
{
    FakeClock::Reset();
    DelayTimer timer;
    timer.Start(50);

    CC_CHECK(timer.IsRunning());

    FakeClock::Advance(49);
    CC_CHECK(!timer.Finished());

    FakeClock::Advance(1);
    CC_CHECK(timer.Finished());
}

CC_TEST(DelayTimer, StopsItselfOnceFinished)
{
    FakeClock::Reset();
    DelayTimer timer(10);
    timer.Start(10);

    FakeClock::Advance(10);
    CC_CHECK(timer.Finished());
    // Second poll is false -- Finished() latched it off.
    CC_CHECK(!timer.Finished());
    CC_CHECK(!timer.IsRunning());
}

CC_TEST(DelayTimer, RestartReusesTheLastDelayFromNow)
{
    FakeClock::Reset();
    DelayTimer timer;
    timer.Start(100);

    FakeClock::Advance(100);
    CC_CHECK(timer.Finished());

    FakeClock::Advance(500);
    timer.ReStart();
    CC_CHECK(timer.IsRunning());

    FakeClock::Advance(99);
    CC_CHECK(!timer.Finished());
    FakeClock::Advance(1);
    CC_CHECK(timer.Finished());
}

CC_TEST(DelayTimer, StopPreventsFiring)
{
    FakeClock::Reset();
    DelayTimer timer;
    timer.Start(20);
    timer.Stop();

    CC_CHECK(!timer.IsRunning());
    FakeClock::Advance(1000);
    CC_CHECK(!timer.Finished());
}
