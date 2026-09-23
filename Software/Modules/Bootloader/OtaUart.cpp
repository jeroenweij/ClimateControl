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

    // RX ring buffer, ISR-fed -- replaces the old poll-only Available()/Read()
    // (which had to catch every byte between one FirmwareSlave::Loop() call
    // and the next, e.g. across a multi-page flash erase/program stretch,
    // with nothing deeper than the single-byte hardware RDR to hold it).
    // Sized comfortably over one NodeLib frame (2 sync + 1 len + 3 header +
    // 32 data + 2 crc = 40 bytes, Id.h::MAX_DATA), matching the sizing logic
    // Hal::Uart's own RX ring buffer uses. One shared buffer, not
    // Hal::Uart's per-instance array, is enough: a provisioned node's
    // bootloader only ever brings up one bus (module decides Usart1 vs
    // Usart2 in Init()), so only one of USART1_IRQHandler/USART2_IRQHandler
    // below ever actually fires.
    constexpr size_t rxBufferSize = 64;
    struct RxRing
    {
        uint8_t         buffer[rxBufferSize];
        volatile size_t head;
        volatile size_t tail;
    };
    RxRing rxRing;

    // Common to both ISRs -- clears ORE/FE/NE the same way Available() used
    // to (see its old comment, still true: unclearred, one glitch deafens the
    // receiver until a power-on reset) and empties RDR into the ring buffer.
    void ServiceRxIrq(USART_TypeDef* const usart)
    {
        if ((usart->ISR & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE)) != 0)
        {
            usart->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF;
        }

        if ((usart->ISR & USART_ISR_RXNE_RXFNE) != 0)
        {
            const uint8_t byte = static_cast<uint8_t>(usart->RDR); // clears RXNE
            const size_t  next = (rxRing.head + 1) % rxBufferSize;
            if (next != rxRing.tail)
            {
                rxRing.buffer[rxRing.head] = byte;
                rxRing.head                = next;
            }
            // else: ring buffer full -- drop, same backstop as Hal::Uart's.
        }
    }
} // namespace

// startup_stm32g031xx.s leaves these weakly aliased to Default_Handler (an
// infinite-loop trap) -- without real definitions, enabling either NVIC line
// would hang the CPU on the first byte, same pitfall Hal::Uart.cpp notes.
extern "C" void USART1_IRQHandler()
{
    ServiceRxIrq(USART1);
}

extern "C" void USART2_IRQHandler()
{
    ServiceRxIrq(USART2);
}

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

    rxRing = RxRing{};

    USART_TypeDef* const usart = Regs(bus);
    usart->CR1                 = 0;
    usart->BRR                 = (SystemCoreClock + baudRate / 2u) / baudRate;
    usart->CR3                 = USART_CR3_DEM; // hardware driver-enable
    usart->CR1                 = USART_CR1_DEAT_0 | USART_CR1_DEDT_0 // 1 sample-time assert/deassert
        | USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE_RXFNEIE | USART_CR1_UE;

    // Highest priority (STM32G0's 2 priority bits -> 0..3), same reasoning as
    // Hal::Uart.cpp: a byte at bus baud is tens of microseconds, tighter than
    // anything else running here.
    const IRQn_Type irqn = (bus == Bus::Usart2) ? USART2_IRQn : USART1_IRQn;
    NVIC_SetPriority(irqn, 0);
    NVIC_EnableIRQ(irqn);
}

bool OtaUart::Available() const
{
    return rxRing.head != rxRing.tail;
}

uint8_t OtaUart::Read()
{
    const uint8_t byte = rxRing.buffer[rxRing.tail];
    rxRing.tail        = (rxRing.tail + 1) % rxBufferSize;
    return byte;
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
