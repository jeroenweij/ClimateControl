/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "Pin.h"

namespace Hal
{
    // Pin assignment is board-specific -- supplied by the caller (see
    // Lib/Board/BoardPins.h) rather than hardcoded in this HAL layer.
    //
    // Each line carries its own alternate-function number: on the STM32G030F6
    // there is no single AF that covers TX/RX and DE together (TX/RX are AF0 on
    // PB6/PB7, DE is AF1 on PA12).
    struct UartPin
    {
        Pin     pin;
        uint8_t alternateFunction;
    };

    struct UartPins
    {
        UartPin tx;
        UartPin rx;
        UartPin de;
    };

    // Wraps USART1 configured for RS485 half-duplex with hardware Driver-Enable
    // (DE) timing, per Node-Bus-Hardware-Design-Spec.md Sec6 and
    // RS485-Node-Protocol-Spec-STM32G030.md Sec2/Sec8 -- no manual setEnable()/delay()
    // GPIO toggling, the peripheral handles DE assertion/de-assertion automatically.
    class Uart
    {
      public:
        Uart();

        void Init(const uint32_t baudRate, const UartPins& pins);

        bool    Available() const;
        uint8_t ReadByte();
        void    WriteBytes(const uint8_t* const data, const size_t len);
    };
} // namespace Hal
