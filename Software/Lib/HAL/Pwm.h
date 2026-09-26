/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

#include "Pin.h"

namespace Hal
{
    struct PwmPins
    {
        Pin     out;
        uint8_t alternateFunction;
    };

    // Single-channel PWM output on TIM3 channel 1, with a 1 us tick -- the one
    // PWM output this project has, the ControllerNode's servo signal
    // (Node-Bus-Hardware-Design-Spec.md §6.2).
    //
    // No STM32Cube TIM HAL is vendored (Lib/HAL/Vendor only carries what
    // earlier peripherals needed), so this talks to the TIM3 registers
    // directly against RM0444 Sec22, the same approach Hal::Adc and Hal::I2c
    // take for their peripherals.
    //
    // The prescaler is derived from the timer clock at Init(), so the 1 us
    // tick holds at the applications' 64 MHz and at the reset-default 16 MHz.
    class Pwm
    {
      public:
        Pwm(const PwmPins& pins, const uint16_t periodUs);

        // Clocks the timer, starts the counter and hands the pin to TIM3 with
        // the output held low (pulse 0).
        void Init();

        // High time per period, in us. 0 holds the output low; values past the
        // period are clamped to it. Preloaded -- takes effect at the next
        // period boundary, so a change never produces a runt pulse.
        void SetPulseUs(const uint16_t pulseUs);

      private:
        PwmPins  pins;
        uint16_t periodUs;
    };
} // namespace Hal
