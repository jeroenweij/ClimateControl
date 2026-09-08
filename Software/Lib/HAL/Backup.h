/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

namespace Hal
{
    // RTC/TAMP backup registers -- retained across a warm reset (NRST, software
    // reset, IWDG) but cleared on power-on / brown-out. Used for the
    // app <-> bootloader handoff, see
    // Spec/Node-Flash-Layout-and-Bootloader-Spec.md §2/§5.
    //
    // No VBAT battery is fitted, so these deliberately do NOT survive the
    // bus ENABLE-line power cut.
    namespace Backup
    {
        enum class Reg : uint8_t
        {
            Boot        = 0, // TAMP->BKP0R -- Board::EnterBootloaderMagic
            BootCounter = 1, // TAMP->BKP1R -- optional boot-fail counter
        };

        // Enables backup-domain write access on first use.
        uint32_t Read(Reg reg);
        void     Write(Reg reg, uint32_t value);
    } // namespace Backup
} // namespace Hal
