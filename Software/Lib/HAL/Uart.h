/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "Pin.h"

namespace Hal
{
    // Pin assignment is board-specific and not yet fixed by any schematic, so it's
    // supplied by the caller rather than hardcoded here.
    struct UartPins
    {
        Pin     tx;
        Pin     rx;
        Pin     de;
        uint8_t alternateFunction;
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
