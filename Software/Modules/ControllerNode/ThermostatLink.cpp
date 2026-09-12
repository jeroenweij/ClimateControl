/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include <string.h>

#include "Logger.h"

#include "EEndpoint.h"
#include "EOperation.h"

#include "ThermostatLink.h"

using NodeLib::Endpoint;
using NodeLib::FirmwareError;
using NodeLib::FirmwareOp;
using NodeLib::Message;
using NodeLib::Operation;

namespace
{
    uint16_t ReadU16(const uint8_t* const p)
    {
        return static_cast<uint16_t>(p[0] | (p[1] << 8));
    }

    uint32_t ReadU32(const uint8_t* const p)
    {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
            (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    }

    void WriteU32(uint8_t* const p, const uint32_t v)
    {
        p[0] = static_cast<uint8_t>(v);
        p[1] = static_cast<uint8_t>(v >> 8);
        p[2] = static_cast<uint8_t>(v >> 16);
        p[3] = static_cast<uint8_t>(v >> 24);
    }

    // Peer Status state byte: 0 = app; the bootloader never reports 0.
    constexpr uint8_t StateApp       = 0;
    constexpr uint8_t StateReceiving = 3;
} // namespace

ThermostatLink::ThermostatLink(NodeLib::LinkMaster& link, Damper& damper) :
    link(link),
    damper(damper),
    room{0, 0, 0, 0, false},
    thermostatFw(0),
    otaState(OtaState::Idle),
    lastError(FirmwareError::None),
    beginPayload{},
    expectedOffset(0),
    peerState(StateApp),
    enterBlTimer(),
    displayTimer(),
    lastPushedActual(0xFF),
    lastPushedMode(0xFF)
{
}

void ThermostatLink::Loop()
{
    if (otaState == OtaState::EnteringBootloader)
    {
        if (link.PeerInBootloader())
        {
            SendFirmwareOp(FirmwareOp::Begin, &beginPayload[1], sizeof(beginPayload) - 1);
            otaState = OtaState::Transferring;
        }
        else if (enterBlTimer.Finished())
        {
            LOG_WARN("Thermostat did not enter bootloader");
            lastError = FirmwareError::BadState;
            otaState  = OtaState::Failed;
        }
        return;
    }

    if (otaState != OtaState::Idle)
    {
        return; // no display chatter mid-transfer -- the Thermostat UI is down
    }

    if (displayTimer.Finished() || !displayTimer.IsRunning())
    {
        PushDisplay();
        displayTimer.Start(displayPushMs);
    }
}

void ThermostatLink::PushDisplay()
{
    if (!link.LinkUp())
    {
        return;
    }
    const uint8_t actual = damper.Actual();
    const uint8_t mode   = damper.ReportedMode();
    if (actual != lastPushedActual)
    {
        link.SendToPeer(Endpoint::DamperActual, Operation::Set, &actual, 1);
        lastPushedActual = actual;
    }
    if (mode != lastPushedMode)
    {
        link.SendToPeer(Endpoint::DamperMode, Operation::Set, &mode, 1);
        lastPushedMode = mode;
    }
}

void ThermostatLink::ReceivedMessage(const Message& m)
{
    if (m.id.operation == Operation::Report)
    {
        switch (m.id.endpoint)
        {
            case Endpoint::RoomSetpoint:
                if (m.len >= 2)
                {
                    room.setpoint = static_cast<int16_t>(ReadU16(m.data));
                    room.valid    = true;
                }
                break;
            case Endpoint::RoomTemp:
                if (m.len >= 2)
                {
                    room.temp  = static_cast<int16_t>(ReadU16(m.data));
                    room.valid = true;
                }
                break;
            case Endpoint::RoomHumidity:
                if (m.len >= 2)
                {
                    room.humidity = ReadU16(m.data);
                }
                break;
            case Endpoint::RoomMode:
                if (m.len >= 1)
                {
                    room.mode = m.data[0];
                }
                break;
            case Endpoint::SystemInfo:
                // module(1) hwRev(1) fwMajor(2) fwMinor(2)
                if (m.len >= 6)
                {
                    thermostatFw = static_cast<uint16_t>((ReadU16(&m.data[2]) << 8) | ReadU16(&m.data[4]));
                }
                break;
            case Endpoint::Firmware:
                // FirmwareOp::Status from the peer's bootloader: op state
                // expectedOffset(4) lastError fwVersion(2)
                if (m.len >= 9 && m.data[0] == static_cast<uint8_t>(FirmwareOp::Status))
                {
                    peerState      = m.data[1];
                    expectedOffset = ReadU32(&m.data[2]);
                    lastError      = static_cast<FirmwareError>(m.data[6]);
                    thermostatFw   = ReadU16(&m.data[7]);
                    if (peerState == StateReceiving && otaState == OtaState::Transferring)
                    {
                        LOG_INFO("Thermostat OTA at offset " << expectedOffset);
                    }
                }
                break;
            default:
                break;
        }
    }
}

void ThermostatLink::ConnectionLost()
{
    LOG_WARN("Thermostat link down");
    room.valid = false;
    if (otaState == OtaState::EnteringBootloader || otaState == OtaState::Transferring)
    {
        lastError = FirmwareError::LinkDown;
        otaState  = OtaState::Failed;
    }
}

void ThermostatLink::PushSetpoint(const int16_t centiDegC)
{
    const uint8_t payload[2] = {
        static_cast<uint8_t>(centiDegC),
        static_cast<uint8_t>(static_cast<uint16_t>(centiDegC) >> 8),
    };
    link.SendToPeer(Endpoint::RoomSetpoint, Operation::Set, payload, sizeof(payload));
}

const ThermostatLink::RoomState& ThermostatLink::Room() const
{
    return room;
}

bool ThermostatLink::LinkUp() const
{
    return link.LinkUp();
}

uint16_t ThermostatLink::ThermostatFwVersion() const
{
    return thermostatFw;
}

uint8_t ThermostatLink::ThermostatBlState() const
{
    return peerState;
}

// --- firmware relay ------------------------------------------------------

bool ThermostatLink::OtaBegin(const uint8_t module, const uint32_t imageSize, const uint32_t imageCrc32, const uint16_t fwVersion, const bool force)
{
    if (!force && thermostatFw != 0 && fwVersion == thermostatFw)
    {
        LOG_INFO("Thermostat already on fw " << fwVersion << " -- skipping");
        lastError = FirmwareError::AlreadyCurrent;
        otaState  = OtaState::Idle;
        return false;
    }

    beginPayload[0] = static_cast<uint8_t>(FirmwareOp::Begin);
    beginPayload[1] = module;
    WriteU32(&beginPayload[2], imageSize);
    WriteU32(&beginPayload[6], imageCrc32);
    beginPayload[10] = static_cast<uint8_t>(fwVersion);
    beginPayload[11] = static_cast<uint8_t>(fwVersion >> 8);

    lastError      = FirmwareError::None;
    expectedOffset = 0;

    if (link.PeerInBootloader())
    {
        SendFirmwareOp(FirmwareOp::Begin, &beginPayload[1], sizeof(beginPayload) - 1);
        otaState = OtaState::Transferring;
    }
    else
    {
        SendFirmwareOp(FirmwareOp::EnterBootloader, nullptr, 0);
        enterBlTimer.Start(enterBlTimeoutMs);
        otaState = OtaState::EnteringBootloader;
    }
    return true;
}

void ThermostatLink::OtaWrite(const uint32_t offset, const uint8_t* const bytes, const uint8_t len)
{
    if (otaState != OtaState::Transferring)
    {
        return;
    }
    uint8_t payload[4 + 27];
    WriteU32(payload, offset);
    const uint8_t n = len > 27 ? 27 : len;
    memcpy(&payload[4], bytes, n);
    SendFirmwareOp(FirmwareOp::Write, payload, static_cast<uint8_t>(4 + n));
}

void ThermostatLink::OtaEnd()
{
    if (otaState == OtaState::Transferring)
    {
        SendFirmwareOp(FirmwareOp::End, nullptr, 0);
    }
}

void ThermostatLink::OtaActivate()
{
    SendFirmwareOp(FirmwareOp::Activate, nullptr, 0);
    otaState = OtaState::Done;
}

void ThermostatLink::OtaAbort()
{
    SendFirmwareOp(FirmwareOp::Abort, nullptr, 0);
    lastError = FirmwareError::None;
    otaState  = OtaState::Idle;
}

void ThermostatLink::SendFirmwareOp(const FirmwareOp op, const uint8_t* const payload, const uint8_t len)
{
    uint8_t buf[1 + 31];
    buf[0]          = static_cast<uint8_t>(op);
    const uint8_t n = len > 31 ? 31 : len;
    if (payload && n)
    {
        memcpy(&buf[1], payload, n);
    }
    link.SendToPeer(Endpoint::Firmware, Operation::Set, buf, static_cast<uint8_t>(1 + n));
    link.PollPeerNow();
}

void ThermostatLink::FillOtaStatus(uint8_t out[9]) const
{
    out[0] = static_cast<uint8_t>(FirmwareOp::Status);
    out[1] = (otaState == OtaState::Idle || otaState == OtaState::Done) ? StateApp : peerState;
    WriteU32(&out[2], expectedOffset);
    out[6] = static_cast<uint8_t>(lastError);
    out[7] = static_cast<uint8_t>(thermostatFw);
    out[8] = static_cast<uint8_t>(thermostatFw >> 8);
}
