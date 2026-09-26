/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

#include "Adc.h"
#include "DelayTimer.h"
#include "Gpio.h"
#include "Pwm.h"

// The room damper actuator: a metal-geared servo on a MCU-gated 5 V rail
// (Node-Bus-Power-Path-Spec.md §3.1). The servo is UNPOWERED at rest -- the
// gearing holds position -- and energised (Board::ServoEnable) only for the
// brief move to a new setpoint.
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
// Stall detection (Node-Bus-Power-Path-Spec.md §3.1.1): the servo has no
// position feedback, so moveSettleMs alone can only bound how long a jam is
// driven, not detect one. Loop() samples Board::ServoCurrentSense (ADC_IN0,
// the INA180A1/shunt circuit) while powered, and de-energises early -- well
// before moveSettleMs -- once current has stayed above stallThresholdCounts
// for stallConfirmMs. That early cutoff is the point: it is what keeps the
// AO3401A load-switch's worst-case stall exposure to a detection window
// instead of the full settle time.
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

    static constexpr uint8_t NeutralPercent = 50;

    Damper();

    void Init();
    void Loop();

    // Commanded position. Enables the servo, moves, then powers it back down
    // once the move settle time has elapsed.
    void SetTarget(const uint8_t percent);
    void SetMode(const Mode mode);

    uint8_t Target() const;
    uint8_t Actual() const;
    Mode    GetMode() const;
    bool    Moving() const;

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
    // to completion (moveSettleMs, or earlier on a stall) because the caller
    // resets straight after. Returns at once if already parked and unpowered.
    void ParkNeutral();

  private:
    // Time the servo is held powered after a commanded move.
    static const uint32_t moveSettleMs = 1500;

    // Servo frame and pulse range (DS3225: 50 Hz, 500..2500 us over 180 deg).
    static const uint16_t pwmPeriodUs   = 20000;
    static const uint16_t closedPulseUs = 500; // 0 %
    static const uint16_t openPulseUs   = 2500; // 100 %

    // Sustained-overcurrent window before a stall is declared. Long enough to
    // ride out the start-of-move current step the switched-side reservoir cap
    // already softens (Node-Bus-Power-Path-Spec.md §3.1); short next to
    // moveSettleMs is the whole point (see the class comment above).
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

    void PowerOn();
    void PowerOff();

    static uint16_t PulseFor(const uint8_t percent);

    Hal::Gpio         enable;
    Hal::Adc          currentSense;
    Hal::Pwm          pwm;
    uint8_t           target;
    uint8_t           actual;
    Mode              mode;
    bool              powered;
    bool              stalled;
    Tools::DelayTimer settleTimer;
    Tools::DelayTimer stallTimer;
};
