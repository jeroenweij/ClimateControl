/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "stm32g0xx_hal.h"

#include "Backup.h"

namespace
{
    volatile uint32_t* Register(const Hal::Backup::Reg reg)
    {
        return &TAMP->BKP0R + static_cast<uint32_t>(reg);
    }

    void EnableAccess()
    {
        static bool done = false;
        if (done)
        {
            return;
        }
        __HAL_RCC_PWR_CLK_ENABLE();
        __HAL_RCC_RTCAPB_CLK_ENABLE(); // TAMP register bus clock
        HAL_PWR_EnableBkUpAccess(); // PWR_CR1 DBP
        done = true;
    }
} // namespace

uint32_t Hal::Backup::Read(const Reg reg)
{
    EnableAccess();
    return *Register(reg);
}

void Hal::Backup::Write(const Reg reg, const uint32_t value)
{
    EnableAccess();
    *Register(reg) = value;
}
