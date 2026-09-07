/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

namespace Hal
{
    namespace System
    {
        // Brings up HAL_Init()/SysTick at the default HSI clock. Full clock-tree
        // configuration (up to the STM32G030's 64MHz max) is not done here -- see
        // Node-Bus-Hardware-Design-Spec.md.
        void Init();
    } // namespace System
} // namespace Hal
