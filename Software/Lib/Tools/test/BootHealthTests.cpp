/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "FakeBackup.h"
#include "FakeClock.h"
#include "Test.h"

#include "BootHealth.h"

// Note: ConfirmBootHealthy() latches internally (it must only ever clear the
// counter once per real boot) -- that latch has no test-only reset, since
// it isn't a fake, it's the production one-shot-per-boot behaviour itself.
// So it's exercised from exactly one test below, in one continuous sequence,
// rather than split across tests that could run in either order.
namespace
{
    void ResetWorld()
    {
        FakeBackup::Reset();
        FakeClock::Reset();
    }
} // namespace

CC_TEST(BootHealth, TooManyFailedBootsAllowsAFewAttemptsBeforeGivingUp)
{
    ResetWorld();

    for (int i = 0; i < 5; i++)
    {
        CC_CHECK(!Tools::BootHealth::TooManyFailedBoots());
    }
    CC_CHECK(Tools::BootHealth::TooManyFailedBoots()); // 6th attempt -- over budget
}

CC_TEST(BootHealth, ResetFailedBootCountGivesAFreshImageAFullBudgetAgain)
{
    ResetWorld();

    for (int i = 0; i < 6; i++)
    {
        Tools::BootHealth::TooManyFailedBoots();
    }
    CC_CHECK(Tools::BootHealth::TooManyFailedBoots()); // still exhausted

    Tools::BootHealth::ResetFailedBootCount(); // a newly-received image starts clean

    for (int i = 0; i < 5; i++)
    {
        CC_CHECK(!Tools::BootHealth::TooManyFailedBoots());
    }
}

CC_TEST(BootHealth, ConfirmBootHealthyOnlyClearsAfterTheHealthyWindowElapses)
{
    ResetWorld();

    Tools::BootHealth::TooManyFailedBoots(); // counter now 1

    Tools::BootHealth::ConfirmBootHealthy(); // called immediately -- far too early to clear
    for (int i = 0; i < 4; i++)
    {
        CC_CHECK(!Tools::BootHealth::TooManyFailedBoots()); // counter climbs 2..5
    }
    CC_CHECK(Tools::BootHealth::TooManyFailedBoots()); // 6th -- exhausted, confirming it was never cleared

    Tools::BootHealth::ResetFailedBootCount(); // simulate a fresh image so the counter's own state doesn't mask the next check
    FakeClock::Advance(10000);
    Tools::BootHealth::ConfirmBootHealthy(); // now past the healthy window -- clears

    for (int i = 0; i < 5; i++)
    {
        CC_CHECK(!Tools::BootHealth::TooManyFailedBoots()); // full budget again, proving it actually cleared
    }
}
