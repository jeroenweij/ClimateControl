/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "Crc.h"
#include "Frame.h"
#include "Uart.h"

#include "BusHelpers.h"
#include "FakeBus.h"

using NodeLib::Frame;
using NodeLib::Message;

namespace bus
{
    void InjectFrame(const Message& message)
    {
        Hal::Crc  crc;
        Frame     frame(crc);
        Hal::Uart uart;

        const size_t before = FakeBus::TxLen();
        frame.Write(uart, message);
        const size_t after = FakeBus::TxLen();

        FakeBus::InjectRx(FakeBus::Tx() + before, after - before);
        FakeBus::TruncateTx(before);
    }

    int DecodeTx(Message* const out, const int maxOut)
    {
        Hal::Crc crc;
        Frame    frame(crc);

        const uint8_t* const tx  = FakeBus::Tx();
        const size_t         len = FakeBus::TxLen();

        int count = 0;
        for (size_t i = 0; i < len && count < maxOut; i++)
        {
            if (frame.FeedByte(tx[i], out[count]))
            {
                count++;
            }
        }
        return count;
    }
} // namespace bus
