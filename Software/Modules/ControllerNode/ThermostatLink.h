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
        bool     setpointValid;
        bool     tempValid; // reported, and refreshed within NodeLib::RoomTempStaleMs
        bool     valid; // both -- what the room loop needs
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
    void OtaWrite(const uint16_t offset, const uint8_t* const bytes, const uint8_t len);
    void OtaEnd();
    void OtaActivate();
    void OtaAbort();

    // Fills a FirmwareOp::Status-shaped payload (9 bytes) for the ControllerNode
    // to report up as ThermostatFirmware.
    void FillOtaStatus(uint8_t out[9]) const;

    // A Write's Ack/Nack arrives asynchronously over the link, well after
    // OtaWrite() returns (ControllerNode-Thermostat-Link-Spec.md §5.4 --
    // "terminates and re-originates", not a direct forward) -- unlike Report,
    // which the caller answers synchronously from whatever's cached. Called
    // from Loop() once per reply; returns false (out-params untouched) when
    // nothing is waiting to be relayed up the main bus.
    bool ConsumeWriteReply(bool& nack, uint16_t& offset, uint16_t& chunkCrc16, bool& programFailed);

    // True once per change of the Status that FillOtaStatus() would report
    // (peer state byte, or the relay giving up). The ControllerNode then
    // Reports ThermostatFirmware unsolicited: Begin/End's outcome and a
    // Thermostat dropping into or out of its bootloader only become known
    // here, well after the main-bus request that caused them was answered.
    bool ConsumeStatusChange();

  private:
    static const uint32_t displayPushMs    = 1000;
    static const uint32_t enterBlTimeoutMs = 4000;
    // While EnteringBootloader, re-Discover this often: the peer's bootloader
    // Announces only in reply to a Discover, and LinkMaster's periodic one
    // (5 s) is slower than enterBlTimeoutMs.
    static const uint32_t enterBlDiscoverMs = 500;
    // How often SystemInfo is asked for until the Thermostat answers.
    static const uint32_t fwQueryMs = 5000;

    void SendFirmwareOp(const NodeLib::FirmwareOp op, const uint8_t* const payload, const uint8_t len);
    void SetPeerState(const uint8_t state);
    void OnOpReply(const bool nack, const uint8_t error);
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
    // Peer's FirmwareSlave state byte (0 = app), from whichever arrived last:
    // its Announce, a Status Report, or the Ack/Nack to Begin/End/Abort.
    uint8_t             peerState;
    NodeLib::FirmwareOp pendingOp; // last Begin/End/Abort sent, awaiting its Ack/Nack
    bool                statusChanged;
    Tools::DelayTimer   enterBlTimer;
    Tools::DelayTimer   enterBlDiscoverTimer;
    // EnteringBootloader: the first "in the bootloader" Announce has been seen.
    // That first one is normally the app's own, sent just before it resets
    // (Node::HandleFirmwareMessage()) while the bootloader is not listening
    // yet -- so Begin waits for the next one, the bootloader's answer to a
    // fresh Discover.
    bool bootloaderHintSeen;

    // Restarted by every RoomTemp report; on expiry the room temperature is
    // dropped (NodeLib::RoomTempStaleMs).
    Tools::DelayTimer roomTempTimer;

    // The Thermostat's running version is only known from its SystemInfo (or
    // its bootloader's Status during a push), and nothing reports SystemInfo
    // unasked -- so it is asked for whenever the app is up on the link and
    // hasn't answered since it (re)appeared.
    void              QueryFirmwareVersion();
    bool              fwKnown;
    Tools::DelayTimer fwQueryTimer;

    // Outcome of the write currently (or most recently) in flight on the
    // link, awaiting relay up the main bus -- see ConsumeWriteReply().
    uint8_t  pendingWriteLen; // length of that write, to advance expectedOffset on success
    bool     writeReplyPending;
    bool     writeReplyNack;
    uint16_t writeReplyOffset;
    uint16_t writeReplyCrc16;
    bool     writeReplyProgramFailed;

    Tools::DelayTimer displayTimer;
    uint8_t           lastPushedActual;
    uint8_t           lastPushedMode;
};
