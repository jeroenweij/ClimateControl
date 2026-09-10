/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "Logger.h"

#include "EEndpoint.h"
#include "EOperation.h"

#include "ThermostatHandler.h"

using NodeLib::Endpoint;
using NodeLib::Id;
using NodeLib::Message;
using NodeLib::Operation;

namespace
{
    constexpr uint8_t NackReadOnly   = 0x01;
    constexpr uint8_t NackBadRequest = 0x02;

    // Deadbands for on-change publishing (Node-Message-Model-Spec.md §6.1).
    constexpr int16_t  TempDeadband     = 10; // 0.1 degC
    constexpr uint16_t HumidityDeadband = 100; // 1 %RH

    constexpr uint32_t sampleIntervalMs    = 2000;
    constexpr uint32_t keepaliveIntervalMs = 60000;

    void PackU16(uint8_t* const p, const uint16_t v)
    {
        p[0] = static_cast<uint8_t>(v);
        p[1] = static_cast<uint8_t>(v >> 8);
    }

    uint16_t ReadU16(const uint8_t* const p)
    {
        return static_cast<uint16_t>(p[0] | (p[1] << 8));
    }

    int16_t Abs16(const int16_t v)
    {
        return v < 0 ? static_cast<int16_t>(-v) : v;
    }
} // namespace

ThermostatHandler::ThermostatHandler(NodeLib::Node& node) :
    node(node),
    setpoint(2100), // 21.00 degC
    roomTemp(2100),
    humidity(4500),
    roomMode(2), // Auto
    reportedSetpoint(0),
    reportedTemp(0),
    reportedHumidity(0),
    reportedMode(0),
    everReported(false),
    damperActual(0),
    damperMode(0),
    sampleTimer(),
    keepaliveTimer()
{
}

void ThermostatHandler::Init()
{
    // TODO: bring up I2C1, the SSD1306 and the CHT40; configure the two buttons.
    sampleTimer.Start(sampleIntervalMs);
    keepaliveTimer.Start(keepaliveIntervalMs);
}

void ThermostatHandler::Loop()
{
    ServiceButtons();

    if (sampleTimer.Finished())
    {
        SampleRoom();
        sampleTimer.Start(sampleIntervalMs);
    }

    const bool keepalive = keepaliveTimer.Finished();
    if (keepalive)
    {
        keepaliveTimer.Start(keepaliveIntervalMs);
    }
    PublishRoom(keepalive);

    RenderDisplay();
}

void ThermostatHandler::SampleRoom()
{
    // TODO: read CHT40 temperature + humidity over I2C1. Placeholder: hold the
    // last value so PublishRoom stays quiet until a real reading moves it.
}

void ThermostatHandler::ServiceButtons()
{
    // TODO: Board::UserButton = ack / clear link-lost; Board::Button2 = setpoint
    // adjust. For now the setpoint only changes via a master override.
}

void ThermostatHandler::RenderDisplay()
{
    // TODO: draw setpoint / roomTemp / humidity / roomMode and the cached
    // damperActual / damperMode to the SSD1306. Sleep the panel on inactivity.
}

void ThermostatHandler::PublishRoom(const bool force)
{
    if (force || !everReported || setpoint != reportedSetpoint)
    {
        uint8_t p[2];
        PackU16(p, static_cast<uint16_t>(setpoint));
        node.QueueMessage(Id(node.GetId(), Endpoint::RoomSetpoint, Operation::Report), p, 2);
        reportedSetpoint = setpoint;
    }
    if (force || !everReported || Abs16(roomTemp - reportedTemp) >= TempDeadband)
    {
        uint8_t p[2];
        PackU16(p, static_cast<uint16_t>(roomTemp));
        node.QueueMessage(Id(node.GetId(), Endpoint::RoomTemp, Operation::Report), p, 2);
        reportedTemp = roomTemp;
    }
    if (force || !everReported ||
        (humidity > reportedHumidity ? humidity - reportedHumidity : reportedHumidity - humidity) >=
            HumidityDeadband)
    {
        uint8_t p[2];
        PackU16(p, humidity);
        node.QueueMessage(Id(node.GetId(), Endpoint::RoomHumidity, Operation::Report), p, 2);
        reportedHumidity = humidity;
    }
    if (force || !everReported || roomMode != reportedMode)
    {
        node.QueueMessage(Id(node.GetId(), Endpoint::RoomMode, Operation::Report), roomMode);
        reportedMode = roomMode;
    }
    everReported = true;
}

void ThermostatHandler::ReceivedMessage(const Message& m)
{
    switch (m.id.endpoint)
    {
        case Endpoint::RoomSetpoint:
            if (m.id.operation == Operation::Get)
            {
                uint8_t p[2];
                PackU16(p, static_cast<uint16_t>(setpoint));
                node.QueueMessage(Id(node.GetId(), Endpoint::RoomSetpoint, Operation::Report), p, 2);
            }
            else if (m.id.operation == Operation::Set && m.len >= 2)
            {
                setpoint = static_cast<int16_t>(ReadU16(m.data)); // master override
                LOG_INFO("Setpoint override " << setpoint);
            }
            else
            {
                node.QueueMessage(Id(node.GetId(), m.id.endpoint, Operation::Nack), NackBadRequest);
            }
            break;

        case Endpoint::RoomTemp:
        case Endpoint::RoomHumidity:
        case Endpoint::RoomMode:
            if (m.id.operation == Operation::Get)
            {
                PublishRoom(true);
            }
            else
            {
                node.QueueMessage(Id(node.GetId(), m.id.endpoint, Operation::Nack), NackReadOnly);
            }
            break;

        case Endpoint::DamperActual:
            if (m.id.operation == Operation::Set && m.len >= 1)
            {
                damperActual = m.data[0];
            }
            break;
        case Endpoint::DamperMode:
            if (m.id.operation == Operation::Set && m.len >= 1)
            {
                damperMode = m.data[0];
            }
            break;

        default:
            break;
    }
}

void ThermostatHandler::ConnectionLost()
{
    // Hold the setpoint locally (we are its source of truth) and flag the UI.
    LOG_WARN("ControllerNode link lost");
    // TODO: show a "no link" indicator on the display.
}

void ThermostatHandler::PrepareForReset()
{
    // TODO: blank / sleep the OLED before the OTA reset.
}
