/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include <string.h>

#include "EEndpoint.h"
#include "EOperation.h"
#include "Frame.h"
#include "Id.h"

#include "NinaLink.h"
#include "NinaPort.h"

#include "Crc.h"
#include "FakeClock.h"
#include "Test.h"

using NodeLib::Endpoint;
using NodeLib::Id;
using NodeLib::Message;
using NodeLib::Operation;

// NinaLink's outbound drain against a port with a bounded TX buffer, like the
// application's 128-byte Hal::Uart ring: a burst bigger than the buffer must
// come out whole and in order over several passes, never silently dropped.
struct NinaLinkTestAccess
{
    static void Drain(NinaLink& link)
    {
        link.DrainOutboundQueue();
    }
};

namespace
{
    class BoundedPort : public NinaPort
    {
      public:
        static const size_t capacity = 127; // Hal::Uart: bufferSize - 1 usable

        void Init(const uint32_t) override
        {
        }
        bool Available() const override
        {
            return false;
        }
        uint8_t ReadByte() override
        {
            return 0;
        }
        bool WriteBytes(const uint8_t* const data, const size_t len) override
        {
            if (len > capacity - pending)
            {
                return false; // refused whole, like Hal::Uart::WriteBytes
            }
            memcpy(&sent[sentLen], data, len);
            sentLen += len;
            pending += len;
            return true;
        }
        void Flush() override
        {
        }

        // The UART shifting everything out.
        void Transmit()
        {
            pending = 0;
        }

        uint8_t sent[4096];
        size_t  sentLen = 0;
        size_t  pending = 0;
    };

    struct NullHandler : NinaLinkHandler
    {
        void BuildHello(Message& hello) override
        {
            hello = Message(Id(0, Endpoint::UplinkHello, Operation::Report));
        }
        void OnFrame(const Message&) override
        {
        }
    };

    // A MainLog-sized frame (3-byte uptime + ~32 characters) tagged with 'n'.
    Message LogFrame(const uint8_t n)
    {
        Message m(Id(0, Endpoint::MainLog, Operation::Report));
        m.len = 35;
        for (uint8_t i = 0; i < m.len; i++)
        {
            m.data[i] = n;
        }
        return m;
    }

    // Decodes every frame the port has been handed, in order.
    int Decode(const BoundedPort& port, Message* const out, const int maxOut)
    {
        Hal::Crc       crc;
        NodeLib::Frame frame(crc);
        int          n = 0;
        for (size_t i = 0; i < port.sentLen && n < maxOut; i++)
        {
            if (frame.FeedByte(port.sent[i], out[n]))
            {
                n++;
            }
        }
        return n;
    }
} // namespace

CC_TEST(NinaLinkQueue, ABurstBiggerThanTheTxBufferIsSentWholeAndInOrder)
{
    FakeClock::Reset();
    BoundedPort    port;
    NullHandler    handler;
    NinaLinkConfig config{"ssid", "psk", "host", 9000};
    Message        queue[16];
    NinaLink       link(port, config, handler, queue, 16);

    for (uint8_t i = 1; i <= 8; i++) // ~8 x 43 bytes -- nearly three buffers' worth
    {
        CC_CHECK(link.Send(LogFrame(i)));
    }

    NinaLinkTestAccess::Drain(link);
    CC_CHECK(link.Queued() > 0); // what didn't fit is still queued, not dropped
    CC_CHECK(link.Queued() < 8);

    for (int pass = 0; pass < 8 && link.Queued() > 0; pass++)
    {
        port.Transmit();
        NinaLinkTestAccess::Drain(link);
    }
    CC_CHECK_EQ(link.Queued(), 0);

    Message   out[16];
    const int n = Decode(port, out, 16);
    CC_CHECK_EQ(n, 8);
    for (int i = 0; i < n; i++)
    {
        CC_CHECK_EQ(out[i].data[0], i + 1); // every frame, in the order queued
    }
}
