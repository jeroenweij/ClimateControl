/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "Crc.h"
#include "Frame.h"

// The server's side of the uplink socket for tests: encodes/decodes NodeLib
// frames without going through Frame::Write (which targets a Hal::Uart).
namespace uplinkframes
{
    inline size_t Encode(const NodeLib::Message& m, uint8_t* const out)
    {
        out[0] = 0xEE;
        out[1] = 0x42;
        out[2] = m.len;
        out[3] = m.id.node;
        out[4] = static_cast<uint8_t>(m.id.endpoint);
        out[5] = static_cast<uint8_t>(m.id.operation);
        for (uint8_t i = 0; i < m.len; i++)
        {
            out[6 + i] = m.data[i];
        }
        Hal::Crc       crc;
        const uint16_t c = crc.Compute(&out[3], static_cast<size_t>(3 + m.len));
        out[6 + m.len]   = static_cast<uint8_t>(c);
        out[7 + m.len]   = static_cast<uint8_t>(c >> 8);
        return static_cast<size_t>(8 + m.len);
    }

    // Decodes a byte stream into messages; returns how many were completed.
    inline int Decode(const uint8_t* const bytes, const size_t len, NodeLib::Message* const out, const int maxOut)
    {
        Hal::Crc       crc;
        NodeLib::Frame frame(crc);
        int            n = 0;
        for (size_t i = 0; i < len && n < maxOut; i++)
        {
            NodeLib::Message m;
            if (frame.FeedByte(bytes[i], m))
            {
                out[n++] = m;
            }
        }
        return n;
    }
} // namespace uplinkframes
