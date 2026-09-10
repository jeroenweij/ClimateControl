/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "BoardPins.h"
#include "Node.h"

#include "Ds18b20.h"
#include "DuctChannel.h"

#include "BusHelpers.h"
#include "FakeBus.h"
#include "FakeClock.h"
#include "FakeConfigStore.h"
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
        FakeOneWire::ResetAll();
    }

    // Run one full sample cycle: Idle -> start conversion -> wait it out -> read.
    void RunSampleCycle(DuctChannel& channel)
    {
        channel.Loop(); // Idle -> Converting
        channel.Loop(); // still converting
        FakeClock::Advance(Ds18b20::ConversionTimeMs + 100);
        channel.Loop(); // Converting -> read + publish
        FakeClock::Advance(1100); // let the sample timer expire for the next cycle
    }

    // Poll the node so its queue flushes onto the bus, then count Reports on
    // 'endpoint' and return the last one's decoded int16 value.
    int ReportsOn(Node& node, const Endpoint endpoint, int16_t& lastValue)
    {
        FakeBus::Reset();
        bus::InjectFrame(Message(nodeId, Operation::Poll));
        node.Loop();

        Message   tx[8];
        const int n     = bus::DecodeTx(tx, 8);
        int       count = 0;
        for (int i = 0; i < n; i++)
        {
            if (tx[i].id.endpoint == endpoint && tx[i].id.operation == Operation::Report)
            {
                count++;
                lastValue = static_cast<int16_t>(tx[i].data[0] | (tx[i].data[1] << 8));
            }
        }
        return count;
    }
} // namespace

CC_TEST(DuctChannel, BecomesPresentWithTheDecodedValue)
{
    ResetWorld();
    Node node(115200);
    node.Init();

    DuctChannel channel(node, Endpoint::SupplyTemp, line);
    channel.Init();

    uint8_t scratchpad[9];
    MakeScratchpad(0x0190, scratchpad); // +25.0 degC
    FakeOneWire::SetPresent(line, true);
    FakeOneWire::QueueRead(line, scratchpad, 9);

    RunSampleCycle(channel);

    CC_CHECK(channel.Present());
    CC_CHECK_EQ(channel.Value(), 2500);
}

CC_TEST(DuctChannel, EmitsAReportAfterTheFirstSample)
{
    ResetWorld();
    Node node(115200);
    node.Init();

    DuctChannel channel(node, Endpoint::SupplyTemp, line);
    channel.Init();

    uint8_t scratchpad[9];
    MakeScratchpad(0x0190, scratchpad);
    FakeOneWire::SetPresent(line, true);
    FakeOneWire::QueueRead(line, scratchpad, 9);

    RunSampleCycle(channel);

    int16_t reported = 0;
    CC_CHECK_EQ(ReportsOn(node, Endpoint::SupplyTemp, reported), 1);
    CC_CHECK_EQ(reported, 2500);
}

CC_TEST(DuctChannel, AbsentSensorStaysNotPresent)
{
    ResetWorld();
    Node node(115200);
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

CC_TEST(DuctChannel, InvalidateForcesAFreshReportOfAnUnchangedValue)
{
    ResetWorld();
    Node node(115200);
    node.Init();

    DuctChannel channel(node, Endpoint::SupplyTemp, line);
    channel.Init();

    FakeOneWire::SetPresent(line, true);

    uint8_t scratchpad[9];
    MakeScratchpad(0x0190, scratchpad);
    FakeOneWire::QueueRead(line, scratchpad, 9);
    RunSampleCycle(channel);

    int16_t reported = 0;
    CC_CHECK_EQ(ReportsOn(node, Endpoint::SupplyTemp, reported), 1);

    // Same reading again -> no Report (below threshold, refresh not due).
    FakeOneWire::QueueRead(line, scratchpad, 9);
    RunSampleCycle(channel);
    CC_CHECK_EQ(ReportsOn(node, Endpoint::SupplyTemp, reported), 0);

    // After a bus reconnect the channel re-sends the current value.
    channel.Invalidate();
    FakeOneWire::QueueRead(line, scratchpad, 9);
    RunSampleCycle(channel);
    CC_CHECK_EQ(ReportsOn(node, Endpoint::SupplyTemp, reported), 1);
    CC_CHECK_EQ(reported, 2500);
}
