/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

namespace Hal
{
    namespace System
    {
        // Brings up HAL_Init()/SysTick at the reset-default 16 MHz HSI clock.
        void Init();

        // Switches SYSCLK (and HCLK/PCLK) to 64 MHz: HSI16 through the PLL
        // (x8 / 2), 2 flash wait states. SystemCoreClock and the 1 ms SysTick
        // follow, so everything that derives its timing from them (UART baud,
        // Tick, Pwm) adapts; I2c's TIMINGR is written for 64 MHz. Accuracy is
        // the HSI16's own -- the PLL doesn't improve it. Call right after
        // Init(), before any peripheral is brought up. Applications only: the
        // bootloaders stay at 16 MHz, and JumpToApplication() / a reset both
        // return to it. False if the PLL didn't lock -- the clock is then left
        // at 16 MHz, which everything still works at.
        bool ClockTo64MHz();

        // Point the vector table at 'flashBase' (SCB->VTOR). An application
        // image linked above the bootloader must call this first thing in main()
        // -- see Spec/Node-Flash-Layout-and-Bootloader-Spec.md §4.1.
        void SetVectorTable(uint32_t flashBase);

        // Tear down peripherals/SysTick and hand control to the image at
        // 'flashBase' (loads its MSP from [base], jumps to [base+4]). Never
        // returns. Used by the bootloader.
        [[noreturn]] void JumpToApplication(uint32_t flashBase);

        // Warm-reset the MCU (NVIC_SystemReset). Never returns.
        [[noreturn]] void Reset();

        // RCC_CSR[31:24] reset-flag byte (OBL / PIN / BOR / SW / IWDG / WWDG /
        // LPWR). The flags are sticky until cleared, so this reads live and is
        // surfaced verbatim on Endpoint::SystemStatus for the master to decode.
        uint8_t ResetCause();
    } // namespace System
} // namespace Hal
