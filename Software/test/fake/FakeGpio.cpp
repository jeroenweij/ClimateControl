/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "stm32g0xx_hal.h"

#include "Gpio.h"

using Hal::Gpio;
using Hal::Pin;

namespace
{
    GPIO_TypeDef portA;
    GPIO_TypeDef portB;
    GPIO_TypeDef portC;
    GPIO_TypeDef portD;
    GPIO_TypeDef portF;
} // namespace

GPIO_TypeDef* const GPIOA = &portA;
GPIO_TypeDef* const GPIOB = &portB;
GPIO_TypeDef* const GPIOC = &portC;
GPIO_TypeDef* const GPIOD = &portD;
GPIO_TypeDef* const GPIOF = &portF;

Gpio::Gpio(const Pin pin, const Mode) :
    pin(pin)
{
}

void Gpio::Write(const bool value)
{
    if (pin.port == nullptr)
    {
        return;
    }
    // A driven push-pull output reads back its own level on real hardware
    // (IDR follows ODR), which Read()-then-toggle code such as the
    // bootloaders' heartbeat LEDs relies on.
    if (value)
    {
        pin.port->ODR |= pin.pin;
        pin.port->IDR |= pin.pin;
    }
    else
    {
        pin.port->ODR &= ~static_cast<uint32_t>(pin.pin);
        pin.port->IDR &= ~static_cast<uint32_t>(pin.pin);
    }
}

bool Gpio::Read() const
{
    if (pin.port == nullptr)
    {
        return false;
    }
    return (pin.port->IDR & pin.pin) != 0u;
}
