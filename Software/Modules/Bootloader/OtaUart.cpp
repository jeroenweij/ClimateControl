/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "stm32g0xx_hal.h"

#include "ConfigStore.h"

#include "OtaUart.h"

using Boot::OtaUart;

namespace
{
    constexpr uint32_t ModeAf = 2u; // GPIO MODER: alternate function

    void SetModer(GPIO_TypeDef* const port, const uint32_t pin, const uint32_t mode)
    {
        port->MODER = (port->MODER & ~(3u << (pin * 2u))) | (mode << (pin * 2u));
    }

    void SetAf(GPIO_TypeDef* const port, const uint32_t pin, const uint32_t af)
    {
        // AFR[0] covers pins 0..7, AFR[1] pins 8..15.
        const uint32_t idx   = pin >> 3u;
        const uint32_t shift = (pin & 7u) * 4u;
        port->AFR[idx]       = (port->AFR[idx] & ~(0xFu << shift)) | (af << shift);
    }

    USART_TypeDef* Regs(const OtaUart::Bus bus)
    {
        return bus == OtaUart::Bus::Usart2 ? USART2 : USART1;
    }
} // namespace

void OtaUart::Init(const uint32_t baudRate, const uint8_t module)
{
    bus = (module == static_cast<uint8_t>(NodeLib::ConfigStore::Module::Thermostat)) ? Bus::Usart2
                                                                                     : Bus::Usart1;

    RCC->IOPENR |= RCC_IOPENR_GPIOAEN | RCC_IOPENR_GPIOBEN;

    if (bus == Bus::Usart2)
    {
        RCC->APBENR1 |= RCC_APBENR1_USART2EN;
        // PA2 = USART2_TX, PA3 = USART2_RX, PA1 = USART2_DE -- all AF1.
        SetModer(GPIOA, 1, ModeAf);
        SetModer(GPIOA, 2, ModeAf);
        SetModer(GPIOA, 3, ModeAf);
        SetAf(GPIOA, 1, 1u);
        SetAf(GPIOA, 2, 1u);
        SetAf(GPIOA, 3, 1u);
    }
    else
    {
        RCC->APBENR2 |= RCC_APBENR2_USART1EN;
        // PB6 = USART1_TX, PB7 = USART1_RX -- AF0 (the AFR reset value).
        SetModer(GPIOB, 6, ModeAf);
        SetModer(GPIOB, 7, ModeAf);
        // PA12 = USART1_DE -- AF1.
        SetModer(GPIOA, 12, ModeAf);
        SetAf(GPIOA, 12, 1u);
    }

    USART_TypeDef* const usart = Regs(bus);
    usart->CR1                 = 0;
    usart->BRR                 = (SystemCoreClock + baudRate / 2u) / baudRate;
    usart->CR3                 = USART_CR3_DEM; // hardware driver-enable
    usart->CR1                 = USART_CR1_DEAT_0 | USART_CR1_DEDT_0 // 1 sample-time assert/deassert
        | USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
}

bool OtaUart::Available() const
{
    return (Regs(bus)->ISR & USART_ISR_RXNE_RXFNE) != 0;
}

uint8_t OtaUart::Read()
{
    return static_cast<uint8_t>(Regs(bus)->RDR);
}

void OtaUart::Write(const uint8_t* const data, const size_t len)
{
    USART_TypeDef* const usart = Regs(bus);
    for (size_t i = 0; i < len; i++)
    {
        while ((usart->ISR & USART_ISR_TXE_TXFNF) == 0)
        {
        }
        usart->TDR = data[i];
    }
    while ((usart->ISR & USART_ISR_TC) == 0)
    {
    }
}
