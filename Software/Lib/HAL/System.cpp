/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "stm32g0xx_hal.h"

#include "System.h"

void Hal::System::Init()
{
    HAL_Init();
}

bool Hal::System::ClockTo64MHz()
{
    // The regulator resets into Range 1, which 64 MHz needs -- nothing to
    // change there. VCO = 16 MHz / M(1) * N(8) = 128 MHz (in its 64..344 MHz
    // range); PLLR = VCO / 2 = 64 MHz drives SYSCLK. PLLP is unused but must
    // hold a valid divider.
    RCC_OscInitTypeDef osc  = {};
    osc.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
    osc.HSIState            = RCC_HSI_ON;
    osc.HSIDiv              = RCC_HSI_DIV1;
    osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    osc.PLL.PLLState        = RCC_PLL_ON;
    osc.PLL.PLLSource       = RCC_PLLSOURCE_HSI;
    osc.PLL.PLLM            = RCC_PLLM_DIV1;
    osc.PLL.PLLN            = 8;
    osc.PLL.PLLP            = RCC_PLLP_DIV2;
    osc.PLL.PLLR            = RCC_PLLR_DIV2;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK)
    {
        return false;
    }

    // RM0444 Table 7: 2 wait states from 48 MHz up in Range 1. The HAL raises
    // the latency before switching SYSCLK up, then updates SystemCoreClock and
    // re-arms SysTick for the new clock.
    RCC_ClkInitTypeDef clk = {};
    clk.ClockType          = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_PCLK1;
    clk.SYSCLKSource       = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider      = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider     = RCC_HCLK_DIV1;
    return HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2) == HAL_OK;
}

void Hal::System::SetVectorTable(const uint32_t flashBase)
{
    SCB->VTOR = flashBase;
    __DSB();
}

void Hal::System::Reset()
{
    NVIC_SystemReset();
    while (true)
    {
    }
}

uint8_t Hal::System::ResetCause()
{
    return static_cast<uint8_t>(RCC->CSR >> 24U);
}

void Hal::System::JumpToApplication(const uint32_t flashBase)
{
    const uint32_t stackPointer = *reinterpret_cast<const volatile uint32_t*>(flashBase);
    const uint32_t resetVector  = *reinterpret_cast<const volatile uint32_t*>(flashBase + 4U);

    __disable_irq();

    // Stop SysTick and clear anything it left pending.
    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL  = 0U;
    SCB->ICSR     = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;

    HAL_RCC_DeInit();
    HAL_DeInit();

    SetVectorTable(flashBase);
    __set_MSP(stackPointer);
    __enable_irq();

    reinterpret_cast<void (*)()>(resetVector)();

    while (true)
    {
    }
}
