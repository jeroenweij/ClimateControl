/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include <string.h>

#include "Logger.h"

#include "EEndpoint.h"
#include "EOperation.h"

#include "RoomDemand.h"

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

    // Peer Status state byte: 0 = app; the bootloader never reports 0
    // (Modules/Bootloader/FirmwareSlave.h State).
    constexpr uint8_t StateApp       = 0;
    constexpr uint8_t StateIdle      = 1;
    constexpr uint8_t StateReceiving = 3;
    constexpr uint8_t StateValid     = 4;
    constexpr uint8_t StateError     = 5;

    // Write's Ack/Nack: byteOffset(2) chunkCrc16(2) programFailed(1).
    // Begin/End/Abort's: lastError(1).
    constexpr uint8_t WriteReplyLen = 5;
} // namespace

ThermostatLink::ThermostatLink(NodeLib::LinkMaster& link, Damper& damper) :
    link(link),
    damper(damper),
    room{0, 0, 0, 0, false, false, false},
    thermostatFw(0),
    otaState(OtaState::Idle),
    lastError(FirmwareError::None),
    beginPayload{},
    expectedOffset(0),
    peerState(StateApp),
    pendingOp(FirmwareOp::Status),
    statusChanged(false),
    enterBlTimer(),
    enterBlDiscoverTimer(),
    bootloaderHintSeen(false),
    roomTempTimer(),
    fwKnown(false),
    fwQueryTimer(),
    pendingWriteLen(0),
    writeReplyPending(false),
    writeReplyNack(false),
    writeReplyOffset(0),
    writeReplyCrc16(0),
    writeReplyProgramFailed(false),
    displayTimer(),
    lastPushedActual(0xFF),
    lastPushedMode(0xFF)
{
}

void ThermostatLink::Loop()
{
    if (room.tempValid && roomTempTimer.Finished())
    {
        LOG_WARN("Room temperature not refreshed -- dropped");
        room.tempValid = false;
        room.valid     = false;
    }

    if (otaState == OtaState::EnteringBootloader)
    {
        if (link.PeerInBootloader() && !bootloaderHintSeen)
        {
            // The app announces the bootloader before it actually resets: a
            // Begin sent now lands mid-reset and is lost, leaving the
            // bootloader idle. Ask again -- only the bootloader itself can
            // answer this Discover (Rediscover() clears the flag until then).
            bootloaderHintSeen = true;
            link.Rediscover();
            enterBlDiscoverTimer.ReStart();
        }
        else if (link.PeerInBootloader())
        {
            SendFirmwareOp(FirmwareOp::Begin, &beginPayload[1], sizeof(beginPayload) - 1);
            pendingOp = FirmwareOp::Begin;
            otaState  = OtaState::Transferring;
        }
        else if (enterBlTimer.Finished())
        {
            LOG_WARN("Thermostat did not enter bootloader");
            lastError     = FirmwareError::BadState;
            otaState      = OtaState::Failed;
            statusChanged = true;
        }
        else if (enterBlDiscoverTimer.Finished())
        {
            link.Rediscover();
            enterBlDiscoverTimer.ReStart();
        }
        return;
    }

    if (otaState != OtaState::Transferring)
    {
        QueryFirmwareVersion();
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
    if (m.id.operation == Operation::Announce)
    {
        // Handed over by LinkMaster: the bootloader's carries its state byte
        // at data[1], the app's has none.
        SetPeerState(m.len >= 2 ? m.data[1] : StateApp);
    }
    else if (m.id.operation == Operation::Report)
    {
        switch (m.id.endpoint)
        {
            case Endpoint::RoomSetpoint:
                if (m.len >= 2)
                {
                    room.setpoint      = static_cast<int16_t>(ReadU16(m.data));
                    room.setpointValid = true;
                    room.valid         = room.tempValid;
                }
                break;
            case Endpoint::RoomTemp:
                if (m.len >= 2)
                {
                    room.temp      = static_cast<int16_t>(ReadU16(m.data));
                    room.tempValid = true;
                    room.valid     = room.setpointValid;
                    roomTempTimer.Start(NodeLib::RoomTempStaleMs);
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
                    fwKnown      = true;
                    fwQueryTimer.Stop();
                }
                break;
            case Endpoint::Firmware:
                // FirmwareOp::Status from the peer's bootloader: op state
                // expectedOffset(4) lastError fwVersion(2)
                if (m.len >= 9 && m.data[0] == static_cast<uint8_t>(FirmwareOp::Status))
                {
                    SetPeerState(m.data[1]);
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
    else if (m.id.operation == Operation::Ack || m.id.operation == Operation::Nack)
    {
        // Ack/Nack reply to a Write, from the peer's bootloader
        // (Node-Flash-Layout-and-Bootloader-Spec.md §6.2.1): byteOffset(2)
        // chunkCrc16(2) programFailed(1). Relayed up the main bus from
        // Loop() via ConsumeWriteReply(), not answered here -- the CN
        // terminates and re-originates rather than forwarding.
        if (m.id.endpoint == Endpoint::Firmware && m.len >= 1 && m.len < WriteReplyLen)
        {
            OnOpReply(m.id.operation == Operation::Nack, m.data[0]); // Begin/End/Abort
        }
        else if (m.id.endpoint == Endpoint::Firmware && m.len >= WriteReplyLen && otaState == OtaState::Transferring)
        {
            const bool     nack          = (m.id.operation == Operation::Nack);
            const uint16_t offset        = ReadU16(m.data);
            const uint16_t chunkCrc16    = ReadU16(&m.data[2]);
            const bool     programFailed = m.data[4] != 0;

            expectedOffset = nack ? offset : offset + pendingWriteLen;
            if (programFailed)
            {
                lastError = FirmwareError::ProgramFailed;
            }

            writeReplyPending       = true;
            writeReplyNack          = nack;
            writeReplyOffset        = offset;
            writeReplyCrc16         = chunkCrc16;
            writeReplyProgramFailed = programFailed;
        }
    }
}

void ThermostatLink::OnOpReply(const bool nack, const uint8_t error)
{
    if (nack)
    {
        lastError = static_cast<FirmwareError>(error);
        SetPeerState(StateError);
        return;
    }
    const FirmwareOp op = pendingOp;
    pendingOp           = FirmwareOp::Status; // nothing awaiting a reply
    switch (op)
    {
        case FirmwareOp::Begin:
            expectedOffset = 0;
            SetPeerState(StateReceiving);
            break;
        case FirmwareOp::End:
            SetPeerState(StateValid);
            break;
        case FirmwareOp::Abort:
            SetPeerState(StateIdle);
            break;
        default:
            break;
    }
}

void ThermostatLink::SetPeerState(const uint8_t state)
{
    if (state != peerState)
    {
        peerState     = state;
        statusChanged = true;
        if (state != StateApp)
        {
            fwKnown = false; // through the bootloader: ask the app again once it is back
        }
    }
}

void ThermostatLink::QueryFirmwareVersion()
{
    if (fwKnown || !link.LinkUp() || link.PeerInBootloader())
    {
        return;
    }
    if (fwQueryTimer.IsRunning() && !fwQueryTimer.Finished())
    {
        return;
    }
    link.GetFromPeer(Endpoint::SystemInfo);
    fwQueryTimer.Start(fwQueryMs);
}

void ThermostatLink::ConnectionLost()
{
    LOG_WARN("Thermostat link down");
    room.valid         = false;
    room.tempValid     = false;
    room.setpointValid = false;
    roomTempTimer.Stop();
    fwKnown = false; // whatever comes back may be a different build
    // An OTA in flight is not failed here: the peer is silent for a moment
    // both while it resets into the bootloader and while Begin erases the app
    // slot (~0.5-1 s, longer than linkTimeoutMs). enterBlTimer bounds the
    // first; the server's own per-step timeouts bound the transfer.
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
        pendingOp = FirmwareOp::Begin;
        otaState  = OtaState::Transferring;
    }
    else
    {
        SendFirmwareOp(FirmwareOp::EnterBootloader, nullptr, 0);
        bootloaderHintSeen = false;
        enterBlTimer.Start(enterBlTimeoutMs);
        enterBlDiscoverTimer.Start(enterBlDiscoverMs);
        otaState = OtaState::EnteringBootloader;
    }
    return true;
}

void ThermostatLink::OtaWrite(const uint16_t offset, const uint8_t* const bytes, const uint8_t len)
{
    if (otaState != OtaState::Transferring)
    {
        return;
    }
    uint8_t payload[2 + 32];
    payload[0]      = static_cast<uint8_t>(offset);
    payload[1]      = static_cast<uint8_t>(offset >> 8);
    const uint8_t n = len > 32 ? 32 : len;
    memcpy(&payload[2], bytes, n);
    pendingWriteLen = n;
    SendFirmwareOp(FirmwareOp::Write, payload, static_cast<uint8_t>(2 + n));
}

bool ThermostatLink::ConsumeWriteReply(bool& nack, uint16_t& offset, uint16_t& chunkCrc16, bool& programFailed)
{
    if (!writeReplyPending)
    {
        return false;
    }
    nack              = writeReplyNack;
    offset            = writeReplyOffset;
    chunkCrc16        = writeReplyCrc16;
    programFailed     = writeReplyProgramFailed;
    writeReplyPending = false;
    return true;
}

bool ThermostatLink::ConsumeStatusChange()
{
    const bool changed = statusChanged;
    statusChanged      = false;
    return changed;
}

void ThermostatLink::OtaEnd()
{
    if (otaState == OtaState::Transferring)
    {
        SendFirmwareOp(FirmwareOp::End, nullptr, 0);
        pendingOp = FirmwareOp::End;
    }
}

void ThermostatLink::OtaActivate()
{
    SendFirmwareOp(FirmwareOp::Activate, nullptr, 0);
    otaState = OtaState::Done;
    // The peer resets into the new app without replying; if that app doesn't
    // start, the next periodic Discover's Announce puts the bootloader back.
    SetPeerState(StateApp);
}

void ThermostatLink::OtaAbort()
{
    SendFirmwareOp(FirmwareOp::Abort, nullptr, 0);
    pendingOp = FirmwareOp::Abort;
    lastError = FirmwareError::None;
    otaState  = OtaState::Idle;
}

void ThermostatLink::SendFirmwareOp(const FirmwareOp op, const uint8_t* const payload, const uint8_t len)
{
    // A Write is op(1) + byteOffset(2) + data(32) = MAX_DATA.
    uint8_t buf[NodeLib::MAX_DATA];
    buf[0]          = static_cast<uint8_t>(op);
    const uint8_t n = len > NodeLib::MAX_DATA - 1 ? NodeLib::MAX_DATA - 1 : len;
    if (payload && n)
    {
        memcpy(&buf[1], payload, n);
    }
    link.SendToPeer(Endpoint::Firmware, Operation::Set, buf, static_cast<uint8_t>(1 + n));
}

void ThermostatLink::FillOtaStatus(uint8_t out[9]) const
{
    out[0] = static_cast<uint8_t>(FirmwareOp::Status);
    out[1] = peerState;
    WriteU32(&out[2], expectedOffset);
    out[6] = static_cast<uint8_t>(lastError);
    out[7] = static_cast<uint8_t>(thermostatFw);
    out[8] = static_cast<uint8_t>(thermostatFw >> 8);
}
