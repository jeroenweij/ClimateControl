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
