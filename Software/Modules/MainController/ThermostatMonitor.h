/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

#include "DelayTimer.h"

#include "Id.h"
#include "NodeMaster.h"

// Tells the server about the Thermostat behind each ControllerNode: link
// state, bootloader state and running firmware, as the 0x63 ThermostatStatus
// uplink Report (ControllerNode-Thermostat-Link-Spec.md §5.6,
// MainController-Server-Link-Spec.md §5).
//
// Built from the ControllerNode's own RoomLink and ThermostatFirmware Status
// Reports -- watched as they pass (Observe(), fed from
// UplinkHandler::ReceivedMessage()) and asked for with a Get, one
// ControllerNode per pollStepMs so the bus queue never sees a burst. A
// ControllerNode whose Thermostat is in its bootloader (an OTA push is
// running) is not polled: that push's own Status Reports keep the picture
// current, and extra ones would only add noise to it.
//
// A 0x63 goes out when a Thermostat's picture changes, for every known one on
// each uplink (re)connect, and as a keepalive every keepaliveMs. The uid field
// is zeros: the ControllerNode doesn't carry its Thermostat's MCU id.
class ThermostatMonitor
{
  public:
    explicit ThermostatMonitor(NodeLib::NodeMaster& master);

    void Observe(const NodeLib::Message& m); // every bus frame, from UplinkHandler::ReceivedMessage()
    void Loop(); // polls one ControllerNode per pollStepMs; call only while the uplink is up
    void ResendAll(); // uplink (re)connected -- every known Thermostat is due now

    // The next 0x63 ThermostatStatus Report that is due; false if none.
    bool NextStatus(NodeLib::Message& out);

  private:
    struct SThermostat
    {
        SThermostat();

        bool Known() const;

        bool     seenLink;
        bool     seenFirmware;
        bool     linkUp;
        uint8_t  blState; // ThermostatFirmware Status state byte: 0 = application
        uint16_t fwVersion; // major << 8 | minor, 0 = not known yet
        bool     due;
    };

    static const uint32_t pollStepMs  = 2000;
    static const uint32_t keepaliveMs = 60000;

    bool IsControllerNode(const uint8_t nodeId) const;
    void PollNext();

    NodeLib::NodeMaster& master;
    SThermostat          thermostats[NodeLib::MAX_NODES]; // indexed nodeId - 1
    uint8_t              pollCursor; // last nodeId polled
    Tools::DelayTimer    pollTimer;
    Tools::DelayTimer    keepaliveTimer;
};
