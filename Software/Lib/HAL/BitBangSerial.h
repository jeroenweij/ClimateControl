/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

#include "Gpio.h"
#include "Pin.h"

namespace Hal
{
    // Bit-bang, transmit-only UART (8N1, LSB first) on one push-pull GPIO --
    // for a debug log line where no hardware USART is free (MainController-
    // Spec.md §5: both on-chip USARTs are already committed to the bus and
    // NINA). Busy-waits the whole line via Hal::Tick::DelayUs(), same
    // primitive Hal::OneWire bit-bangs with.
    //
    // A modest baud (9600 default) is deliberate: at 1 us integer resolution,
    // 115200's ~8.68 us bit period rounds to 8 us and drifts close to a full
    // bit by the stop bit, where 9600's ~104.17 us rounds to 104 us with
    // negligible drift.
    class BitBangSerial
    {
      public:
        explicit BitBangSerial(const Pin pin, const uint32_t baudRate = 9600);

        void WriteByte(const uint8_t byte);
        void WriteString(const char* const text);

      private:
        Gpio     gpio;
        uint32_t bitPeriodUs;
    };
} // namespace Hal
