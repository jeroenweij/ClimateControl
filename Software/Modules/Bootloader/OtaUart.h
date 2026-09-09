/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace Boot
{
    // Minimal polled USART1 driver with hardware RS485 driver-enable, for the
    // bootloader only. Bypasses Lib/HAL/Uart so the ~17 KB STM32Cube UART
    // driver stays out of the 8 KB bootloader image; the running app uses
    // Hal::Uart as normal.
    //
    // Pins are USART1 on the Main/Node boards: PB6 TX (AF0), PB7 RX (AF0),
    // PA12 DE (AF1) -- see Lib/Board/BoardPins.h.
    class OtaUart
    {
      public:
        void Init(const uint32_t baudRate);

        bool    Available() const;
        uint8_t Read();

        // Blocks until the last byte has left the shift register (so DE has
        // de-asserted before we start listening again).
        void Write(const uint8_t* const data, const size_t len);
    };
} // namespace Boot
