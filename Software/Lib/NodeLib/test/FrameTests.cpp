/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "Crc.h"
#include "Frame.h"
#include "Uart.h"

#include "FakeBus.h"
#include "FakeClock.h"
#include "Test.h"

using NodeLib::Endpoint;
using NodeLib::Frame;
using NodeLib::Message;
using NodeLib::Operation;

namespace
{
    // Feed a byte buffer through the parser, returning the first fully decoded
    // message (if any) via 'out'.
    bool FeedAll(Frame& frame, const uint8_t* data, size_t len, Message& out)
    {
        for (size_t i = 0; i < len; i++)
        {
            if (frame.FeedByte(data[i], out))
            {
                return true;
            }
        }
        return false;
    }
} // namespace

CC_TEST(Frame, EncodeThenDecodeRoundTrips)
{
    FakeBus::Reset();
    FakeClock::Reset();

    Hal::Crc  crc;
    Frame     frame(crc);
    Hal::Uart uart;

    Message tx(9, Endpoint::RoomTemp, Operation::Report, 0);
    tx.len     = 4;
    tx.data[0] = 0xDE;
    tx.data[1] = 0xAD;
    tx.data[2] = 0xBE;
    tx.data[3] = 0xEF;
    frame.Write(uart, tx);

    Message rx;
    CC_CHECK(FeedAll(frame, FakeBus::Tx(), FakeBus::TxLen(), rx));
    CC_CHECK_EQ(rx.id.node, 9);
    CC_CHECK(rx.id.endpoint == Endpoint::RoomTemp);
    CC_CHECK(rx.id.operation == Operation::Report);
    CC_CHECK_EQ(rx.len, 4);
    CC_CHECK_EQ(rx.data[0], 0xDE);
    CC_CHECK_EQ(rx.data[3], 0xEF);
    CC_CHECK_EQ(frame.Counters().frames, 1);
}

CC_TEST(Frame, ZeroLengthPayloadRoundTrips)
{
    FakeBus::Reset();
    FakeClock::Reset();

    Hal::Crc  crc;
    Frame     frame(crc);
    Hal::Uart uart;

    const Message tx(1, Operation::Poll);
    frame.Write(uart, tx);

    Message rx;
    CC_CHECK(FeedAll(frame, FakeBus::Tx(), FakeBus::TxLen(), rx));
    CC_CHECK_EQ(rx.len, 0);
    CC_CHECK(rx.id.endpoint == Endpoint::Transport);
    CC_CHECK(rx.id.operation == Operation::Poll);
}

CC_TEST(Frame, CorruptedPayloadFailsCrcAndIsCounted)
{
    FakeBus::Reset();
    FakeClock::Reset();

    Hal::Crc  crc;
    Frame     frame(crc);
    Hal::Uart uart;

    frame.Write(uart, Message(4, Endpoint::DamperTarget, Operation::Set, 0x55));

    uint8_t      wire[64];
    const size_t len = FakeBus::TxLen();
    for (size_t i = 0; i < len; i++)
    {
        wire[i] = FakeBus::Tx()[i];
    }
    wire[5] ^= 0xFF; // flip a header/data byte, leave the CRC bytes intact

    Message rx;
    CC_CHECK(!FeedAll(frame, wire, len, rx));
    CC_CHECK_EQ(frame.Counters().crcErrors, 1);
    CC_CHECK_EQ(frame.Counters().frames, 0);
}

CC_TEST(Frame, OversizedLengthTriggersResyncThenRecovers)
{
    FakeBus::Reset();
    FakeClock::Reset();

    Hal::Crc crc;
    Frame    frame(crc);

    // SYNC SYNC LEN=33 (> MAX_DATA) -> abandoned.
    const uint8_t bogus[3] = {0xEE, 0x42, 33};
    Message       rx;
    CC_CHECK(!FeedAll(frame, bogus, sizeof(bogus), rx));
    CC_CHECK_EQ(frame.Counters().resyncs, 1);

    // A good frame right after still decodes.
    Hal::Uart uart;
    frame.Write(uart, Message(2, Endpoint::SupplyTemp, Operation::Report, 0x11));
    CC_CHECK(FeedAll(frame, FakeBus::Tx(), FakeBus::TxLen(), rx));
    CC_CHECK_EQ(rx.id.node, 2);
    CC_CHECK_EQ(frame.Counters().frames, 1);
}

CC_TEST(Frame, DecodesAKnownWireVector)
{
    FakeClock::Reset();

    Hal::Crc crc;
    Frame    frame(crc);

    // EE 42 | LEN=2 | node=3, endpoint=0x30, op=0x03 | data C4 09 | CRC16 LE.
    // CRC-16/CCITT-FALSE over {03,30,03,C4,09} == 0xC1DE  ->  bytes DE C1.
    const uint8_t vector[10] = {0xEE, 0x42, 0x02, 0x03, 0x30, 0x03, 0xC4, 0x09, 0xDE, 0xC1};

    Message rx;
    CC_CHECK(FeedAll(frame, vector, sizeof(vector), rx));
    CC_CHECK_EQ(rx.id.node, 3);
    CC_CHECK(rx.id.endpoint == Endpoint::DamperTarget);
    CC_CHECK(rx.id.operation == Operation::Report);
    CC_CHECK_EQ(rx.len, 2);
    CC_CHECK_EQ(rx.data[0], 0xC4);
    CC_CHECK_EQ(rx.data[1], 0x09);
}

CC_TEST(Frame, InterByteTimeoutResyncsAWedgedParser)
{
    FakeBus::Reset();
    FakeClock::Reset();

    Hal::Crc crc;
    Frame    frame(crc);

    Message       rx;
    const uint8_t partial[3] = {0xEE, 0x42, 0x04};
    FeedAll(frame, partial, sizeof(partial), rx);

    FakeClock::Advance(5); // past the ~2 ms inter-byte timeout
    frame.Update();
    CC_CHECK_EQ(frame.Counters().interByteTimeouts, 1);

    // Parser is back at SYNC: a fresh frame decodes.
    Hal::Uart uart;
    frame.Write(uart, Message(6, Operation::Done));
    CC_CHECK(FeedAll(frame, FakeBus::Tx(), FakeBus::TxLen(), rx));
    CC_CHECK_EQ(rx.id.node, 6);
}
