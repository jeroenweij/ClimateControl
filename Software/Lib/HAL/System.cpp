/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "stm32g0xx_hal.h"

#include "System.h"

void Hal::System::Init()
{
    HAL_Init();
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
