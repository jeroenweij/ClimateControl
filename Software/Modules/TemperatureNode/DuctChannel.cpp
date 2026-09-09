/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "Logger.h"

#include "DuctChannel.h"

using NodeLib::Endpoint;
using NodeLib::Id;
using NodeLib::Operation;

namespace
{
    // The duct air changes far slower than this; one reading per second is
    // plenty and leaves the 1-Wire line idle most of the time.
    constexpr Tools::time_a SampleIntervalMs = 1000;

    // Small margin on top of the datasheet worst-case conversion time.
    constexpr Tools::time_a ConversionGuardMs = 20;

    // Force a Report at least this often even when the reading has not moved, so
    // MainController's view of the value does not go stale (TemperatureNode-Spec.md
    // §5 item 3 -- final rate still open).
    constexpr Tools::time_a MinReportIntervalMs = 30000;

    // Report early when the reading moves by at least this much (centi-degC).
    constexpr int16_t ReportThresholdCentiDeg = 10; // 0.1 degC
} // namespace

DuctChannel::DuctChannel(NodeLib::Node& node, const Endpoint endpoint, const Hal::Pin oneWirePin) :
    node(node),
    endpoint(endpoint),
    sensor(oneWirePin),
    state(State::Idle),
    value(0),
    lastReported(0),
    everReported(false),
    present(false),
    sampleTimer(),
    conversionTimer(),
    minReportTimer()
{
}

void DuctChannel::Init()
{
    // The 1-Wire line is configured (open-drain, released) by the Hal::OneWire
    // inside 'sensor'. Nothing else to bring up here.
    state   = State::Idle;
    present = false;
}

void DuctChannel::Loop()
{
    switch (state)
    {
        case State::Idle:
        {
            if (sampleTimer.IsRunning() && !sampleTimer.Finished())
            {
                return;
            }

            if (sensor.StartConversion())
            {
                state = State::Converting;
                conversionTimer.Start(Ds18b20::ConversionTimeMs + ConversionGuardMs);
            }
            else
            {
                SetPresent(false);
                sampleTimer.Start(SampleIntervalMs);
            }
            break;
        }

        case State::Converting:
        {
            if (conversionTimer.IsRunning() && !conversionTimer.Finished())
            {
                return;
            }

            int16_t sample = 0;
            if (sensor.ReadTemperature(sample))
            {
                SetPresent(true);
                value = sample;
                PublishIfDue();
            }
            else
            {
                SetPresent(false);
            }

            state = State::Idle;
            sampleTimer.Start(SampleIntervalMs);
            break;
        }
    }
}

void DuctChannel::PublishIfDue()
{
    const int16_t delta      = static_cast<int16_t>(value - lastReported);
    const bool    moved      = delta >= ReportThresholdCentiDeg || delta <= -ReportThresholdCentiDeg;
    const bool    refreshDue = !minReportTimer.IsRunning() || minReportTimer.Finished();

    if (!everReported || moved || refreshDue)
    {
        Report();
    }
}

void DuctChannel::Report()
{
    // int16 centi-degC, little-endian (Node-Message-Model-Spec.md §5).
    const uint8_t payload[2] = {
        static_cast<uint8_t>(value & 0xFF),
        static_cast<uint8_t>((value >> 8) & 0xFF),
    };
    node.QueueMessage(Id(node.GetId(), endpoint, Operation::Report), payload, sizeof(payload));

    lastReported = value;
    everReported = true;
    minReportTimer.Start(MinReportIntervalMs);
}

void DuctChannel::Invalidate()
{
    everReported = false;
}

void DuctChannel::SetPresent(const bool nowPresent)
{
    if (nowPresent != present)
    {
        LOG_INFO("Duct probe " << endpoint << (nowPresent ? " present" : " lost"));
        present = nowPresent;
    }
}

bool DuctChannel::Present() const
{
    return present;
}

int16_t DuctChannel::Value() const
{
    return value;
}
