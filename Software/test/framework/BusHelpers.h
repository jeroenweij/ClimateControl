/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stddef.h>

#include "Id.h" // NodeLib::Message

// Frame-level helpers over FakeBus, using the real NodeLib::Frame codec and
// the software Hal::Crc. Compiled into every test executable by the test
// framework target.
namespace bus
{
    // Encode 'message' with a real Frame and queue it as bytes for the code
    // under test to receive.
    void InjectFrame(const NodeLib::Message& message);

    // Decode every complete, CRC-valid frame currently in FakeBus::Tx() into
    // 'out' (up to 'maxOut'). Returns how many were decoded.
    int DecodeTx(NodeLib::Message* out, int maxOut);
} // namespace bus
