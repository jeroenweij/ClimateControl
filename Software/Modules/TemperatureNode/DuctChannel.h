/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include "DelayTimer.h"
#include "Pin.h"

#include "EEndpoint.h"
#include "Node.h"

#include "Ds18b20.h"

// One duct temperature probe (a DS18B20 on its own single-drop 1-Wire line --
// TemperatureNode-Spec.md §4.1) mapped onto one bus endpoint.
//
// Mirrors the auto-reporting ANALOG_IN channel from the AVR predecessor
// (~/git/node Channel::Loop): sample on an interval, and push a Report only when
// the reading has moved past a threshold or a minimum refresh interval has
// elapsed -- so MainController still sees fresh values during a long stretch of
// unchanging duct air (TemperatureNode-Spec.md §5 item 3). Solicited Get is
// answered immediately via Report().
//
// The DS18B20 conversion (~750 ms) is not blocked on: the channel kicks it off,
// returns to the caller, and reads the result on a later Loop(). The 1-Wire
// transactions themselves do block -- ~2 ms to start a conversion, ~7 ms to read
// the scratchpad back -- so a Loop() that touches the probe stalls the bus
// service for that long, roughly twice per SampleIntervalMs.
class DuctChannel
{
  public:
    DuctChannel(NodeLib::Node& node, const NodeLib::Endpoint endpoint, const Hal::Pin oneWirePin);

    void Init();
    void Loop();

    // Emit a Report with the current value (reply to a Get, or on reconnect).
    void Report();

    // Drop the "already reported" latch so the next Loop() re-sends the value --
    // used when the bus link has been re-established.
    void Invalidate();

    bool    Present() const;
    int16_t Value() const;

  private:
    enum class State
    {
        Idle, // waiting out sampleTimer before the next conversion
        Converting, // CONVERT T issued, waiting out conversionTimer
    };

    void SetPresent(const bool present);
    void PublishIfDue();

    NodeLib::Node&    node;
    NodeLib::Endpoint endpoint;
    Ds18b20           sensor;

    State   state;
    int16_t value;
    int16_t lastReported;
    bool    everReported;
    bool    present;

    Tools::DelayTimer sampleTimer;
    Tools::DelayTimer conversionTimer;
    Tools::DelayTimer minReportTimer;
};
