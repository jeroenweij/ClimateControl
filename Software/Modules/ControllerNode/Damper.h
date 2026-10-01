/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

#include "Adc.h"
#include "DelayTimer.h"
#include "Gpio.h"
#include "Pwm.h"
#include "SStream.h"

// The room damper actuator: a metal-geared servo on a MCU-gated 5 V rail
// (Node-Bus-Power-Path-Spec.md §3.1). The servo is UNPOWERED at rest -- the
// gearing holds position -- and energised (Board::ServoEnable) only for the
// move to a new setpoint.
//
// Position is a standard 50 Hz servo pulse on TIM3_CH1 (Board::ServoPwm),
// mapped linearly from percent open (0..100) onto the DS3225's full
// 500..2500 us, i.e. 0..180 deg. No per-unit trim or software end stops: the
// linkage gears that travel down to under 90 deg of damper blade, and the
// mechanics alone set minimum ventilation and maximum opening
// (Node-Bus-Hardware-Design-Spec.md §4). The pulse is only driven while the
// servo rail is on; with the rail off the signal is held low so it never
// back-feeds an unpowered servo.
//
// Moves are slewed, not stepped: the DS3225 drives at full speed (~0.4 s for
// 180 deg) toward whatever pulse it is given, so Loop() walks the pulse from
// the last commanded position to the target at a fixed rate (fullStrokeMs for
// 0..100 %), one servo frame at a time. That keeps the start-of-move current
// step, linkage shock and noise down. The last commanded pulse is kept across
// power-off as the estimate of where the gearing is holding the horn, and a
// move re-energises the servo on that pulse, so power-up itself never jumps.
// The one exception is the first move after boot: the position is unknown and
// assumed NeutralPercent, so that move starts with an uncontrolled jump from
// wherever the damper really is.
//
// State: Off (unpowered, holding at Actual()) -> Moving (powered, slewing) ->
// Settling (pulse at the target, holding power for settleMs while the servo
// catches up) -> Off. Stalled is Off after a sustained overcurrent, kept until
// the next SetTarget().
//
// Stall detection (Node-Bus-Power-Path-Spec.md §3.1.1): the servo has no
// position feedback, so the move timing alone can only bound how long a jam is
// driven, not detect one. Loop() samples Board::ServoCurrentSense (ADC_IN0,
// the INA180A1/shunt circuit) while powered, and de-energises early -- well
// before the move would end -- once current has stayed above
// stallThresholdCounts for stallConfirmMs. That early cutoff is the point: it
// is what keeps the AO3401A load-switch's worst-case stall exposure to a
// detection window instead of the whole move. On a stall the commanded
// position falls back to where it was when the overcurrent began -- the best
// estimate of the jam point -- so a retry does not first snap the horn back
// into the jam at full speed.
class Damper
{
  public:
    enum class Mode : uint8_t
    {
        Closed = 0,
        Open   = 1,
        Auto   = 2,
        Manual = 3,
    };

    enum class State : uint8_t
    {
        Off, // unpowered, gearing holding at Actual()
        Moving, // powered, pulse slewing toward Target()
        Settling, // powered, pulse at Target(), servo catching up
        Stalled, // unpowered after a sustained overcurrent
    };

    static constexpr uint8_t NeutralPercent = 50;

    Damper();

    void Init();
    void Loop();

    // Commanded position. Enables the servo, slews to the target, then powers
    // it back down once settled. A new target mid-move re-aims the running
    // slew without a power cycle.
    void SetTarget(const uint8_t percent);
    void SetMode(const Mode mode);

    uint8_t Target() const;
    uint8_t Actual() const;
    Mode    GetMode() const;
    State   GetState() const;

    // Servo powered: Moving or Settling.
    bool Moving() const;

    // True from the moment a sustained overcurrent is detected (see Loop())
    // until the next SetTarget() gives the servo a fresh attempt.
    bool Stalled() const;

    // Wire value for the DamperMode endpoint (Node-Message-Model-Spec.md §3):
    // GetMode()'s coding, except while Stalled() -- reported as StalledCode
    // instead, so the fault is visible on the Thermostat display and the main
    // bus without a separate endpoint. RO-only: SetMode() never accepts
    // StalledCode, it is not a commandable mode.
    uint8_t ReportedMode() const;

    // Drive to NeutralPercent and cut servo power -- for PrepareForReset()
    // (ControllerNode-Thermostat-Link-Spec.md §5.1). Blocking: runs the move
    // to completion (at most half a stroke plus settleMs, or earlier on a
    // stall) because the caller resets straight after. Returns at once if
    // already parked and unpowered.
    void ParkNeutral();

  private:
    // Servo frame and pulse range (DS3225: 50 Hz, 500..2500 us over 180 deg).
    static const uint16_t pwmPeriodUs   = 20000;
    static const uint16_t closedPulseUs = 500; // 0 %
    static const uint16_t openPulseUs   = 2500; // 100 %

    // Slew rate: a full 0..100 % stroke takes this long (~10x the servo's own
    // speed), advanced one servo frame at a time. Bounded by the watchdog --
    // ParkNeutral() blocks for up to half a stroke (static_assert in
    // Damper.cpp).
    static const uint32_t fullStrokeMs = 4000;
    static const uint32_t slewFrameMs  = pwmPeriodUs / 1000;
    static const uint16_t slewStepUs   = (openPulseUs - closedPulseUs) * slewFrameMs / fullStrokeMs;

    // Power held after the pulse reaches the target, for the servo to finish
    // following the ramp before the rail drops.
    static const uint32_t settleMs = 300;

    // Sustained-overcurrent window before a stall is declared. Long enough to
    // ride out the start-of-move current step the switched-side reservoir cap
    // already softens (Node-Bus-Power-Path-Spec.md §3.1); short next to a
    // move is the whole point (see the class comment above).
    static const uint32_t stallConfirmMs = 200;

    // Raw 12-bit ADC code, not a calibrated current -- this is a threshold
    // detector, not an ammeter. Derivation (§3.1.1): ~2.6-2.8A stall gives
    // OUT ~= 0.67V -> ~831 counts at VDDA ~= 3.3V; ~0.3-0.8A running gives
    // ~0.07-0.19V -> ~90-240 counts. This sits comfortably between the two.
    static const uint16_t stallThresholdCounts = 500;

    // DamperMode wire code for a stall (Node-Message-Model-Spec.md §3) --
    // one past Mode's highest real value (Manual = 3), so it can never
    // collide with a commandable mode.
    static const uint8_t StalledCode = 4;

    void StartMove();
    void Slew();
    bool CheckStall();
    void PowerOff(const State next);

    static uint16_t PulseFor(const uint8_t percent);

    Hal::Gpio         enable;
    Hal::Adc          currentSense;
    Hal::Pwm          pwm;
    uint8_t           target;
    uint8_t           actual;
    Mode              mode;
    State             state;
    uint16_t          positionUs; // last commanded pulse, kept while unpowered
    uint16_t          stallStartUs; // positionUs when the overcurrent began
    uint32_t          lastSlewMs;
    Tools::DelayTimer settleTimer;
    Tools::DelayTimer stallTimer;
};

inline std::stringstream& operator<<(std::stringstream& oStrStream, const Damper::State state)
{
    switch (state)
    {
        case Damper::State::Off:
            oStrStream << "Off";
            break;
        case Damper::State::Moving:
            oStrStream << "Moving";
            break;
        case Damper::State::Settling:
            oStrStream << "Settling";
            break;
        case Damper::State::Stalled:
            oStrStream << "Stalled";
            break;
    }
    return oStrStream;
}
