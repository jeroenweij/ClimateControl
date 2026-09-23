/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "OtaUart.h"

#include "FakeOtaUart.h"

using Boot::OtaUart;

namespace
{
    const size_t bufferSize = 4096;

    uint8_t txBuffer[bufferSize];
    size_t  txLength = 0;

    uint8_t rxBuffer[bufferSize];
    size_t  rxLength = 0;
    size_t  rxCursor = 0;
} // namespace

namespace FakeOtaUart
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

    void InjectRx(const uint8_t* const data, const size_t length)
    {
        for (size_t i = 0; i < length && rxLength < bufferSize; i++)
        {
            rxBuffer[rxLength++] = data[i];
        }
    }
} // namespace FakeOtaUart

void OtaUart::Init(const uint32_t, const uint8_t)
{
}

bool OtaUart::Available() const
{
    return rxCursor < rxLength;
}

uint8_t OtaUart::Read()
{
    return rxCursor < rxLength ? rxBuffer[rxCursor++] : 0u;
}

void OtaUart::Write(const uint8_t* const data, const size_t len)
{
    for (size_t i = 0; i < len && txLength < bufferSize; i++)
    {
        txBuffer[txLength++] = data[i];
    }
}
