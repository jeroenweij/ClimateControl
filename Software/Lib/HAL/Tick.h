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

        // Free-running microsecond counter (Millis() scaled up, sub-ms part
        // read from the SysTick down-counter). Wraps at ~71 minutes; callers
        // compare against it with wraparound-safe signed subtraction, e.g.
        // (int32_t)(Micros() - deadline) >= 0. Use this over repeated DelayUs()
        // calls when scheduling several fixed-period steps in a row (e.g. one
        // UART bit each) -- an absolute deadline absorbs each step's own call
        // overhead into the wait, where re-arming a fresh DelayUs() after that
        // overhead stretches every period by it (a bit-banged UART at 9600
        // shows it: ~8.85 us of Gpio::Write()/loop overhead per bit pushes the
        // real bit period to ~112.85 us and garbles every byte).
        uint32_t Micros();

        // Busy-wait for at least 'us' microseconds, derived from the SysTick
        // down-counter so it stays correct with interrupts masked (used by the
        // 1-Wire bit-bang, Hal::OneWire). Not for long delays -- a single call
        // must stay well under the 1 ms SysTick reload.
        void DelayUs(const uint32_t us);
    } // namespace Tick
} // namespace Hal
