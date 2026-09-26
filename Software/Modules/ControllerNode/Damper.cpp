/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "BoardPins.h"
#include "Logger.h"
#include "Tick.h"

#include "Damper.h"

namespace
{
    // How often ParkNeutral()'s blocking move re-runs Loop() -- stall checks
    // stay well inside stallConfirmMs.
    constexpr uint32_t parkPollMs = 5;

    uint8_t Clamp100(const uint8_t percent)
    {
        return percent > 100 ? 100 : percent;
    }
} // namespace

Damper::Damper() :
    enable(Board::ServoEnable, Hal::Gpio::Mode::Output),
    currentSense(Board::ServoCurrentSenseChannel),
    pwm({Board::ServoPwm, Board::ServoPwmAf}, pwmPeriodUs),
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
    pwm.Init();
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
    if (!powered && target == NeutralPercent && actual == NeutralPercent)
    {
        return; // already parked
    }

    LOG_WARN("Damper -> neutral, servo off");
    target  = NeutralPercent;
    stalled = false;
    PowerOn();
    while (powered)
    {
        Loop(); // powers off once settled, or early on a stall
        Hal::Tick::DelayMs(parkPollMs);
    }
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

uint8_t Damper::ReportedMode() const
{
    return stalled ? StalledCode : static_cast<uint8_t>(mode);
}

void Damper::PowerOn()
{
    // Signal first, so the servo sees the target pulse the moment its rail
    // comes up rather than whatever it last held.
    pwm.SetPulseUs(PulseFor(target));
    powered = true;
    enable.Write(true);
    settleTimer.Start(moveSettleMs);
    stallTimer.Stop(); // no stale overcurrent window carried over from last time
}

void Damper::PowerOff()
{
    powered = false;
    enable.Write(false);
    pwm.SetPulseUs(0); // signal low -- never drive an unpowered servo's input
    settleTimer.Stop();
    stallTimer.Stop();
}

uint16_t Damper::PulseFor(const uint8_t percent)
{
    // Rounded to the nearest us.
    return static_cast<uint16_t>(closedPulseUs + ((openPulseUs - closedPulseUs) * Clamp100(percent) + 50) / 100);
}
