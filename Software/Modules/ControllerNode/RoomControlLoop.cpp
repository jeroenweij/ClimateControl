/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "Logger.h"

#include "RoomDemand.h"

#include "RoomControlLoop.h"

RoomControlLoop::RoomControlLoop(ThermostatLink& thermostatLink, const SupplyTemp& supplyTemp, Damper& damper) :
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

    if (damper.GetMode() != Damper::Mode::Auto || !thermostatLink.Room().valid)
    {
        return;
    }

    const uint8_t desired = supplyTemp.Valid()
        ? NodeLib::RoomDemandPercent(supplyTemp.CentiDegC(), thermostatLink.Room().temp, thermostatLink.Room().setpoint)
        : 0; // no supply reading -- don't guess, stay closed

    damper.SetTarget(desired < budget ? desired : budget);
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
