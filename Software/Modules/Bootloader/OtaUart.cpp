/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "stm32g0xx_hal.h"

#include "OtaUart.h"

using Boot::OtaUart;

namespace
{
    constexpr uint32_t ModeAf = 2u; // GPIO MODER: alternate function

    void SetModer(GPIO_TypeDef* const port, const uint32_t pin, const uint32_t mode)
    {
        port->MODER = (port->MODER & ~(3u << (pin * 2u))) | (mode << (pin * 2u));
    }

    void SetAfHigh(GPIO_TypeDef* const port, const uint32_t pin, const uint32_t af)
    {
        const uint32_t shift = (pin - 8u) * 4u;
        port->AFR[1]         = (port->AFR[1] & ~(0xFu << shift)) | (af << shift);
    }
} // namespace

void OtaUart::Init(const uint32_t baudRate)
{
    RCC->IOPENR |= RCC_IOPENR_GPIOAEN | RCC_IOPENR_GPIOBEN;
    RCC->APBENR2 |= RCC_APBENR2_USART1EN;

    // PB6 = USART1_TX, PB7 = USART1_RX -- both AF0 (the AFR reset value).
    SetModer(GPIOB, 6, ModeAf);
    SetModer(GPIOB, 7, ModeAf);
    // PA12 = USART1_DE -- AF1.
    SetModer(GPIOA, 12, ModeAf);
    SetAfHigh(GPIOA, 12, 1u);

    USART1->CR1 = 0;
    USART1->BRR = (SystemCoreClock + baudRate / 2u) / baudRate;
    USART1->CR3 = USART_CR3_DEM; // hardware driver-enable on PA12
    USART1->CR1 = USART_CR1_DEAT_0 | USART_CR1_DEDT_0 // 1 sample-time assert/deassert
        | USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
}

bool OtaUart::Available() const
{
    return (USART1->ISR & USART_ISR_RXNE_RXFNE) != 0;
}

uint8_t OtaUart::Read()
{
    return static_cast<uint8_t>(USART1->RDR);
}

void OtaUart::Write(const uint8_t* const data, const size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        while ((USART1->ISR & USART_ISR_TXE_TXFNF) == 0)
        {
        }
        USART1->TDR = data[i];
    }
    while ((USART1->ISR & USART_ISR_TC) == 0)
    {
    }
}
