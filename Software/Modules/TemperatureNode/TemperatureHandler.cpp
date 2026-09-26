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
    constexpr uint8_t NackReadOnly = 0x01;
} // namespace

TemperatureHandler::TemperatureHandler(NodeLib::Node& node) :
    node(node),
    returnChannel(node, Endpoint::ReturnTemp, Board::OneWire1),
    supplyChannel(node, Endpoint::SupplyTemp, Board::OneWire2)
{
    node.AddPublished(Endpoint::SensorStatus, 1);
}

void TemperatureHandler::Init()
{
    returnChannel.Init();
    supplyChannel.Init();
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
