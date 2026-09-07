/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

namespace Hal
{
    namespace Tick
    {
        uint32_t Millis();
        void     DelayMs(const uint32_t ms);
    } // namespace Tick
} // namespace Hal
