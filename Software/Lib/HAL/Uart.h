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

        // Plain 2-wire UART, no RS485 DE timing -- e.g. the NINA-W152 link, whose
        // one flow-control line (Board::NinaRts) is driven as a plain GPIO rather
        // than the peripheral's hardware DE/RTS, per MainController-Spec.md §5.
        void Init(const uint32_t baudRate, const Instance instance, const UartPin& tx, const UartPin& rx);

        bool    Available() const;
        uint8_t ReadByte();

        // Queues 'data' for transmission and returns immediately -- never
        // blocks on the wire (contrast the old HAL_UART_Transmit(...,
        // HAL_MAX_DELAY) behaviour, which stalled the caller for the whole
        // transfer and, since this firmware is single-threaded with no RX
        // FIFO, silently dropped bytes arriving on *any* other UART meanwhile
        // -- e.g. NINA writes blocking the bus RX poll long enough to lose a
        // node's reply). Copies into a small per-instance ring buffer; bytes
        // actually reach the wire incrementally via Pump(). Atomic: if 'len'
        // doesn't fully fit, nothing is queued (never emits a half-written,
        // unparseable frame) -- returns false, caller's own retry/backstop
        // policy (e.g. NodeLib's queue-drop, MainController-Server-Link-
        // Spec.md §7.2) applies same as any other dropped message.
        bool WriteBytes(const uint8_t* const data, const size_t len);

        // Pushes one queued byte onto the wire if the hardware's transmit
        // register is free right now; a no-op otherwise. Never blocks. Must
        // be called every Loop() iteration (Node::Loop() and NinaAt::Loop()
        // already do) for queued bytes to actually go out -- RS485 DE
        // assertion is peripheral hardware driven off transmit-register
        // activity (DEAT/DEDT, set in Init()), not this function, so feeding
        // one byte at a time here keeps DE correctly asserted for the whole
        // frame as long as Pump() is called well within one byte time of the
        // previous byte (comfortably true for an idle super-loop; a blocking
        // call elsewhere in the loop, e.g. bit-banged console logging, can
        // still starve this -- see Tools::Logger's board-specific backends).
        void Pump();

      private:
        Instance instance = Instance::Usart1;
    };
} // namespace Hal
