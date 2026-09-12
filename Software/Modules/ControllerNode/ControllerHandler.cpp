/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "Logger.h"

#include "EEndpoint.h"
#include "EFirmware.h"
#include "EOperation.h"

#include "ControllerHandler.h"

using NodeLib::Endpoint;
using NodeLib::FirmwareOp;
using NodeLib::Id;
using NodeLib::Message;
using NodeLib::Operation;
using NodeLib::SystemStatus;

namespace
{
    constexpr uint8_t NackReadOnly    = 0x01;
    constexpr uint8_t NackBadRequest  = 0x02;
    constexpr uint8_t NackUnsupported = 0x03;

    constexpr uint8_t ForceFlag = 1u << 0;

    void PackU16(uint8_t* const p, const uint16_t v)
    {
        p[0] = static_cast<uint8_t>(v);
        p[1] = static_cast<uint8_t>(v >> 8);
    }

    uint16_t ReadU16(const uint8_t* const p)
    {
        return static_cast<uint16_t>(p[0] | (p[1] << 8));
    }

    uint32_t ReadU32(const uint8_t* const p)
    {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
            (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    }
} // namespace

ControllerHandler::ControllerHandler(NodeLib::Node& node, Damper& damper, ThermostatLink& link) :
    node(node),
    damper(damper),
    thermostatLink(link),
    reportedActual(0),
    reportedActualValid(false),
    reportedMode(0),
    reportedModeValid(false)
{
}

void ControllerHandler::Loop()
{
    damper.Loop();

    const uint8_t actual = damper.Actual();
    if (!reportedActualValid || actual != reportedActual)
    {
        reportedActual      = actual;
        reportedActualValid = true;
        Report(Endpoint::DamperActual, &actual, 1);
    }

    const uint8_t mode = damper.ReportedMode();
    if (!reportedModeValid || mode != reportedMode)
    {
        reportedMode      = mode;
        reportedModeValid = true;
        Report(Endpoint::DamperMode, &mode, 1);
    }
}

void ControllerHandler::ReceivedMessage(const Message& m)
{
    switch (static_cast<uint8_t>(m.id.endpoint) & 0xF0)
    {
        case static_cast<uint8_t>(Endpoint::DamperTarget) & 0xF0:
            HandleDamper(m);
            break;
        case static_cast<uint8_t>(Endpoint::RoomSetpoint) & 0xF0:
            HandleRoom(m);
            break;
        default:
            if (m.id.endpoint == Endpoint::ThermostatFirmware)
            {
                HandleThermostatFirmware(m);
            }
            break;
    }
}

void ControllerHandler::HandleDamper(const Message& m)
{
    switch (m.id.endpoint)
    {
        case Endpoint::DamperTarget:
            if (m.id.operation == Operation::Get)
            {
                const uint8_t v = damper.Target();
                Report(Endpoint::DamperTarget, &v, 1);
            }
            else if (m.id.operation == Operation::Set && m.len >= 1)
            {
                damper.SetTarget(m.data[0]);
            }
            else
            {
                Nack(m, NackBadRequest);
            }
            break;

        case Endpoint::DamperActual:
            if (m.id.operation == Operation::Get)
            {
                const uint8_t v = damper.Actual();
                Report(Endpoint::DamperActual, &v, 1);
            }
            else
            {
                Nack(m, NackReadOnly);
            }
            break;

        case Endpoint::DamperMode:
            if (m.id.operation == Operation::Get)
            {
                const uint8_t v = damper.ReportedMode();
                Report(Endpoint::DamperMode, &v, 1);
            }
            else if (m.id.operation == Operation::Set && m.len >= 1 && m.data[0] <= 3)
            {
                damper.SetMode(static_cast<Damper::Mode>(m.data[0]));
            }
            else
            {
                Nack(m, NackBadRequest);
            }
            break;

        default:
            break;
    }
}

void ControllerHandler::HandleRoom(const Message& m)
{
    const ThermostatLink::RoomState& room = thermostatLink.Room();

    if (m.id.operation == Operation::Set)
    {
        if (m.id.endpoint == Endpoint::RoomSetpoint && m.len >= 2)
        {
            thermostatLink.PushSetpoint(static_cast<int16_t>(ReadU16(m.data)));
            return;
        }
        Nack(m, NackReadOnly);
        return;
    }
    if (m.id.operation != Operation::Get)
    {
        Nack(m, NackReadOnly);
        return;
    }

    uint8_t payload[2];
    switch (m.id.endpoint)
    {
        case Endpoint::RoomSetpoint:
            PackU16(payload, static_cast<uint16_t>(room.setpoint));
            Report(Endpoint::RoomSetpoint, payload, 2);
            break;
        case Endpoint::RoomTemp:
            PackU16(payload, static_cast<uint16_t>(room.temp));
            Report(Endpoint::RoomTemp, payload, 2);
            break;
        case Endpoint::RoomHumidity:
            PackU16(payload, room.humidity);
            Report(Endpoint::RoomHumidity, payload, 2);
            break;
        case Endpoint::RoomMode:
            payload[0] = room.mode;
            Report(Endpoint::RoomMode, payload, 1);
            break;
        case Endpoint::RoomLink:
            payload[0] = thermostatLink.LinkUp() ? 1 : 0;
            Report(Endpoint::RoomLink, payload, 1);
            break;
        default:
            break;
    }
}

void ControllerHandler::HandleThermostatFirmware(const Message& m)
{
    if (m.id.operation == Operation::Get)
    {
        ReportThermostatFirmwareStatus();
        return;
    }
    if (m.id.operation != Operation::Set || m.len < 1)
    {
        Nack(m, NackBadRequest);
        return;
    }

    switch (static_cast<FirmwareOp>(m.data[0]))
    {
        case FirmwareOp::Begin:
            // op module(1) imageSize(4) imageCrc32(4) fwVersion(2) flags(1)
            if (m.len >= 13)
            {
                const bool force = (m.data[12] & ForceFlag) != 0;
                thermostatLink.OtaBegin(m.data[1], ReadU32(&m.data[2]), ReadU32(&m.data[6]), ReadU16(&m.data[10]), force);
            }
            break;
        case FirmwareOp::Write:
            // op byteOffset(4) bytes
            if (m.len >= 5)
            {
                thermostatLink.OtaWrite(ReadU32(&m.data[1]), &m.data[5], static_cast<uint8_t>(m.len - 5));
            }
            break;
        case FirmwareOp::End:
            thermostatLink.OtaEnd();
            break;
        case FirmwareOp::Activate:
            thermostatLink.OtaActivate();
            break;
        case FirmwareOp::Abort:
            thermostatLink.OtaAbort();
            break;
        default:
            Nack(m, NackUnsupported);
            return;
    }
    ReportThermostatFirmwareStatus();
}

void ControllerHandler::ReportThermostatFirmwareStatus()
{
    uint8_t status[9];
    thermostatLink.FillOtaStatus(status);
    Report(Endpoint::ThermostatFirmware, status, sizeof(status));
}

void ControllerHandler::ConnectionLost()
{
    LOG_WARN("Main bus connection lost");
    reportedActualValid = false;
    reportedModeValid   = false;
}

void ControllerHandler::PrepareForReset()
{
    damper.ParkNeutral();
}

void ControllerHandler::FillStatus(SystemStatus& status)
{
    if (!thermostatLink.LinkUp())
    {
        status.errorFlags |= ThermostatLinkDown;
    }
}

void ControllerHandler::Report(const Endpoint endpoint, const uint8_t* const data, const uint8_t len)
{
    node.QueueMessage(Id(node.GetId(), endpoint, Operation::Report), data, len);
}

void ControllerHandler::Nack(const Message& m, const uint8_t reason)
{
    node.QueueMessage(Id(node.GetId(), m.id.endpoint, Operation::Nack), reason);
}
