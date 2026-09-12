/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

namespace Hal
{
    // Minimal single-channel ADC1 driver: software-triggered single
    // conversion, 12-bit, polled (no DMA/interrupts). All this project needs
    // it for so far is an occasional diagnostic sample -- servo stall
    // detection, Node-Bus-Power-Path-Spec.md Sec3.1.1 -- not a real-time
    // control-loop input.
    //
    // STM32Cube's own ADC HAL driver isn't vendored (Lib/HAL/Vendor only
    // carries what earlier peripherals needed), so this talks to the ADC1
    // registers directly against RM0444 Sec15 -- the same approach
    // Hal::OneWire already takes for its own peripheral, rather than pulling
    // in the full vendor driver for one occasional 12-bit read.
    //
    // 'channel' is the ADC_IN index (e.g. PA0 = ADC_IN0 on this package), not
    // a GPIO pin. The caller is responsible for the pin being analog -- which
    // it is unless something has since reconfigured it, since analog-in is
    // every GPIO's reset-default state (RM0444 Table 11).
    class Adc
    {
      public:
        explicit Adc(const uint8_t channel);

        // Blocking single conversion. Returns the raw 12-bit result (0..4095).
        uint16_t Read();

      private:
        uint8_t channel;
    };
} // namespace Hal
