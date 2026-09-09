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
    // DS18B20 12-bit conversion is ~750 ms; one sample per second keeps the
    // probe well within spec and the duct air changes far slower than that.
    constexpr Tools::time_a SampleIntervalMs = 1000;

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
    oneWirePin(oneWirePin),
    value(0),
    lastReported(0),
    everReported(false),
    present(false),
    sampleTimer(),
    minReportTimer()
{
}

void DuctChannel::Init()
{
    // TODO: 1-Wire bit-bang bring-up on 'oneWirePin' -- open-drain (drive low /
    // release, the on-board 4.7k pulls high), timing-critical slots with
    // interrupts briefly masked (TemperatureNode-Spec.md §4.1). Hal::Gpio has no
    // open-drain mode yet; that HAL addition + the DS18B20 driver are the
    // remaining work here.
    present = false;
}

void DuctChannel::Loop()
{
    if (sampleTimer.IsRunning() && !sampleTimer.Finished())
    {
        return;
    }
    sampleTimer.Start(SampleIntervalMs);

    int16_t    sample = 0;
    const bool ok     = ReadProbe(sample);
    if (ok != present)
    {
        LOG_INFO("Duct probe " << endpoint << (ok ? " present" : " lost"));
        present = ok;
    }
    if (!ok)
    {
        return;
    }

    value = sample;

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

bool DuctChannel::Present() const
{
    return present;
}

int16_t DuctChannel::Value() const
{
    return value;
}

bool DuctChannel::ReadProbe(int16_t& /*centiDegC*/)
{
    // TODO: DS18B20 transaction -- reset pulse + presence detect, Skip-ROM
    // (0xCC) CONVERT T (0x44), then Skip-ROM READ SCRATCHPAD (0xBE); check the
    // scratchpad CRC-8, convert the native int16 (1/16 degC) to centi-degC.
    // One device per line, so no ROM search (TemperatureNode-Spec.md §4.1).
    // Until the 1-Wire driver lands, report the probe as absent rather than
    // inventing a reading.
    return false;
}
