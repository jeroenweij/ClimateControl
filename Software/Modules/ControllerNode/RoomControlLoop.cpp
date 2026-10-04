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
    budgetMax(defaultBudget),
    budgetMin(0),
    dumpRoom(false),
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

    // Without a supply reading or without room data (no Thermostat, or its
    // temperature stopped arriving) the room can't be controlled: fail safe
    // to the neutral position, the same as having no control at all -- still
    // inside the budget range, which MainController keeps at 50% for such a
    // room (Damper-Budget-Spec.md §4.4).
    uint8_t desired = Damper::NeutralPercent;
    if (supplyTemp.Valid() && thermostatLink.Room().valid)
    {
        desired = NodeLib::RoomDemandPercent(supplyTemp.CentiDegC(), thermostatLink.Room().temp, thermostatLink.Room().setpoint);
    }

    uint8_t target = desired < budgetMax ? desired : budgetMax;
    target         = target > budgetMin ? target : budgetMin;
    // SetTarget() (re)starts a move -- repeating the target the damper already
    // has on every pass would keep restarting it, so the move would never
    // settle and a stall would never be confirmed.
    if (target != damper.Target())
    {
        damper.SetTarget(target);
    }
}

void RoomControlLoop::SetBudget(const uint8_t max, const uint8_t min)
{
    budgetMin      = min > 100 ? 100 : min;
    budgetMax      = max > 100 ? 100 : max;
    budgetMax      = budgetMax > budgetMin ? budgetMax : budgetMin;
    connectionLost = false;
    rampTimer.Stop();

    ClampIntoRange(); // at once, not just on the next Loop() tick
}

void RoomControlLoop::ClampIntoRange()
{
    if (damper.GetMode() != Damper::Mode::Auto)
    {
        return;
    }
    if (damper.Target() > budgetMax)
    {
        damper.SetTarget(budgetMax);
    }
    else if (damper.Target() < budgetMin)
    {
        damper.SetTarget(budgetMin);
    }
}

uint8_t RoomControlLoop::BudgetMax() const
{
    return budgetMax;
}

uint8_t RoomControlLoop::BudgetMin() const
{
    return budgetMin;
}

void RoomControlLoop::SetDumpRoom(const bool isDumpRoom)
{
    dumpRoom = isDumpRoom;
}

bool RoomControlLoop::DumpRoom() const
{
    return dumpRoom;
}

void RoomControlLoop::ConnectionLost()
{
    connectionLost = true;
    rampTimer.Start(rampIntervalMs);

    // Without MainController nobody is watching the total airflow any more:
    // the dump room opens fully, every other room drops its minimum.
    budgetMin = dumpRoom ? 100 : 0;
    budgetMax = budgetMax > budgetMin ? budgetMax : budgetMin;
    if (dumpRoom)
    {
        LOG_WARN("Bus lost -- dump room opening fully");
    }
    ClampIntoRange();
}

void RoomControlLoop::StepBudgetRamp()
{
    if (!rampTimer.Finished())
    {
        return;
    }
    rampTimer.Start(rampIntervalMs);

    // The dump room's max is pinned at 100 by its min (ConnectionLost()) and
    // stays there; every other room ramps its ceiling back to the default.
    if (budgetMax < defaultBudget)
    {
        const uint8_t next = static_cast<uint8_t>(budgetMax + rampStepPercent);
        budgetMax          = next > defaultBudget ? defaultBudget : next;
        LOG_INFO("Damper budget ramping toward default: " << budgetMax << "%");
    }
    else if (budgetMax > defaultBudget && budgetMax > budgetMin)
    {
        const uint8_t next = static_cast<uint8_t>(budgetMax - rampStepPercent);
        budgetMax          = next < defaultBudget ? defaultBudget : next;
        budgetMax          = budgetMax > budgetMin ? budgetMax : budgetMin;
        LOG_INFO("Damper budget ramping toward default: " << budgetMax << "%");
    }
}
