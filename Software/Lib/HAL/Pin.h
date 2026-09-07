/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include "stm32g0xx_hal.h"

namespace Hal
{
    struct Pin
    {
        GPIO_TypeDef* port;
        uint16_t      pin;
    };
} // namespace Hal
