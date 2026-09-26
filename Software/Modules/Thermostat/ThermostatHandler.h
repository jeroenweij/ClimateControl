/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include "DelayTimer.h"
#include "Gpio.h"
#include "I2c.h"
#include "INodeHandler.h"
#include "Node.h"

#include "Cht40.h"
#include "Ssd1306.h"

// Thermostat application logic (INodeHandler for the point-to-point link Node).
//
// The Thermostat is an ordinary NodeLib slave -- the link being 1:1 changes
// nothing on the slave side (ControllerNode-Thermostat-Link-Spec.md §5.2). It
// is the SOURCE OF TRUTH for the Room* endpoints and pushes Reports on change;
// it caches DamperActual / DamperMode from the ControllerNode for the display.
class ThermostatHandler : public NodeLib::INodeHandler
{
  public:
    explicit ThermostatHandler(NodeLib::Node& node);

    void Init();
    void Loop();

    void ReceivedMessage(const NodeLib::Message& message) override;
    void ConnectionLost() override;
    void PrepareForReset() override;
    void Snoop(const NodeLib::Message& message) override;

  private:
    void SampleRoom();
    void ServiceButtons(); // +0.5/-0.5 setpoint step, clamped 19.00-23.00 degC
    // What the panel shows, quantized the way it is drawn -- a change the
    // eye can't see (a 0.01 degC step) doesn't cost a redraw.
    struct SView
    {
        int16_t tempTenths;
        int16_t setpointTenths;
        int16_t humidityPercent;
        uint8_t damperBar; // filled pixels of the damper bar
        bool    linkUp;

        bool operator==(const SView& other) const;
    };

    SView CurrentView() const;
    void  RenderDisplay(); // redraws only on a visible change, at most every minRedrawMs
    void  WakeDisplay(); // turns the panel on (if asleep) and restarts its inactivity timer

    void PublishRoom(); // current room values to Node's change-driven publisher
    void ReportRoom(const NodeLib::Endpoint endpoint); // reply to a Get

    NodeLib::Node& node;

    Hal::I2c  i2c;
    Cht40     sensor;
    Ssd1306   display;
    Hal::Gpio oledPower; // VBAT load-switch gate -- HIGH = OLED powered
    Hal::Gpio oledReset; // active-low, pulsed once at bring-up
    Hal::Gpio buttonDown; // Board::UserButton  (PA11) -- setpoint -0.5
    Hal::Gpio buttonUp; // Board::UserButton2 (PA12) -- setpoint +0.5

    // Room* -- source of truth
    int16_t  setpoint; // centi-degC
    int16_t  roomTemp; // centi-degC
    uint16_t humidity; // centi-%RH
    uint8_t  roomMode; // DamperMode coding

    // display cache from the ControllerNode
    uint8_t damperActual;
    uint8_t damperMode;

    bool downWasPressed;
    bool upWasPressed;
    bool linkUp; // any frame at all on this point-to-point link counts (Snoop())
    bool displayOn;

    // A full redraw is a 1 KB I2C transfer (~0.1 s at 100 kHz) that blocks
    // the loop -- and with it the link to the ControllerNode, which gives up
    // after 3 missed 200 ms polls. So: nothing while the view is unchanged,
    // and no more often than minRedrawMs while it is changing.
    SView             shownView;
    bool              redrawNeeded; // woken, or a change is waiting out minRedrawMs
    Tools::DelayTimer redrawTimer;

    Tools::DelayTimer sampleTimer;
    Tools::DelayTimer displayTimer; // panel sleeps when this elapses (§4.2)
};
