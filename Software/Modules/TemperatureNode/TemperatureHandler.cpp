/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "BoardPins.h"
#include "Logger.h"

#include "TemperatureHandler.h"

using NodeLib::Endpoint;
using NodeLib::Id;
using NodeLib::Message;
using NodeLib::Operation;
using NodeLib::SystemStatus;

namespace
{
    // Nack reason byte for a write to a read-only endpoint (Node-Message-Model-
    // Spec.md §4 -- the reason-code catalog itself is still informal).
    constexpr uint8_t NackReadOnly    = 0x01;
    constexpr uint8_t NackBadRequest  = 0x02;
    constexpr uint8_t NackWriteFailed = 0x04;
} // namespace

TemperatureHandler::TemperatureHandler(NodeLib::Node& node) :
    node(node),
    calibration(),
    returnChannel(node, Endpoint::ReturnTemp, Board::OneWire1),
    supplyChannel(node, Endpoint::SupplyTemp, Board::OneWire2)
{
    node.AddPublished(Endpoint::SensorStatus, 1);
}

void TemperatureHandler::Init()
{
    calibration.Load();
    returnChannel.Init();
    supplyChannel.Init();
    returnChannel.SetOffset(calibration.Offset(Calibration::Probe::Return));
    supplyChannel.SetOffset(calibration.Offset(Calibration::Probe::Supply));
}

void TemperatureHandler::Loop()
{
    returnChannel.Loop();
    supplyChannel.Loop();

    node.PublishIfChanged(Endpoint::SensorStatus, SensorStatus());
}

void TemperatureHandler::ReceivedMessage(const Message& m)
{
    switch (m.id.endpoint)
    {
        case Endpoint::SupplyTemp:
        case Endpoint::ReturnTemp:
        case Endpoint::SensorStatus:
            break;
        case Endpoint::SupplyTempOffset:
            HandleOffset(m, Calibration::Probe::Supply, supplyChannel);
            return;
        case Endpoint::ReturnTempOffset:
            HandleOffset(m, Calibration::Probe::Return, returnChannel);
            return;
        default:
            // System / Firmware / Diagnostics blocks are NodeLib-owned.
            return;
    }

    if (m.id.operation != Operation::Get)
    {
        LOG_WARN("Rejecting write to read-only endpoint " << m.id.endpoint);
        node.QueueMessage(Id(node.GetId(), m.id.endpoint, Operation::Nack), NackReadOnly);
        return;
    }

    switch (m.id.endpoint)
    {
        case Endpoint::ReturnTemp:
            returnChannel.Report();
            break;
        case Endpoint::SupplyTemp:
            supplyChannel.Report();
            break;
        case Endpoint::SensorStatus:
            node.QueueMessage(Id(node.GetId(), Endpoint::SensorStatus, Operation::Report), SensorStatus());
            break;
        default:
            break;
    }
}

void TemperatureHandler::ConnectionLost()
{
    // Nothing to drive to a safe state -- TemperatureNode only measures
    // (TemperatureNode-Spec.md §2). Node's publisher re-sends the current
    // readings once the master resumes polling.
    LOG_WARN("Bus connection lost");
}

void TemperatureHandler::FillStatus(SystemStatus& status)
{
    if (!returnChannel.Present())
    {
        status.errorFlags |= ReturnSensorFault;
    }
    if (!supplyChannel.Present())
    {
        status.errorFlags |= SupplySensorFault;
    }
}

void TemperatureHandler::HandleOffset(const Message& m, const Calibration::Probe probe, DuctChannel& channel)
{
    if (m.id.operation == Operation::Get)
    {
        ReportOffset(m.id.endpoint, calibration.Offset(probe));
        return;
    }
    if (m.id.operation != Operation::Set || m.len < 2)
    {
        node.QueueMessage(Id(node.GetId(), m.id.endpoint, Operation::Nack), NackBadRequest);
        return;
    }

    const int16_t requested = static_cast<int16_t>(m.data[0] | (m.data[1] << 8));
    if (requested > Calibration::MaxOffset || requested < -Calibration::MaxOffset)
    {
        LOG_WARN("Rejecting out-of-range " << m.id.endpoint << " " << requested);
        node.QueueMessage(Id(node.GetId(), m.id.endpoint, Operation::Nack), NackBadRequest);
        return;
    }
    if (!calibration.SetOffset(probe, requested))
    {
        node.QueueMessage(Id(node.GetId(), m.id.endpoint, Operation::Nack), NackWriteFailed);
        return;
    }

    LOG_INFO(m.id.endpoint << " = " << requested << " centi-degC");
    channel.SetOffset(requested);
    ReportOffset(m.id.endpoint, requested);
}

void TemperatureHandler::ReportOffset(const Endpoint endpoint, const int16_t centiDegC)
{
    // int16 centi-degC, little-endian (Node-Message-Model-Spec.md §5).
    const uint8_t payload[2] = {
        static_cast<uint8_t>(centiDegC & 0xFF),
        static_cast<uint8_t>((centiDegC >> 8) & 0xFF),
    };
    node.QueueMessage(Id(node.GetId(), endpoint, Operation::Report), payload, sizeof(payload));
}

uint8_t TemperatureHandler::SensorStatus() const
{
    uint8_t status = 0;
    if (returnChannel.Present())
    {
        status |= ReturnSensorValid;
    }
    if (supplyChannel.Present())
    {
        status |= SupplySensorValid;
    }
    return status;
}
