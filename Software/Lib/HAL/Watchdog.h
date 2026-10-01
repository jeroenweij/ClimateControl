/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

namespace Hal
{
    // Independent watchdog (IWDG, clocked from the ~32 kHz LSI). Once started
    // it cannot be stopped -- only a reset does that, so an app that resets
    // into the bootloader leaves it behind. A reset it causes is recorded as a
    // Hal::Fault::Code::Watchdog by Hal::Fault::Init() on the next boot.
    namespace Watchdog
    {
        // Timeout between two Feed()s before the IWDG resets the MCU. Covers
        // the longest blocking call in any application main loop (Damper::
        // ParkNeutral(), up to half a slewed stroke plus settle -- checked by a
        // static_assert there) with margin for the LSI's tolerance.
        constexpr uint32_t TimeoutMs = 4000;

        // Starts the IWDG. Frozen while the core is halted by a debugger, so
        // a J-Link session doesn't reset the board under it.
        void Start();

        // Reloads the counter; call once per main-loop iteration. Also stamps
        // the uptime into Hal::Backup::Reg::AliveUptime, which becomes the
        // Watchdog fault's uptime if the next Feed() never comes.
        void Feed();
    } // namespace Watchdog
} // namespace Hal
