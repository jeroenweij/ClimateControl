/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "FakeNinaUart.h"

#include "NinaUart.h"

namespace
{
    const size_t cap = 16384;

    uint8_t txBuf[cap];
    size_t  txLen = 0;
    uint8_t rxBuf[cap];
    size_t  rxHead = 0;
    size_t  rxTail = 0;
} // namespace

namespace FakeNinaUart
{
    void Reset()
    {
        txLen  = 0;
        rxHead = 0;
        rxTail = 0;
    }

    const uint8_t* Tx()
    {
        return txBuf;
    }

    size_t TxLen()
    {
        return txLen;
    }

    void TruncateTx(const size_t length)
    {
        if (length < txLen)
        {
            txLen = length;
        }
    }

    void InjectRx(const uint8_t* const data, const size_t length)
    {
        for (size_t i = 0; i < length && rxTail < cap; i++)
        {
            rxBuf[rxTail++] = data[i];
        }
    }
} // namespace FakeNinaUart

void Boot::NinaUart::Init(const uint32_t)
{
    FakeNinaUart::Reset();
}

bool Boot::NinaUart::Available() const
{
    return rxHead != rxTail;
}

uint8_t Boot::NinaUart::ReadByte()
{
    return rxBuf[rxHead++];
}

void Boot::NinaUart::WriteBytes(const uint8_t* const data, const size_t len)
{
    for (size_t i = 0; i < len && txLen < cap; i++)
    {
        txBuf[txLen++] = data[i];
    }
}
