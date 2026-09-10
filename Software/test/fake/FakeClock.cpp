/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "Tick.h"

#include "FakeClock.h"

namespace
{
    uint32_t nowMs = 0;
} // namespace

namespace FakeClock
{
    void Reset()
    {
        nowMs = 0;
    }

    void Set(const uint32_t milliseconds)
    {
        nowMs = milliseconds;
    }

    void Advance(const uint32_t milliseconds)
    {
        nowMs += milliseconds;
    }

    uint32_t Now()
    {
        return nowMs;
    }
} // namespace FakeClock

namespace Hal
{
    namespace Tick
    {
        uint32_t Millis()
        {
            return nowMs;
        }

        void DelayMs(const uint32_t ms)
        {
            nowMs += ms;
        }

        void DelayUs(const uint32_t)
        {
        }
    } // namespace Tick
} // namespace Hal
