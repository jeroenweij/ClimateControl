/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace Boot
{
    // Minimal polled USART driver with hardware RS485 driver-enable, for the
    // bootloader only. Bypasses Lib/HAL/Uart so the ~17 KB STM32Cube UART
    // driver stays out of the 10 KB bootloader image; the running app uses
    // Hal::Uart as normal.
    //
    // Which USART depends on the provisioned module type
    // (ControllerNode-Thermostat-Link-Spec.md §5.5):
    //   Thermostat      -> USART2  PA2 TX / PA3 RX / PA1 DE   (all AF1)  -- the
    //                      point-to-point link to its ControllerNode
    //   everything else -> USART1  PB6 TX / PB7 RX / PA12 DE  (AF0/AF0/AF1) -- the
    //                      RS485 main bus
    // Half-duplex 2-wire RS485 with hardware DE in both cases; identical framing
    // and baud. See Lib/Board/BoardPins.h.
    class OtaUart
    {
      public:
        enum class Bus : uint8_t
        {
            Usart1, // main bus
            Usart2, // Thermostat link
        };

        // module is the ConfigStore module byte (4 = Thermostat -> USART2).
        void Init(const uint32_t baudRate, const uint8_t module);

        bool    Available() const;
        uint8_t Read();

        // Blocks until the last byte has left the shift register (so DE has
        // de-asserted before we start listening again).
        void Write(const uint8_t* const data, const size_t len);

      private:
        Bus bus = Bus::Usart1;
    };
} // namespace Boot
