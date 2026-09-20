/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "Uart.h"

#include "FakeBus.h"

using Hal::Uart;
using Hal::UartPins;

namespace
{
    const size_t bufferSize = 4096;

    uint8_t txBuffer[bufferSize];
    size_t  txLength = 0;

    uint8_t rxBuffer[bufferSize];
    size_t  rxLength = 0;
    size_t  rxCursor = 0;
} // namespace

namespace FakeBus
{
    void Reset()
    {
        txLength = 0;
        rxLength = 0;
        rxCursor = 0;
    }

    const uint8_t* Tx()
    {
        return txBuffer;
    }

    size_t TxLen()
    {
        return txLength;
    }

    void TruncateTx(const size_t length)
    {
        if (length <= txLength)
        {
            txLength = length;
        }
    }

    void InjectRx(const uint8_t* const data, const size_t length)
    {
        for (size_t i = 0; i < length && rxLength < bufferSize; i++)
        {
            rxBuffer[rxLength++] = data[i];
        }
    }
} // namespace FakeBus

Uart::Uart()
{
}

void Uart::Init(const uint32_t, const Instance, const UartPins&)
{
}

bool Uart::Available() const
{
    return rxCursor < rxLength;
}

uint8_t Uart::ReadByte()
{
    return rxCursor < rxLength ? rxBuffer[rxCursor++] : 0u;
}

bool Uart::WriteBytes(const uint8_t* const data, const size_t len)
{
    // Unlike the real Hal::Uart (interrupt-drained), this writes straight
    // into the fake bus synchronously -- FakeBus::Tx()/TxLen() are meant to
    // see the bytes immediately after Write() returns, and the 4096-byte
    // buffer is never realistically exhausted by a test.
    if (len > bufferSize - txLength)
    {
        return false;
    }
    for (size_t i = 0; i < len; i++)
    {
        txBuffer[txLength++] = data[i];
    }
    return true;
}

void Uart::FlushTx() const
{
    // WriteBytes() above is already synchronous (no ring buffer/ISR to
    // drain), so there's nothing in flight to wait for here.
}
