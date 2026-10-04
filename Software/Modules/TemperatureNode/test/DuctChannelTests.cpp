/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "BoardPins.h"
#include "Node.h"

#include "Ds18b20.h"
#include "DuctChannel.h"
#include "TemperatureHandler.h"

#include "BusHelpers.h"
#include "FakeBus.h"
#include "FakeClock.h"
#include "FakeConfigStore.h"
#include "FakeFlash.h"
#include "FakeOneWire.h"
#include "Test.h"

using Hal::OneWire;
using NodeLib::Endpoint;
using NodeLib::Message;
using NodeLib::Node;
using NodeLib::Operation;

namespace
{
    const Hal::Pin line   = Board::OneWire2;
    const uint8_t  nodeId = 7;

    void MakeScratchpad(const int16_t raw, uint8_t out[9])
    {
        out[0] = static_cast<uint8_t>(raw & 0xFF);
        out[1] = static_cast<uint8_t>((raw >> 8) & 0xFF);
        out[2] = 0x4B;
        out[3] = 0x46;
        out[4] = 0x7F;
        out[5] = 0xFF;
        out[6] = 0x0C;
        out[7] = 0x10;
        out[8] = OneWire::Crc8(out, 8);
    }

    void ResetWorld()
    {
        FakeBus::Reset();
        FakeClock::Reset();
        FakeConfig::Reset();
        FakeConfig::SetNodeId(nodeId);
        FakeFlash::Reset();
        FakeOneWire::ResetAll();
    }

    // The master's Poll -- a slave only transmits in its own polled window,
    // and only counts as connected (its values get published) while polls
    // keep arriving less than the 1 s heartbeat apart.
    void Poll(Node& node)
    {
        bus::InjectFrame(Message(nodeId, Operation::Poll));
        node.Loop();
    }

    // Run one full sample cycle: Idle -> start conversion -> wait it out ->
    // read, polling the node throughout as a live bus would.
    void RunSampleCycle(DuctChannel& channel, Node& node)
    {
        channel.Loop(); // Idle -> Converting
        Poll(node);
        FakeClock::Advance(Ds18b20::ConversionTimeMs + 100);
        Poll(node);
        channel.Loop(); // Converting -> read + publish
        Poll(node);
        FakeClock::Advance(550); // let the sample timer expire for the next cycle
        Poll(node);
        FakeClock::Advance(550);
        Poll(node);
    }

    // Count the Reports on 'endpoint' sent since the last call (two more
    // polls first, so anything still queued goes out), returning the last
    // one's decoded int16 value.
    int ReportsOn(Node& node, const Endpoint endpoint, int16_t& lastValue)
    {
        Poll(node);
        Poll(node);

        Message   tx[32];
        const int n     = bus::DecodeTx(tx, 32);
        int       count = 0;
        for (int i = 0; i < n; i++)
        {
            if (tx[i].id.endpoint == endpoint && tx[i].id.operation == Operation::Report)
            {
                count++;
                lastValue = static_cast<int16_t>(tx[i].data[0] | (tx[i].data[1] << 8));
            }
        }
        FakeBus::TruncateTx(0);
        return count;
    }
} // namespace

CC_TEST(DuctChannel, BecomesPresentWithTheDecodedValue)
{
    ResetWorld();
    Node node;
    node.Init();

    DuctChannel channel(node, Endpoint::SupplyTemp, line);
    channel.Init();

    uint8_t scratchpad[9];
    MakeScratchpad(0x0190, scratchpad); // +25.0 degC
    FakeOneWire::SetPresent(line, true);
    FakeOneWire::QueueRead(line, scratchpad, 9);

    RunSampleCycle(channel, node);

    CC_CHECK(channel.Present());
    CC_CHECK_EQ(channel.Value(), 2500);
}

CC_TEST(DuctChannel, EmitsAReportAfterTheFirstSample)
{
    ResetWorld();
    Node node;
    node.Init();

    DuctChannel channel(node, Endpoint::SupplyTemp, line);
    channel.Init();

    uint8_t scratchpad[9];
    MakeScratchpad(0x0190, scratchpad);
    FakeOneWire::SetPresent(line, true);
    FakeOneWire::QueueRead(line, scratchpad, 9);

    RunSampleCycle(channel, node);

    int16_t reported = 0;
    CC_CHECK_EQ(ReportsOn(node, Endpoint::SupplyTemp, reported), 1);
    CC_CHECK_EQ(reported, 2500);
}

CC_TEST(DuctChannel, AbsentSensorStaysNotPresent)
{
    ResetWorld();
    Node node;
    node.Init();

    DuctChannel channel(node, Endpoint::ReturnTemp, line);
    channel.Init();

    FakeOneWire::SetPresent(line, false);

    channel.Loop(); // start conversion fails
    FakeClock::Advance(1100);
    channel.Loop();

    CC_CHECK(!channel.Present());

    int16_t reported = 0;
    CC_CHECK_EQ(ReportsOn(node, Endpoint::ReturnTemp, reported), 0);
}

CC_TEST(DuctChannel, AnUnchangedValueIsResentAfterTheMasterWasLost)
{
    ResetWorld();
    Node node;
    node.Init();

    DuctChannel channel(node, Endpoint::SupplyTemp, line);
    channel.Init();

    FakeOneWire::SetPresent(line, true);

    uint8_t scratchpad[9];
    MakeScratchpad(0x0190, scratchpad);
    FakeOneWire::QueueRead(line, scratchpad, 9);
    RunSampleCycle(channel, node);

    int16_t reported = 0;
    CC_CHECK_EQ(ReportsOn(node, Endpoint::SupplyTemp, reported), 1);

    // Same reading again -> no Report (below the 0.1 degC step).
    FakeOneWire::QueueRead(line, scratchpad, 9);
    RunSampleCycle(channel, node);
    CC_CHECK_EQ(ReportsOn(node, Endpoint::SupplyTemp, reported), 0);

    // The master goes quiet past the heartbeat, then comes back: the
    // unchanged reading is sent again.
    FakeClock::Advance(1500);
    node.Loop();
    CC_CHECK_EQ(ReportsOn(node, Endpoint::SupplyTemp, reported), 1);
    CC_CHECK_EQ(reported, 2500);
}

CC_TEST(DuctChannel, AMissingProbeStopsBeingReportedEvenAsAKeepalive)
{
    ResetWorld();
    Node node;
    node.Init();

    DuctChannel channel(node, Endpoint::SupplyTemp, line);
    channel.Init();

    FakeOneWire::SetPresent(line, true);
    uint8_t scratchpad[9];
    MakeScratchpad(0x0190, scratchpad);
    FakeOneWire::QueueRead(line, scratchpad, 9);
    RunSampleCycle(channel, node);
    int16_t reported = 0;
    CC_CHECK_EQ(ReportsOn(node, Endpoint::SupplyTemp, reported), 1);

    FakeOneWire::SetPresent(line, false); // probe unplugged
    for (int i = 0; i < 40; i++) // ~70 s -- past a whole keepalive round
    {
        RunSampleCycle(channel, node);
    }
    CC_CHECK(!channel.Present());
    CC_CHECK_EQ(ReportsOn(node, Endpoint::SupplyTemp, reported), 0);
}

CC_TEST(DuctChannel, TheOffsetIsAddedToEveryReading)
{
    ResetWorld();
    Node node;
    node.Init();

    DuctChannel channel(node, Endpoint::SupplyTemp, line);
    channel.Init();
    channel.SetOffset(-130);

    uint8_t scratchpad[9];
    MakeScratchpad(0x0190, scratchpad); // raw +25.00 degC
    FakeOneWire::SetPresent(line, true);
    FakeOneWire::QueueRead(line, scratchpad, 9);
    RunSampleCycle(channel, node);

    CC_CHECK_EQ(channel.Value(), 2370);
    int16_t reported = 0;
    CC_CHECK_EQ(ReportsOn(node, Endpoint::SupplyTemp, reported), 1);
    CC_CHECK_EQ(reported, 2370);
}

CC_TEST(DuctChannel, ANewOffsetIsPublishedWithoutWaitingForTheNextSample)
{
    ResetWorld();
    Node node;
    node.Init();

    DuctChannel channel(node, Endpoint::SupplyTemp, line);
    channel.Init();

    uint8_t scratchpad[9];
    MakeScratchpad(0x0190, scratchpad);
    FakeOneWire::SetPresent(line, true);
    FakeOneWire::QueueRead(line, scratchpad, 9);
    RunSampleCycle(channel, node);
    int16_t reported = 0;
    CC_CHECK_EQ(ReportsOn(node, Endpoint::SupplyTemp, reported), 1);

    channel.SetOffset(-90);
    CC_CHECK_EQ(ReportsOn(node, Endpoint::SupplyTemp, reported), 1);
    CC_CHECK_EQ(reported, 2410);
}

namespace
{
    // The handler's reply to the last request: a Report (value out) or a Nack.
    bool LastReply(Node& node, const Endpoint endpoint, Operation& op, int16_t& value)
    {
        Poll(node);
        Poll(node);
        Message   tx[32];
        const int n     = bus::DecodeTx(tx, 32);
        bool      found = false;
        for (int i = 0; i < n; i++)
        {
            if (tx[i].id.endpoint == endpoint)
            {
                found = true;
                op    = tx[i].id.operation;
                value = tx[i].len >= 2 ? static_cast<int16_t>(tx[i].data[0] | (tx[i].data[1] << 8)) : 0;
            }
        }
        FakeBus::TruncateTx(0);
        return found;
    }

    Message OffsetSet(const Endpoint endpoint, const int16_t centiDegC)
    {
        Message m(NodeLib::Id(nodeId, endpoint, Operation::Set));
        m.data[0] = static_cast<uint8_t>(centiDegC & 0xFF);
        m.data[1] = static_cast<uint8_t>((centiDegC >> 8) & 0xFF);
        m.len     = 2;
        return m;
    }
} // namespace

CC_TEST(TemperatureHandler, AnOffsetSetIsStoredAndEchoed)
{
    ResetWorld();
    Node node;
    node.Init();
    TemperatureHandler handler(node);
    handler.Init();

    handler.ReceivedMessage(OffsetSet(Endpoint::ReturnTempOffset, -130));
    Operation op    = Operation::Get;
    int16_t   value = 0;
    CC_CHECK(LastReply(node, Endpoint::ReturnTempOffset, op, value));
    CC_CHECK(op == Operation::Report);
    CC_CHECK_EQ(value, -130);

    TemperatureHandler afterReset(node); // a fresh handler loads it back from flash
    afterReset.Init();
    afterReset.ReceivedMessage(Message(NodeLib::Id(nodeId, Endpoint::ReturnTempOffset, Operation::Get)));
    CC_CHECK(LastReply(node, Endpoint::ReturnTempOffset, op, value));
    CC_CHECK_EQ(value, -130);
}

CC_TEST(TemperatureHandler, AnOutOfRangeOffsetIsNacked)
{
    ResetWorld();
    Node node;
    node.Init();
    TemperatureHandler handler(node);
    handler.Init();

    handler.ReceivedMessage(OffsetSet(Endpoint::SupplyTempOffset, 1500)); // +15 degC
    Operation op    = Operation::Get;
    int16_t   value = 0;
    CC_CHECK(LastReply(node, Endpoint::SupplyTempOffset, op, value));
    CC_CHECK(op == Operation::Nack);
}
