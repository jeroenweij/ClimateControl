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
    // Each line carries its own alternate-function number: on the STM32G031F8
    // there is no single AF that covers TX/RX and DE together (bus USART1: TX/RX
    // are AF0 on PB6/PB7, DE is AF1 on PA12; link USART2: all three AF1).
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

    // Wraps a USART configured for RS485 half-duplex with hardware Driver-Enable
    // (DE) timing, per Node-Bus-Hardware-Design-Spec.md §6 and
    // RS485-Node-Protocol-Spec-STM32G030.md §2/§9 -- no manual setEnable()/delay()
    // GPIO toggling, the peripheral handles DE assertion/de-assertion.
    //
    // Two instances coexist on a ControllerNode: USART1 for the main bus and
    // USART2 for the point-to-point Thermostat link
    // (ControllerNode-Thermostat-Link-Spec.md §5).
    class Uart
    {
      public:
        enum class Instance : uint8_t
        {
            Usart1, // RS485 main bus
            Usart2, // ControllerNode<->Thermostat link
        };

        Uart();

        void Init(const uint32_t baudRate, const Instance instance, const UartPins& pins);

        bool    Available() const;
        uint8_t ReadByte();
        void    WriteBytes(const uint8_t* const data, const size_t len);

      private:
        Instance instance = Instance::Usart1;
    };
} // namespace Hal
