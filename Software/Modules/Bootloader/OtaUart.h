/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace Boot
{
    // Minimal USART driver with hardware RS485 driver-enable, for the
    // bootloader only. Bypasses Lib/HAL/Uart so the ~17 KB STM32Cube UART
    // driver stays out of the 10 KB bootloader image; the running app uses
    // Hal::Uart as normal.
    //
    // RX is interrupt-driven, backed by a small ring buffer (OtaUart.cpp) --
    // found on the bench that a plain poll-only receiver (checking ISR/RDR
    // straight from Available()) could miss a byte that landed while
    // FirmwareSlave::Loop() was off doing something else for a stretch (a
    // multi-page flash erase/program run being the main one), corrupting
    // that frame with no way to catch up before the next byte overwrote the
    // single-deep hardware RDR. TX is still a plain blocking loop -- SendFrame()
    // building a whole frame before ever calling Write() means there's nothing
    // for TX buffering to overlap with here, unlike Hal::Uart's non-blocking
    // WriteBytes() (which exists so NodeLib callers on the app side never block
    // their own send path on the wire).
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

        bool    Available();
        uint8_t Read();

        // Blocks until the last byte has left the shift register (so DE has
        // de-asserted before we start listening again).
        void Write(const uint8_t* const data, const size_t len);

      private:
        Bus bus = Bus::Usart1;
    };
} // namespace Boot
