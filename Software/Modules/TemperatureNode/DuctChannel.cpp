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
} // namespace

DuctChannel::DuctChannel(NodeLib::Node& node, const Endpoint endpoint, const Hal::Pin oneWirePin) :
    node(node),
    endpoint(endpoint),
    sensor(oneWirePin),
    state(State::Idle),
    value(0),
    present(false),
    sampleTimer(),
    conversionTimer()
{
    // On change by 0.1 degC + keepalive, re-sent after a lost master
    // (Node's publisher, Node-Message-Model-Spec.md §6.1).
    node.AddPublished(endpoint, 2, NodeLib::TempMinChange);
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
                node.PublishIfChanged(endpoint, value);
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

void DuctChannel::Report()
{
    // int16 centi-degC, little-endian (Node-Message-Model-Spec.md §5).
    const uint8_t payload[2] = {
        static_cast<uint8_t>(value & 0xFF),
        static_cast<uint8_t>((value >> 8) & 0xFF),
    };
    node.QueueMessage(Id(node.GetId(), endpoint, Operation::Report), payload, sizeof(payload));
}

void DuctChannel::SetPresent(const bool nowPresent)
{
    if (nowPresent != present)
    {
        LOG_INFO("Duct probe " << endpoint << (nowPresent ? " present" : " lost"));
        present = nowPresent;
        if (!present)
        {
            node.ClearPublished(endpoint); // no reading -- don't keep reporting the last one
        }
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
