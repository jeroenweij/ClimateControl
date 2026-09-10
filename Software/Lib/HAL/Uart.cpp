/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "stm32g0xx_hal.h"

#include "Uart.h"

using Hal::Uart;
using Hal::UartPins;

namespace
{
    // One handle per instance -- Uart objects are cheap value types, the HAL
    // state lives here keyed by Instance.
    UART_HandleTypeDef handles[2] = {};

    USART_TypeDef* Regs(const Uart::Instance instance)
    {
        return instance == Uart::Instance::Usart2 ? USART2 : USART1;
    }

    UART_HandleTypeDef& Handle(const Uart::Instance instance)
    {
        return handles[static_cast<int>(instance)];
    }

    void ConfigureAfPin(const Hal::Pin pin, const uint8_t alternateFunction)
    {
        GPIO_InitTypeDef init = {};
        init.Pin              = pin.pin;
        init.Mode             = GPIO_MODE_AF_PP;
        init.Pull             = GPIO_NOPULL;
        init.Speed            = GPIO_SPEED_FREQ_HIGH;
        init.Alternate        = alternateFunction;

        HAL_GPIO_Init(pin.port, &init);
    }
} // namespace

// Clock + GPIO are brought up in Init() below (per instance, no shared state),
// so the HAL's MspInit hook has nothing to do.
extern "C" void HAL_UART_MspInit(UART_HandleTypeDef*)
{
}

Uart::Uart()
{
}

void Uart::Init(const uint32_t baudRate, const Instance instance, const UartPins& pins)
{
    this->instance = instance;

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    if (instance == Instance::Usart2)
    {
        __HAL_RCC_USART2_CLK_ENABLE();
    }
    else
    {
        __HAL_RCC_USART1_CLK_ENABLE();
    }

    ConfigureAfPin(pins.tx.pin, pins.tx.alternateFunction);
    ConfigureAfPin(pins.rx.pin, pins.rx.alternateFunction);
    ConfigureAfPin(pins.de.pin, pins.de.alternateFunction);

    UART_HandleTypeDef& handle         = Handle(instance);
    handle.Instance                    = Regs(instance);
    handle.Init.BaudRate               = baudRate;
    handle.Init.WordLength             = UART_WORDLENGTH_8B;
    handle.Init.StopBits               = UART_STOPBITS_1;
    handle.Init.Parity                 = UART_PARITY_NONE;
    handle.Init.Mode                   = UART_MODE_TX_RX;
    handle.Init.HwFlowCtl              = UART_HWCONTROL_NONE;
    handle.Init.OverSampling           = UART_OVERSAMPLING_16;
    handle.Init.OneBitSampling         = UART_ONE_BIT_SAMPLE_DISABLE;
    handle.Init.ClockPrescaler         = UART_PRESCALER_DIV1;
    handle.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;

    // DEAT/DEDT are in bit-periods (5-bit fields). 1 bit-period is a
    // conservative placeholder -- not yet tuned against real bus/transceiver
    // turnaround timing, see Node-Bus-Hardware-Design-Spec.md §6.
    const uint32_t assertionTime   = 1;
    const uint32_t deassertionTime = 1;
    HAL_RS485Ex_Init(&handle, UART_DE_POLARITY_HIGH, assertionTime, deassertionTime);
}

bool Uart::Available() const
{
    return __HAL_UART_GET_FLAG(&Handle(instance), UART_FLAG_RXNE);
}

uint8_t Uart::ReadByte()
{
    return static_cast<uint8_t>(Regs(instance)->RDR);
}

void Uart::WriteBytes(const uint8_t* const data, const size_t len)
{
    HAL_UART_Transmit(&Handle(instance), data, static_cast<uint16_t>(len), HAL_MAX_DELAY);
}
