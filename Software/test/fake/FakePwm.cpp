/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "Pwm.h"

#include "FakePwm.h"

using Hal::Pwm;

namespace
{
    uint16_t pulseUs     = 0;
    bool     initialised = false;
} // namespace

namespace FakePwm
{
    void Reset()
    {
        pulseUs     = 0;
        initialised = false;
    }

    uint16_t PulseUs()
    {
        return pulseUs;
    }

    bool Initialised()
    {
        return initialised;
    }
} // namespace FakePwm

Pwm::Pwm(const PwmPins& pins, const uint16_t periodUs) :
    pins(pins),
    periodUs(periodUs)
{
}

void Pwm::Init()
{
    initialised = true;
    pulseUs     = 0;
}

void Pwm::SetPulseUs(const uint16_t newPulseUs)
{
    pulseUs = newPulseUs > periodUs ? periodUs : newPulseUs;
}
