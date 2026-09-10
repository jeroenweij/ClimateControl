/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include "INodeHandler.h"
#include "Node.h"

#include "Damper.h"
#include "ThermostatLink.h"

// ControllerNode main-bus application logic (INodeHandler for the RS485 bus
// Node). Serves:
//   Damper*  (0x30..0x32)  -- this node's own damper
//   Room*    (0x40..0x44)  -- the paired Thermostat's state, from the link cache
//   ThermostatFirmware (0x22) -- relays an image to the Thermostat over the link
//
// System* / Firmware / Diagnostics* / Transport stay inside NodeLib.
class ControllerHandler : public NodeLib::INodeHandler
{
  public:
    ControllerHandler(NodeLib::Node& node, Damper& damper, ThermostatLink& link);

    void Loop();

    void ReceivedMessage(const NodeLib::Message& message) override;
    void ConnectionLost() override;
    void PrepareForReset() override;
    void FillStatus(NodeLib::SystemStatus& status) override;

  private:
    enum ErrorBit : uint16_t
    {
        ThermostatLinkDown = 1u << 0,
    };

    void HandleDamper(const NodeLib::Message& m);
    void HandleRoom(const NodeLib::Message& m);
    void HandleThermostatFirmware(const NodeLib::Message& m);
    void ReportThermostatFirmwareStatus();

    void Report(const NodeLib::Endpoint endpoint, const uint8_t* const data, const uint8_t len);
    void Nack(const NodeLib::Message& m, const uint8_t reason);

    NodeLib::Node&  node;
    Damper&         damper;
    ThermostatLink& thermostatLink;

    uint8_t reportedActual;
    bool    reportedActualValid;
};
