/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

#include "DelayTimer.h"
#include "Gpio.h"

// The room damper actuator: a metal-geared servo on a MCU-gated 5 V rail
// (Node-Bus-Power-Path-Spec.md §3.1). The servo is UNPOWERED at rest -- the
// gearing holds position -- and energised (Board::ServoEnable) only for the
// brief move to a new setpoint.
//
// PWM generation on TIM3_CH1 (Board::ServoPwm) is not wired yet: SetPercent()
// stores the target and drives the enable line; the actual timer output is a
// TODO once a timer HAL exists. Value units are percent open (0..100).
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

    // Park at NeutralPercent and cut servo power -- for PrepareForReset() and
    // any loss of the room control input (ControllerNode-Thermostat-Link-Spec.md
    // §5.1).
    void ParkNeutral();

  private:
    // Time the servo is held powered after a commanded move.
    static const uint32_t moveSettleMs = 1500;

    void PowerOn();
    void PowerOff();

    Hal::Gpio         enable;
    uint8_t           target;
    uint8_t           actual;
    Mode              mode;
    bool              powered;
    Tools::DelayTimer settleTimer;
};
