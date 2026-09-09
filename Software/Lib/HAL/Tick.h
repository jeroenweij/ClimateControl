/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

namespace Hal
{
    namespace Tick
    {
        uint32_t Millis();
        void     DelayMs(const uint32_t ms);

        // Busy-wait for at least 'us' microseconds, derived from the SysTick
        // down-counter so it stays correct with interrupts masked (used by the
        // 1-Wire bit-bang, Hal::OneWire). Not for long delays -- a single call
        // must stay well under the 1 ms SysTick reload.
        void DelayUs(const uint32_t us);
    } // namespace Tick
} // namespace Hal
