/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include "DelayTimer.h"
#include "INodeHandler.h"
#include "Node.h"

// Thermostat application logic (INodeHandler for the point-to-point link Node).
//
// The Thermostat is an ordinary NodeLib slave -- the link being 1:1 changes
// nothing on the slave side (ControllerNode-Thermostat-Link-Spec.md §5.2). It
// is the SOURCE OF TRUTH for the Room* endpoints and pushes Reports on change;
// it caches DamperActual / DamperMode from the ControllerNode for the display.
//
// Peripheral drivers are not wired yet: the SSD1306 OLED, the CHT40 room sensor
// and the two buttons all hang off I2C1 / GPIO (Board:: pins) and need an I2C
// HAL that does not exist. Sensor values below are placeholders so the reporting
// path is exercised; every peripheral touch point is marked TODO.
class ThermostatHandler : public NodeLib::INodeHandler
{
  public:
    explicit ThermostatHandler(NodeLib::Node& node);

    void Init();
    void Loop();

    void ReceivedMessage(const NodeLib::Message& message) override;
    void ConnectionLost() override;
    void PrepareForReset() override;

  private:
    void SampleRoom(); // TODO: read CHT40 over I2C1
    void ServiceButtons(); // TODO: read Board::UserButton / Board::Button2
    void RenderDisplay(); // TODO: draw to the SSD1306 over I2C1

    void PublishRoom(const bool force);

    NodeLib::Node& node;

    // Room* -- source of truth
    int16_t  setpoint; // centi-degC
    int16_t  roomTemp; // centi-degC
    uint16_t humidity; // centi-%RH
    uint8_t  roomMode; // DamperMode coding

    // last values reported, for on-change publishing
    int16_t  reportedSetpoint;
    int16_t  reportedTemp;
    uint16_t reportedHumidity;
    uint8_t  reportedMode;
    bool     everReported;

    // display cache from the ControllerNode
    uint8_t damperActual;
    uint8_t damperMode;

    Tools::DelayTimer sampleTimer;
    Tools::DelayTimer keepaliveTimer;
};
