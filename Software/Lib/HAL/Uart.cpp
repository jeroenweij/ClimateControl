/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "stm32g0xx_hal.h"

#include "Uart.h"

using Hal::Uart;
using Hal::UartPins;

namespace
{
    UART_HandleTypeDef handle      = {};
    UartPins           pendingPins = {};

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

extern "C" void HAL_UART_MspInit(UART_HandleTypeDef*)
{
    __HAL_RCC_USART1_CLK_ENABLE();

    ConfigureAfPin(pendingPins.tx, pendingPins.alternateFunction);
    ConfigureAfPin(pendingPins.rx, pendingPins.alternateFunction);
    ConfigureAfPin(pendingPins.de, pendingPins.alternateFunction);
}

Uart::Uart()
{
}

void Uart::Init(const uint32_t baudRate, const UartPins& pins)
{
    pendingPins = pins;

    handle.Instance                    = USART1;
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

    // DEAT/DEDT are in bit-periods (DEAT/DEDT fields, 5 bits each). 1 bit-period is
    // a conservative placeholder -- not yet tuned against real bus/transceiver
    // turnaround timing, see Node-Bus-Hardware-Design-Spec.md Sec6.
    const uint32_t assertionTime   = 1;
    const uint32_t deassertionTime = 1;
    HAL_RS485Ex_Init(&handle, UART_DE_POLARITY_HIGH, assertionTime, deassertionTime);
}

bool Uart::Available() const
{
    return __HAL_UART_GET_FLAG(&handle, UART_FLAG_RXNE);
}

uint8_t Uart::ReadByte()
{
    return static_cast<uint8_t>(handle.Instance->RDR);
}

void Uart::WriteBytes(const uint8_t* const data, const size_t len)
{
    HAL_UART_Transmit(&handle, data, static_cast<uint16_t>(len), HAL_MAX_DELAY);
}
