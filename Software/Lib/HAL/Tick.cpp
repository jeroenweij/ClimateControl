/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "stm32g0xx_hal.h"

#include "Tick.h"

// startup_stm32g031xx.s leaves SysTick_Handler weakly aliased to
// Default_Handler (an infinite-loop trap) -- without this, the first SysTick
// interrupt HAL_InitTick() arms (in HAL_Init(), via System::Init()) freezes
// the CPU the moment it fires, hanging any HAL_Delay()/DelayMs() caller.
extern "C" void SysTick_Handler()
{
    HAL_IncTick();
}

uint32_t Hal::Tick::Millis()
{
    return HAL_GetTick();
}

void Hal::Tick::DelayMs(const uint32_t ms)
{
    HAL_Delay(ms);
}

uint32_t Hal::Tick::Micros()
{
    uint32_t ms;
    uint32_t val;
    uint32_t ms2;

    do
    {
        ms  = HAL_GetTick();
        val = SysTick->VAL;
        ms2 = HAL_GetTick();
    } while (ms != ms2); // retry if the 1 ms tick rolled over mid-read

    const uint32_t reload        = SysTick->LOAD + 1U;
    const uint32_t ticksPerUs    = SystemCoreClock / 1000000U;
    const uint32_t elapsedInTick = (reload - 1U - val) / ticksPerUs; // counts down from reload-1

    return ms * 1000U + elapsedInTick;
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
