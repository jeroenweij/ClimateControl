/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "BoardPins.h"
#include "Logger.h"
#include "Tick.h"
#include "Watchdog.h"

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
    state(State::Off),
    positionUs(PulseFor(NeutralPercent)),
    stallStartUs(positionUs),
    lastSlewMs(0),
    moveFrom(NeutralPercent),
    sensePeak(0),
    overSinceMs(0),
    overRunning(false),
    overLongestMs(0),
    senseSum(0),
    senseSamples(0),
    settleTimer(),
    stallTimer()
{
}

void Damper::Init()
{
    pwm.Init();
    PowerOff(State::Off);
}

void Damper::Loop()
{
    if (!Moving())
    {
        return;
    }

    if (CheckStall())
    {
        return;
    }

    if (state == State::Moving)
    {
        Slew();
    }
    else if (settleTimer.Finished())
    {
        // The servo has had settleMs to follow the end of the ramp -- adopt the
        // target and drop servo power; the gearing holds it there.
        actual = target;
        PowerOff(State::Off);
    }
}

void Damper::SetTarget(const uint8_t percent)
{
    const uint8_t clamped = Clamp100(percent);
    if (clamped == target && state != State::Stalled)
    {
        return; // already there or on the way -- but after a stall, the same target is a retry
    }
    LOG_INFO("Damper target " << clamped << "%");
    target = clamped;
    StartMove();
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
    // Blocks the main loop for up to half a stroke plus the settle time --
    // keep that well inside the watchdog.
    static_assert(fullStrokeMs / 2 + settleMs + 1000 <= Hal::Watchdog::TimeoutMs, "ParkNeutral() would outlast the watchdog");

    if (state == State::Off && target == NeutralPercent && actual == NeutralPercent)
    {
        return; // already parked
    }

    LOG_WARN("Damper -> neutral, servo off");
    target = NeutralPercent;
    StartMove();
    while (Moving())
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

Damper::State Damper::GetState() const
{
    return state;
}

bool Damper::Moving() const
{
    return state == State::Moving || state == State::Settling;
}

bool Damper::Stalled() const
{
    return state == State::Stalled;
}

uint8_t Damper::ReportedMode() const
{
    return Stalled() ? StalledCode : static_cast<uint8_t>(mode);
}

void Damper::StartMove()
{
    if (!Moving())
    {
        // Signal first, on the last commanded position -- where the gearing
        // has been holding the horn -- so the servo sees no jump the moment
        // its rail comes up.
        pwm.SetPulseUs(positionUs);
        moveFrom      = actual;
        sensePeak     = 0;
        senseSum      = 0;
        senseSamples  = 0;
        overRunning   = false;
        overLongestMs = 0;
        enable.Write(true);
        stallTimer.Stop(); // no stale overcurrent window carried over from last time
    }
    settleTimer.Stop();
    lastSlewMs = Hal::Tick::Millis();
    state      = State::Moving;
}

void Damper::Slew()
{
    const uint32_t now    = Hal::Tick::Millis();
    const uint32_t frames = (now - lastSlewMs) / slewFrameMs;
    if (frames == 0)
    {
        return;
    }
    lastSlewMs += frames * slewFrameMs;

    const uint16_t goal = PulseFor(target);
    const uint32_t step = frames * slewStepUs;
    const uint32_t gap  = positionUs < goal ? goal - positionUs : positionUs - goal;
    if (positionUs < goal)
    {
        positionUs = (gap <= step) ? goal : static_cast<uint16_t>(positionUs + step);
    }
    else if (positionUs > goal)
    {
        positionUs = (gap <= step) ? goal : static_cast<uint16_t>(positionUs - step);
    }
    pwm.SetPulseUs(positionUs);

    if (positionUs == goal)
    {
        state = State::Settling;
        settleTimer.Start(settleMs);
    }
}

bool Damper::CheckStall()
{
    const uint16_t counts = currentSense.Read();
    if (counts > sensePeak)
    {
        sensePeak = counts;
    }
    senseSum += counts;
    senseSamples++;

    if (counts < stallThresholdCounts)
    {
        overRunning = false;
        stallTimer.Stop(); // current dropped back down -- not a sustained stall
        return false;
    }

    const uint32_t now = Hal::Tick::Millis();
    if (!overRunning)
    {
        overRunning = true;
        overSinceMs = now;
    }
    if (now - overSinceMs > overLongestMs)
    {
        overLongestMs = now - overSinceMs;
    }

    if (!stallTimer.IsRunning())
    {
        stallTimer.Start(stallConfirmMs);
        stallStartUs = positionUs;
        return false;
    }
    if (!stallTimer.Finished())
    {
        return false;
    }

    LOG_WARN("Damper stall detected, cutting servo power early");
    positionUs = stallStartUs; // the slew ran on past the jam during the confirm window
    PowerOff(State::Stalled);
    return true;
}

void Damper::PowerOff(const State next)
{
    const bool wasPowered = Moving();
    state                 = next;
    enable.Write(false);
    pwm.SetPulseUs(0); // signal low -- never drive an unpowered servo's input
    settleTimer.Stop();
    stallTimer.Stop();
    if (wasPowered)
    {
        LogSense();
    }
}

void Damper::LogSense() const
{
    // Fits one 32-character DiagLog line ("I: " included) for any realistic
    // reading; Tools::LogRing truncates the rare longer one.
    const uint32_t mean = senseSamples > 0 ? senseSum / senseSamples : 0;
    LOG_INFO("Srv " << moveFrom << ">" << target << " pk" << sensePeak << " av" << mean << " ov" << overLongestMs);
}

uint16_t Damper::PulseFor(const uint8_t percent)
{
    // Rounded to the nearest us.
    return static_cast<uint16_t>(closedPulseUs + ((openPulseUs - closedPulseUs) * Clamp100(percent) + 50) / 100);
}
