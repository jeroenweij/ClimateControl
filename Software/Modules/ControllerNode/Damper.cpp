/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "BoardPins.h"
#include "Logger.h"

#include "Damper.h"

namespace
{
    uint8_t Clamp100(const uint8_t percent)
    {
        return percent > 100 ? 100 : percent;
    }
} // namespace

Damper::Damper() :
    enable(Board::ServoEnable, Hal::Gpio::Mode::Output),
    currentSense(Board::ServoCurrentSenseChannel),
    target(NeutralPercent),
    actual(NeutralPercent),
    mode(Mode::Auto),
    powered(false),
    stalled(false),
    settleTimer(),
    stallTimer()
{
}

void Damper::Init()
{
    PowerOff();
}

void Damper::Loop()
{
    if (!powered)
    {
        return;
    }

    if (currentSense.Read() >= stallThresholdCounts)
    {
        if (!stallTimer.IsRunning())
        {
            stallTimer.Start(stallConfirmMs);
        }
        else if (stallTimer.Finished())
        {
            LOG_WARN("Damper stall detected, cutting servo power early");
            stalled = true;
            PowerOff();
            return;
        }
    }
    else
    {
        stallTimer.Stop(); // current dropped back down -- not a sustained stall
    }

    if (settleTimer.Finished())
    {
        // Move settle time elapsed -- assume the actuator reached the target and
        // drop servo power; the gearing holds it there.
        actual = target;
        PowerOff();
    }
}

void Damper::SetTarget(const uint8_t percent)
{
    const uint8_t clamped = Clamp100(percent);
    if (clamped == target && !powered)
    {
        return;
    }
    LOG_INFO("Damper target " << clamped << "%");
    target  = clamped;
    stalled = false; // a fresh move gets a fresh attempt
    PowerOn();
    // TODO: program TIM3_CH1 pulse width for 'target' once a timer HAL exists.
}

void Damper::SetMode(const Mode newMode)
{
    mode = newMode;
    switch (mode)
    {
        case Mode::Closed:
            SetTarget(0);
            break;
        case Mode::Open:
            SetTarget(100);
            break;
        case Mode::Auto:
        case Mode::Manual:
            break; // position driven by the control loop / master
    }
}

void Damper::ParkNeutral()
{
    LOG_WARN("Damper -> neutral, servo off");
    target = NeutralPercent;
    actual = NeutralPercent;
    // TODO: drive the PWM to neutral before cutting power once the timer is wired.
    PowerOff();
}

uint8_t Damper::Target() const
{
    return target;
}

uint8_t Damper::Actual() const
{
    return actual;
}

Damper::Mode Damper::GetMode() const
{
    return mode;
}

bool Damper::Moving() const
{
    return powered;
}

bool Damper::Stalled() const
{
    return stalled;
}

void Damper::PowerOn()
{
    powered = true;
    enable.Write(true);
    settleTimer.Start(moveSettleMs);
    stallTimer.Stop(); // no stale overcurrent window carried over from last time
}

void Damper::PowerOff()
{
    powered = false;
    enable.Write(false);
    settleTimer.Stop();
    stallTimer.Stop();
}
