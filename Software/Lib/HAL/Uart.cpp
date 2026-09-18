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

    // Non-blocking TX ring buffer, one per instance, same keyed-by-Instance
    // pattern as 'handles' above. Sized to comfortably hold the largest
    // single enqueue used anywhere: a full NodeLib frame (2 sync + 1 len + 3
    // header + 32 data + 2 crc = 40 bytes, Id.h's MAX_DATA) or the longest AT
    // command MainController sends NINA (AT+UWSC's SSID/PSK strings, under
    // 100 bytes) -- with headroom to queue a couple of those before a
    // WriteBytes() call has to reject for lack of room.
    const size_t txBufferSize = 128;

    struct TxRingBuffer
    {
        uint8_t buffer[txBufferSize];
        size_t  head; // next write index
        size_t  tail; // next read index
        size_t  count;
    };
    TxRingBuffer txBuffers[2] = {};

    USART_TypeDef* Regs(const Uart::Instance instance)
    {
        return instance == Uart::Instance::Usart2 ? USART2 : USART1;
    }

    UART_HandleTypeDef& Handle(const Uart::Instance instance)
    {
        return handles[static_cast<int>(instance)];
    }

    TxRingBuffer& TxBuffer(const Uart::Instance instance)
    {
        return txBuffers[static_cast<int>(instance)];
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

    // Shared by both Init() overloads: clocks, TX/RX pin mux, and the common
    // UART_InitTypeDef fields. Returns the handle for the caller to finish
    // (RS485Ex vs. plain HAL_UART_Init).
    UART_HandleTypeDef& ConfigureHandle(const uint32_t       baudRate,
                                        const Uart::Instance instance,
                                        const Hal::UartPin&  tx,
                                        const Hal::UartPin&  rx)
    {
        __HAL_RCC_GPIOA_CLK_ENABLE();
        __HAL_RCC_GPIOB_CLK_ENABLE();
        if (instance == Uart::Instance::Usart2)
        {
            __HAL_RCC_USART2_CLK_ENABLE();
        }
        else
        {
            __HAL_RCC_USART1_CLK_ENABLE();
        }

        ConfigureAfPin(tx.pin, tx.alternateFunction);
        ConfigureAfPin(rx.pin, rx.alternateFunction);

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

        return handle;
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

    UART_HandleTypeDef& handle = ConfigureHandle(baudRate, instance, pins.tx, pins.rx);
    ConfigureAfPin(pins.de.pin, pins.de.alternateFunction);

    // DEAT/DEDT are in bit-periods (5-bit fields). 1 bit-period is a
    // conservative placeholder -- not yet tuned against real bus/transceiver
    // turnaround timing, see Node-Bus-Hardware-Design-Spec.md §6.
    const uint32_t assertionTime   = 1;
    const uint32_t deassertionTime = 1;
    HAL_RS485Ex_Init(&handle, UART_DE_POLARITY_HIGH, assertionTime, deassertionTime);
}

void Uart::Init(const uint32_t baudRate, const Instance instance, const UartPin& tx, const UartPin& rx)
{
    this->instance = instance;

    UART_HandleTypeDef& handle = ConfigureHandle(baudRate, instance, tx, rx);
    HAL_UART_Init(&handle);
}

bool Uart::Available() const
{
    UART_HandleTypeDef& handle = Handle(instance);

    // STM32G0's USART is the ISR/ICR-style peripheral (not the older SR/DR
    // one where reading DR auto-clears error flags) -- an overrun (RDR is a
    // single byte deep, no FIFO) leaves ORE set and RXNE stuck low until ORE
    // is explicitly cleared, permanently deafening this port otherwise. Any
    // data still in RDR when this happens is lost either way, so just clear
    // it and let framing resync (as designed, RS485-Node-Protocol-Spec-
    // STM32G030.md §5) or the caller's own line/response parsing recover.
    if (__HAL_UART_GET_FLAG(&handle, UART_FLAG_ORE))
    {
        __HAL_UART_CLEAR_OREFLAG(&handle);
    }

    return __HAL_UART_GET_FLAG(&handle, UART_FLAG_RXNE);
}

uint8_t Uart::ReadByte()
{
    return static_cast<uint8_t>(Regs(instance)->RDR);
}

bool Uart::WriteBytes(const uint8_t* const data, const size_t len)
{
    TxRingBuffer& tx = TxBuffer(instance);

    if (len > txBufferSize - tx.count)
    {
        return false; // wouldn't fully fit -- reject atomically, queue nothing
    }

    for (size_t i = 0; i < len; i++)
    {
        tx.buffer[tx.head] = data[i];
        tx.head            = (tx.head + 1) % txBufferSize;
    }
    tx.count += len;
    return true;
}

void Uart::Pump()
{
    UART_HandleTypeDef& handle = Handle(instance);
    TxRingBuffer&       tx     = TxBuffer(instance);

    if (tx.count == 0)
    {
        return;
    }
    if (!__HAL_UART_GET_FLAG(&handle, UART_FLAG_TXE))
    {
        return;
    }

    Regs(instance)->TDR = tx.buffer[tx.tail];
    tx.tail             = (tx.tail + 1) % txBufferSize;
    tx.count--;
}
