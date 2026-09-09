/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

namespace Hal
{
    namespace System
    {
        // Brings up HAL_Init()/SysTick at the default HSI clock. Full clock-tree
        // configuration (up to the STM32G031's 64MHz max) is not done here -- see
        // Node-Bus-Hardware-Design-Spec.md.
        void Init();

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
