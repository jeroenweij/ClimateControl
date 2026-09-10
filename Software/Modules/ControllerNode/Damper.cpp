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
    target(NeutralPercent),
    actual(NeutralPercent),
    mode(Mode::Auto),
    powered(false),
    settleTimer()
{
}

void Damper::Init()
{
    PowerOff();
}

void Damper::Loop()
{
    if (powered && settleTimer.Finished())
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
    target = clamped;
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

void Damper::PowerOn()
{
    powered = true;
    enable.Write(true);
    settleTimer.Start(moveSettleMs);
}

void Damper::PowerOff()
{
    powered = false;
    enable.Write(false);
    settleTimer.Stop();
}
