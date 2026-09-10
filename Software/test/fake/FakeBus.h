/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stddef.h>
#include <stdint.h>

// A single shared RS485 segment behind every Hal::Uart instance in the test:
// bytes written by the code under test land in Tx(), bytes queued with
// InjectRx() are handed back by Hal::Uart::ReadByte(). Frame-level helpers
// (encode a NodeLib::Message onto the wire, decode the wire back) live in
// the test framework's BusHelpers, not here.
namespace FakeBus
{
    void Reset();

    const uint8_t* Tx();
    size_t         TxLen();
    void           TruncateTx(size_t length);

    void InjectRx(const uint8_t* data, size_t length);
} // namespace FakeBus
