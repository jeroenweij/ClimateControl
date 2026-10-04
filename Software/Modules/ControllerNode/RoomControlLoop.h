/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

#include "DelayTimer.h"

#include "Damper.h"
#include "SupplyTemp.h"
#include "ThermostatLink.h"

// The room's control loop (ControllerNode-Thermostat-Link-Spec.md §6 item 1,
// resolved: yes, it runs here -- not on MainController). Drives
// Damper::SetTarget() from ThermostatLink's Room* cache (setpoint/roomTemp)
// and SupplyTemp, kept inside the [min, max] range of whatever DamperBudget
// MainController last set: max is the fair-share ceiling, min is raised so
// enough air keeps flowing while other rooms close (the dump room first).
// Only active while Damper::GetMode() == Auto -- Closed/Open/Manual are
// explicit overrides this loop never touches, budget included
// (Damper-Budget-Spec.md §4.2).
class RoomControlLoop
{
  public:
    RoomControlLoop(ThermostatLink& thermostatLink, const NodeLib::SupplyTemp& supplyTemp, Damper& damper);

    void Loop();

    // Set/Get DamperBudget (Endpoint::DamperBudget), from MainController.
    // Both clamped to 100; a min above max wins (max is raised to it). A
    // fresh Set also proves the main-bus connection is alive -- cancels the
    // disconnect fallback below (Damper-Budget-Spec.md §4.3).
    void    SetBudget(const uint8_t max, const uint8_t min = 0);
    uint8_t BudgetMax() const;
    uint8_t BudgetMin() const;

    // This room is the dump room (Endpoint::DumpRoom, persisted by
    // ControllerHandler): it is what MainController opens first, and what
    // opens by itself when MainController is gone.
    void SetDumpRoom(const bool dumpRoom);
    bool DumpRoom() const;

    // Main-bus connection lost (ControllerHandler::ConnectionLost()) -- max
    // starts its gradual return to defaultBudget, and min drops to 0, or to
    // 100 on the dump room so the unit's air still has somewhere to go.
    void ConnectionLost();

  private:
    void StepBudgetRamp();
    // Moves the damper into [min, max] at once if it sits outside it.
    void ClampIntoRange();

    static const uint8_t defaultBudget = 50;
    // 1 point every 36s -> 30 minutes for the worst-case 50-point gap
    // (defaultBudget sits exactly in the middle of [0,100], so no gap ever
    // exceeds 50 points) -- Damper-Budget-Spec.md §4.3.
    static const uint32_t rampIntervalMs  = 36000;
    static const uint8_t  rampStepPercent = 1;

    ThermostatLink&            thermostatLink;
    const NodeLib::SupplyTemp& supplyTemp;
    Damper&                    damper;

    uint8_t           budgetMax;
    uint8_t           budgetMin;
    bool              dumpRoom;
    bool              connectionLost;
    Tools::DelayTimer rampTimer;
};
