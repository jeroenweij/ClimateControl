/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

// Hal::Adc double: Read() returns whatever FakeAdc::SetValue() last set
// (0 -- "no current" -- until then), regardless of channel. Used by Damper's
// stall-detection Loop() in host tests.
namespace FakeAdc
{
    void Reset();
    void SetValue(const uint16_t counts);
} // namespace FakeAdc
