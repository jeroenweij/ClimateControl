/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "Gpio.h"

using Hal::Gpio;
using Hal::Pin;

namespace
{
    void EnableGpioClock(GPIO_TypeDef* const port)
    {
        if (port == GPIOA)
        {
            __HAL_RCC_GPIOA_CLK_ENABLE();
        }
        else if (port == GPIOB)
        {
            __HAL_RCC_GPIOB_CLK_ENABLE();
        }
        else if (port == GPIOC)
        {
            __HAL_RCC_GPIOC_CLK_ENABLE();
        }
        else if (port == GPIOD)
        {
            __HAL_RCC_GPIOD_CLK_ENABLE();
        }
        else if (port == GPIOF)
        {
            __HAL_RCC_GPIOF_CLK_ENABLE();
        }
    }
} // namespace

Gpio::Gpio(const Pin pin, const Mode mode) :
    pin(pin)
{
    EnableGpioClock(pin.port);

    GPIO_InitTypeDef init = {};
    init.Pin              = pin.pin;
    init.Speed            = GPIO_SPEED_FREQ_LOW;

    switch (mode)
    {
        case Mode::Output:
            init.Mode = GPIO_MODE_OUTPUT_PP;
            init.Pull = GPIO_NOPULL;
            break;
        case Mode::Input:
            init.Mode = GPIO_MODE_INPUT;
            init.Pull = GPIO_NOPULL;
            break;
        case Mode::InputPullUp:
            init.Mode = GPIO_MODE_INPUT;
            init.Pull = GPIO_PULLUP;
            break;
    }

    HAL_GPIO_Init(pin.port, &init);
}

void Gpio::Write(const bool value)
{
    HAL_GPIO_WritePin(pin.port, pin.pin, value ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

bool Gpio::Read() const
{
    return HAL_GPIO_ReadPin(pin.port, pin.pin) == GPIO_PIN_SET;
}
