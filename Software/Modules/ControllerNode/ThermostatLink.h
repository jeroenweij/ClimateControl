/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

#include "DelayTimer.h"
#include "EFirmware.h"
#include "INodeHandler.h"
#include "LinkMaster.h"

#include "Damper.h"

// The ControllerNode side of the point-to-point link to its paired Thermostat
// (ControllerNode-Thermostat-Link-Spec.md §5). Registered as the LinkMaster's
// handler. Responsibilities:
//   - cache the Thermostat's Room* state (it is the source of truth) so the
//     main-bus Room* endpoints can be answered without link traffic
//   - push DamperActual / DamperMode to the Thermostat for its display
//   - relay a firmware image to the Thermostat: terminate the main-bus
//     ThermostatFirmware ops and re-originate Firmware ops on the link (§5.4)
class ThermostatLink : public NodeLib::INodeHandler
{
  public:
    struct RoomState
    {
        int16_t  setpoint; // centi-degC
        int16_t  temp; // centi-degC
        uint16_t humidity; // centi-%RH
        uint8_t  mode; // DamperMode coding
        bool     valid;
    };

    ThermostatLink(NodeLib::LinkMaster& link, Damper& damper);

    void Loop();

    void ReceivedMessage(const NodeLib::Message& message) override;
    void ConnectionLost() override;

    const RoomState& Room() const;
    bool             LinkUp() const;

    // Master setpoint override -- pushed down the link to the Thermostat
    // (ControllerNode-Thermostat-Link-Spec.md §5.3 / Node-Message-Model §3).
    void PushSetpoint(const int16_t centiDegC);
    // Packed (major << 8) | minor, matching the Firmware protocol's fwVersion.
    uint16_t ThermostatFwVersion() const;
    uint8_t  ThermostatBlState() const;

    // --- firmware relay, driven by the main-bus ThermostatFirmware handler ---
    enum class OtaState : uint8_t
    {
        Idle,
        EnteringBootloader,
        Transferring,
        Done,
        Failed,
    };

    // Returns false and sets lastError when the request is refused outright
    // (e.g. AlreadyCurrent) -- nothing goes on the link in that case.
    bool OtaBegin(const uint8_t module, const uint32_t imageSize, const uint32_t imageCrc32, const uint16_t fwVersion, const bool force);
    void OtaWrite(const uint32_t offset, const uint8_t* const bytes, const uint8_t len);
    void OtaEnd();
    void OtaActivate();
    void OtaAbort();

    // Fills a FirmwareOp::Status-shaped payload (9 bytes) for the ControllerNode
    // to report up as ThermostatFirmware.
    void FillOtaStatus(uint8_t out[9]) const;

  private:
    static const uint32_t displayPushMs    = 1000;
    static const uint32_t enterBlTimeoutMs = 4000;

    void SendFirmwareOp(const NodeLib::FirmwareOp op, const uint8_t* const payload, const uint8_t len);
    void PushDisplay();

    NodeLib::LinkMaster& link;
    Damper&              damper;

    RoomState room;
    uint16_t  thermostatFw; // packed (major << 8) | minor

    // OTA relay
    OtaState               otaState;
    NodeLib::FirmwareError lastError;
    uint8_t                beginPayload[12]; // cached Firmware[Begin] body
    uint32_t               expectedOffset;
    uint8_t                peerState; // last state byte from the peer's Status
    Tools::DelayTimer      enterBlTimer;

    Tools::DelayTimer displayTimer;
    uint8_t           lastPushedActual;
    uint8_t           lastPushedMode;
};
