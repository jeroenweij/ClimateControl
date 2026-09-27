/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "stm32g0xx_hal.h"

#include "Backup.h"
#include "Tick.h"

#include "Watchdog.h"

namespace
{
    constexpr uint32_t KeyStart  = 0xCCCC;
    constexpr uint32_t KeyUnlock = 0x5555;
    constexpr uint32_t KeyReload = 0xAAAA;

    // LSI 32 kHz / 64 (PR = 4) = 500 Hz, 2 ms per count; RLR is 12 bits.
    constexpr uint32_t PrescalerDiv64 = 4;
    constexpr uint32_t CountsPerMs    = 2;
    constexpr uint32_t Reload         = Hal::Watchdog::TimeoutMs / CountsPerMs;
    static_assert(Reload <= 0xFFF, "IWDG reload is 12 bits");
} // namespace

void Hal::Watchdog::Start()
{
    __HAL_RCC_DBGMCU_CLK_ENABLE();
    DBG->APBFZ1 |= DBG_APB_FZ1_DBG_IWDG_STOP;

    Feed(); // AliveUptime must not hold a previous boot's stamp
    IWDG->KR  = KeyStart; // also starts the LSI
    IWDG->KR  = KeyUnlock;
    IWDG->PR  = PrescalerDiv64;
    IWDG->RLR = Reload;
    while (IWDG->SR != 0U)
    {
    }
    IWDG->KR = KeyReload;
}

void Hal::Watchdog::Feed()
{
    IWDG->KR = KeyReload;
    Hal::Backup::Write(Hal::Backup::Reg::AliveUptime, Hal::Tick::Millis());
}
