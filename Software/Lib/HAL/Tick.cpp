/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "stm32g0xx_hal.h"

#include "Tick.h"

uint32_t Hal::Tick::Millis()
{
    return HAL_GetTick();
}

void Hal::Tick::DelayMs(const uint32_t ms)
{
    HAL_Delay(ms);
}

void Hal::Tick::DelayUs(const uint32_t us)
{
    if (us == 0U)
    {
        return;
    }

    const uint32_t reload     = SysTick->LOAD + 1U; // ticks per 1 ms SysTick period
    const uint32_t ticksPerUs = SystemCoreClock / 1000000U;
    uint32_t       remaining  = us * ticksPerUs;
    uint32_t       prev       = SysTick->VAL;

    while (remaining != 0U)
    {
        const uint32_t now = SysTick->VAL;
        // SysTick counts down and wraps from 0 back to 'reload - 1'.
        const uint32_t elapsed = (now <= prev) ? (prev - now) : (prev + reload - now);

        if (elapsed >= remaining)
        {
            break;
        }
        remaining -= elapsed;
        prev = now;
    }
}
