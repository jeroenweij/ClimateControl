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
    // Byte-stream framer/deframer for the v2 wire format
    // (RS485-Node-Protocol-Spec-STM32G030.md §3): SYNC(2) LEN(1) NODE_ID(1)
    // CHANNEL(1) OPERATION(1) DATA(LEN) CRC16(2, little-endian). Replaces v1's
    // fixed-size Frame struct + raw Serial1.write((uint8_t*)&m, sizeof(m)), per
    // the protocol spec's migration notes §8.
    class Frame
    {
      public:
        Frame(Hal::Crc& crc);

        // Feed one received byte into the parser. Returns true once a complete,
        // CRC-valid message has been decoded into 'message'. Call once per byte
        // read from Hal::Uart.
        bool FeedByte(const uint8_t byte, Message& message);

        // Must be called regularly (e.g. once per Node::Loop() iteration) even
        // when no byte has arrived, so the inter-byte timeout (spec §5) can fire
        // and resync a wedged parser.
        void Update();

        void Write(Hal::Uart& uart, const Message& message) const;

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
    };
} // namespace NodeLib
