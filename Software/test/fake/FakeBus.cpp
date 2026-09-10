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

void Uart::WriteBytes(const uint8_t* const data, const size_t len)
{
    for (size_t i = 0; i < len && txLength < bufferSize; i++)
    {
        txBuffer[txLength++] = data[i];
    }
}
