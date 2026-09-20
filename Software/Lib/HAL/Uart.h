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
    //
    // Both directions are interrupt-driven, backed by a small per-instance
    // ring buffer each (see Uart.cpp) -- not polled from Loop() at all. RX:
    // Init() enables RXNEIE once and leaves it on; USARTx_IRQHandler empties
    // RDR into the RX ring buffer the moment each byte lands, so a byte
    // survives however long the main loop takes to get back around to
    // Available()/ReadByte() (bounded by the ring buffer's depth, not by a
    // single-byte hardware window -- what actually caused dropped bus bytes
    // this project hit earlier: something elsewhere in the shared super-loop,
    // e.g. a blocking NINA write or bit-banged log line, ran long enough to
    // miss the old single-byte-deep RDR). TX: WriteBytes() copies into the TX
    // ring buffer and arms TXEIE; USARTx_IRQHandler feeds TDR one byte at a
    // time and disables TXEIE once the buffer drains. RS485 DE assertion
    // stays peripheral hardware driven off transmit-register activity
    // (DEAT/DEDT, set in Init()) either way, interrupt- or polling-fed.
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

        // Non-blocking; reads straight from the RX ring buffer the ISR fills.
        bool    Available() const;
        uint8_t ReadByte();

        // Queues 'data' for transmission and returns immediately -- never
        // blocks on the wire (contrast the old HAL_UART_Transmit(...,
        // HAL_MAX_DELAY) behaviour, which stalled the caller for the whole
        // transfer). Atomic: if 'len' doesn't fully fit in the TX ring
        // buffer, nothing is queued (never emits a half-written, unparseable
        // frame) -- returns false, caller's own retry/backstop policy (e.g.
        // NodeLib's queue-drop, MainController-Server-Link-Spec.md §7.2)
        // applies same as any other dropped message.
        bool WriteBytes(const uint8_t* const data, const size_t len);

        // Blocks until every queued byte has actually finished shifting out
        // on the wire (TX ring buffer empty *and* the hardware's Transmission
        // Complete flag set -- TDR-empty alone just means the last byte was
        // handed to the shift register, not that it's done transmitting).
        // Needed before anything that kills the peripheral outright (e.g.
        // Hal::System::Reset()) -- WriteBytes() only queues, so code that
        // "waits until after the last WriteMessage() call" before resetting
        // does not actually wait for the bytes to reach the wire; see
        // Node::PerformPendingReset().
        void FlushTx() const;

      private:
        Instance instance = Instance::Usart1;
    };
} // namespace Hal
