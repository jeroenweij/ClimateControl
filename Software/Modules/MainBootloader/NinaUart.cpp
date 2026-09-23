/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "stm32g0xx_hal.h"

#include "NinaUart.h"

using Boot::NinaUart;

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

    // RX ring buffer, ISR-fed -- see NinaUart.h's class comment for why a
    // poll-only receiver isn't enough. Sized comfortably over one NodeLib
    // frame (2 sync + 1 len + 3 header + 32 data + 2 crc = 40 bytes).
    constexpr size_t rxBufferSize = 64;
    struct RxRing
    {
        uint8_t         buffer[rxBufferSize];
        volatile size_t head;
        volatile size_t tail;
    };
    RxRing rxRing;
} // namespace

// startup_stm32g031xx.s leaves this weakly aliased to Default_Handler (an
// infinite-loop trap) -- without a real definition, enabling the NVIC line
// would hang the CPU on the first byte, same pitfall Hal::Uart.cpp notes.
extern "C" void USART2_IRQHandler()
{
    // Clears ORE/FE/NE the same way Bootloader/OtaUart.cpp's ISR does --
    // unclearred, one glitch deafens the receiver until a power-on reset.
    if ((USART2->ISR & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE)) != 0)
    {
        USART2->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF;
    }

    if ((USART2->ISR & USART_ISR_RXNE_RXFNE) != 0)
    {
        const uint8_t byte = static_cast<uint8_t>(USART2->RDR); // clears RXNE
        const size_t  next = (rxRing.head + 1) % rxBufferSize;
        if (next != rxRing.tail)
        {
            rxRing.buffer[rxRing.head] = byte;
            rxRing.head                = next;
        }
        // else: ring buffer full -- drop, same backstop as Hal::Uart's.
    }
}

void NinaUart::Init(const uint32_t baudRate)
{
    RCC->IOPENR |= RCC_IOPENR_GPIOAEN;
    RCC->APBENR1 |= RCC_APBENR1_USART2EN;

    // PA0 = USART2_CTS, PA1 = USART2_RTS, PA2 = USART2_TX, PA3 = USART2_RX --
    // all AF1 (Lib/Board/BoardPins.h's NinaCts/NinaRts/Usart2Tx/Usart2Rx).
    SetModer(GPIOA, 0, ModeAf);
    SetModer(GPIOA, 1, ModeAf);
    SetModer(GPIOA, 2, ModeAf);
    SetModer(GPIOA, 3, ModeAf);
    SetAf(GPIOA, 0, 1u);
    SetAf(GPIOA, 1, 1u);
    SetAf(GPIOA, 2, 1u);
    SetAf(GPIOA, 3, 1u);

    rxRing = RxRing{};

    USART2->CR1 = 0;
    USART2->BRR = (SystemCoreClock + baudRate / 2u) / baudRate;
    // Hardware 4-wire flow control -- NOT USART_CR3_DEM (RS485 driver-
    // enable): this is a point-to-point link to the NINA module, which
    // drives its own RTS/CTS in u-connectXpress's default 4-wire mode
    // (MainController-Server-Link-Spec.md §3). RTSE holds the MCU's own
    // nRTS (PA1) low only while room remains in the RX ring's hardware
    // stand-in; CTSE stalls TX (WriteBytes()'s TXE wait below) while NINA's
    // nRTS (wired to our CTS, PA0) says it isn't ready.
    USART2->CR3 = USART_CR3_RTSE | USART_CR3_CTSE;
    USART2->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE_RXFNEIE | USART_CR1_UE;

    // Highest priority (STM32G0's 2 priority bits -> 0..3), same reasoning as
    // Hal::Uart.cpp: a byte at 115200 baud is tens of microseconds, tighter
    // than anything else running here.
    NVIC_SetPriority(USART2_IRQn, 0);
    NVIC_EnableIRQ(USART2_IRQn);
}

bool NinaUart::Available() const
{
    return rxRing.head != rxRing.tail;
}

uint8_t NinaUart::ReadByte()
{
    const uint8_t byte = rxRing.buffer[rxRing.tail];
    rxRing.tail        = (rxRing.tail + 1) % rxBufferSize;
    return byte;
}

void NinaUart::WriteBytes(const uint8_t* const data, const size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        while ((USART2->ISR & USART_ISR_TXE_TXFNF) == 0)
        {
        }
        USART2->TDR = data[i];
    }
    while ((USART2->ISR & USART_ISR_TC) == 0)
    {
    }
}
