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
// and SupplyTemp, capped by whatever DamperBudget MainController last set.
// Only active while Damper::GetMode() == Auto -- Closed/Open/Manual are
// explicit overrides this loop never touches, budget included
// (Damper-Budget-Spec.md §4.2).
class RoomControlLoop
{
  public:
    RoomControlLoop(ThermostatLink& thermostatLink, const SupplyTemp& supplyTemp, Damper& damper);

    void Loop();

    // Set/Get DamperBudget (Endpoint::DamperBudget), from MainController. A
    // fresh Set also proves the main-bus connection is alive -- cancels the
    // disconnect ramp below (Damper-Budget-Spec.md §4.3).
    void    SetBudget(const uint8_t percent);
    uint8_t Budget() const;

    // Main-bus connection lost (ControllerHandler::ConnectionLost()) -- starts
    // the gradual return to defaultBudget while no fresh budget arrives.
    void ConnectionLost();

  private:
    void StepBudgetRamp();

    static const uint8_t defaultBudget = 50;
    // 1 point every 36s -> 30 minutes for the worst-case 50-point gap
    // (defaultBudget sits exactly in the middle of [0,100], so no gap ever
    // exceeds 50 points) -- Damper-Budget-Spec.md §4.3.
    static const uint32_t rampIntervalMs  = 36000;
    static const uint8_t  rampStepPercent = 1;

    ThermostatLink&   thermostatLink;
    const SupplyTemp& supplyTemp;
    Damper&           damper;

    uint8_t           budget;
    bool              connectionLost;
    Tools::DelayTimer rampTimer;
};
