/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

// Hal::Pwm double: records the last SetPulseUs() (clamped to the period, as
// the real one is) so Damper's servo drive can be checked in host tests.
namespace FakePwm
{
    void     Reset();
    uint16_t PulseUs();
    bool     Initialised();
} // namespace FakePwm
