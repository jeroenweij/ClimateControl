/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include "Crc.h"
#include "DelayTimer.h"
#include "Uart.h"

#include "Id.h"

namespace NodeLib
{
    // Bus-health tallies surfaced on Endpoint::DiagRxCounters
    // (Spec/Node-Message-Model-Spec.md §3).
    struct RxCounters
    {
        uint32_t frames; // CRC-valid frames decoded
        uint32_t crcErrors; // frames dropped on CRC mismatch
        uint32_t resyncs; // partial frame abandoned (bad LEN / spurious sync)
        uint32_t interByteTimeouts; // mid-frame gap exceeded the resync timeout (§5)
    };

    // Byte-stream framer/deframer for the v2 wire format
    // (RS485-Node-Protocol-Spec-STM32G030.md §3): SYNC(2) LEN(1) NODE_ID(1)
    // ENDPOINT(1) OPERATION(1) DATA(LEN) CRC16(2, little-endian). Replaces v1's
    // fixed-size Frame struct + raw Serial1.write((uint8_t*)&m, sizeof(m)), per
    // the protocol spec's migration notes §8.
    class Frame
    {
      public:
        // SYNC(2) LEN(1) NODE ENDPOINT OPERATION(3) DATA(MAX_DATA) CRC16(2)
        static const size_t MaxFrameBytes = 2 + 1 + 3 + MAX_DATA + 2;

        Frame(Hal::Crc& crc);

        // Feed one received byte into the parser. Returns true once a complete,
        // CRC-valid message has been decoded into 'message'. Call once per byte
        // read from Hal::Uart.
        bool FeedByte(const uint8_t byte, Message& message);

        // Must be called regularly (e.g. once per Node::Loop() iteration) even
        // when no byte has arrived, so the inter-byte timeout (spec §5) can fire
        // and resync a wedged parser.
        void Update();

        // Serialises 'message' into 'out' -- SYNC LEN NODE ENDPOINT OPERATION
        // DATA CRC16, the v2 wire format -- and returns the byte count (at most
        // MaxFrameBytes). No hardware involved besides the CRC unit, so
        // anything that can push bytes (a Hal::Uart, the bootloader's own
        // NinaUart, a test fake) can transmit a frame with it.
        size_t Encode(const Message& message, uint8_t* const out) const;

        // Returns false if the underlying Hal::Uart::WriteBytes() rejected the
        // frame (its TX ring buffer didn't have room) -- queues nothing on
        // failure, same atomic-reject semantics as WriteBytes() itself.
        // Callers that can usefully react to a drop (e.g. log it) should
        // check this; callers that can't (most of NodeLib's own internal
        // sends) are free to ignore it, same as before this was added.
        bool Write(Hal::Uart& uart, const Message& message) const;

        const RxCounters& Counters() const
        {
            return counters;
        }

        // Endpoint::DiagReset -- zero the RX tallies.
        void ResetCounters()
        {
            counters = RxCounters{};
        }

      private:
        enum class State
        {
            Sync0,
            Sync1,
            Len,
            Header,
            Data,
            Crc0,
            Crc1,
        };

        void ResetToSync();

        Hal::Crc& crc;
        State     state;
        uint8_t   len;
        uint8_t   dataIndex;
        // node, channel, operation, then data[] -- CRC is computed over exactly
        // this buffer, per spec §4 ("NODE_ID..DATA inclusive").
        uint8_t           headerAndData[3 + MAX_DATA];
        uint8_t           headerIndex;
        uint16_t          receivedCrc;
        Tools::DelayTimer interByteTimer;
        RxCounters        counters;
    };
} // namespace NodeLib
