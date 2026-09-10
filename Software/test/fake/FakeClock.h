/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

// Test-controlled stand-in for the 1 ms SysTick behind Hal::Tick. Time only
// moves when a test advances it (or calls Hal::Tick::DelayMs), so DelayTimer
// behaviour is fully deterministic.
namespace FakeClock
{
    void     Reset();
    void     Set(uint32_t milliseconds);
    void     Advance(uint32_t milliseconds);
    uint32_t Now();
} // namespace FakeClock
