/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "Logger.h"

#include "RoomDemand.h"

#include "RoomControlLoop.h"

RoomControlLoop::RoomControlLoop(ThermostatLink& thermostatLink, const NodeLib::SupplyTemp& supplyTemp, Damper& damper) :
    thermostatLink(thermostatLink),
    supplyTemp(supplyTemp),
    damper(damper),
    budget(defaultBudget),
    connectionLost(false),
    rampTimer()
{
}

void RoomControlLoop::Loop()
{
    if (connectionLost)
    {
        StepBudgetRamp();
    }

    if (damper.GetMode() != Damper::Mode::Auto)
    {
        return;
    }

    // Without a supply reading the room can't be controlled: fail safe to the
    // neutral position, the same as having no control at all -- still under
    // the budget, which MainController walks back to 50% meanwhile
    // (Damper-Budget-Spec.md §4.4).
    uint8_t desired = Damper::NeutralPercent;
    if (supplyTemp.Valid())
    {
        if (!thermostatLink.Room().valid)
        {
            return;
        }
        desired = NodeLib::RoomDemandPercent(supplyTemp.CentiDegC(), thermostatLink.Room().temp, thermostatLink.Room().setpoint);
    }

    const uint8_t target = desired < budget ? desired : budget;
    // SetTarget() (re)starts a move -- repeating the target the damper already
    // has on every pass would keep restarting it, so the move would never
    // settle and a stall would never be confirmed.
    if (target != damper.Target())
    {
        damper.SetTarget(target);
    }
}

void RoomControlLoop::SetBudget(const uint8_t percent)
{
    budget         = percent > 100 ? 100 : percent;
    connectionLost = false;
    rampTimer.Stop();

    if (damper.GetMode() == Damper::Mode::Auto && damper.Target() > budget)
    {
        damper.SetTarget(budget); // re-clamp immediately, not just on the next Loop() tick
    }
}

uint8_t RoomControlLoop::Budget() const
{
    return budget;
}

void RoomControlLoop::ConnectionLost()
{
    connectionLost = true;
    rampTimer.Start(rampIntervalMs);
}

void RoomControlLoop::StepBudgetRamp()
{
    if (!rampTimer.Finished())
    {
        return;
    }
    rampTimer.Start(rampIntervalMs);

    if (budget < defaultBudget)
    {
        const uint8_t next = static_cast<uint8_t>(budget + rampStepPercent);
        budget             = next > defaultBudget ? defaultBudget : next;
        LOG_INFO("Damper budget ramping toward default: " << budget << "%");
    }
    else if (budget > defaultBudget)
    {
        const uint8_t next = static_cast<uint8_t>(budget - rampStepPercent);
        budget             = next < defaultBudget ? defaultBudget : next;
        LOG_INFO("Damper budget ramping toward default: " << budget << "%");
    }
}
