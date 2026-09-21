/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "Backup.h"
#include "Tick.h"

#include "BootHealth.h"

namespace
{
    // A handful of attempts before giving up on an image that never proves
    // itself healthy -- generous enough to ride out a one-off fluke, tight
    // enough that a genuinely crash-looping image doesn't sit bricked for
    // long before the bootloader takes it back.
    constexpr uint32_t maxFailedBoots = 5;

    // Long enough that reaching it is real evidence of a working app (past
    // any immediate-HardFault-on-boot failure mode), short enough that a
    // healthy node isn't carrying a stale failure count for long after a
    // genuine transient reset.
    constexpr uint32_t healthyAfterMs = 10000;

    bool confirmedHealthy = false;
} // namespace

bool Tools::BootHealth::TooManyFailedBoots()
{
    const uint32_t attempts = Hal::Backup::Read(Hal::Backup::Reg::BootCounter) + 1;
    Hal::Backup::Write(Hal::Backup::Reg::BootCounter, attempts);
    return attempts > maxFailedBoots;
}

void Tools::BootHealth::ResetFailedBootCount()
{
    Hal::Backup::Write(Hal::Backup::Reg::BootCounter, 0);
}

void Tools::BootHealth::ConfirmBootHealthy()
{
    if (confirmedHealthy)
    {
        return;
    }
    if (Hal::Tick::Millis() < healthyAfterMs)
    {
        return;
    }
    ResetFailedBootCount();
    confirmedHealthy = true;
}
