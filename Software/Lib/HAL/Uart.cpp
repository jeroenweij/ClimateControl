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

    // Interrupt-fed ring buffers, one RX and one TX per instance, same
    // keyed-by-Instance pattern as 'handles' above. Single-producer/single-
    // consumer by construction -- for RX, USARTx_IRQHandler is the only
    // writer of 'head' and Available()/ReadByte() (main-loop context) the
    // only writer of 'tail'; for TX it's the reverse (WriteBytes() writes
    // 'head', the ISR writes 'tail') -- so no locking is needed as long as
    // each side only ever writes its own index. 'head'/'tail' are volatile
    // since they're read across that ISR/main-loop boundary. One slot is
    // always left empty to distinguish full from empty without a separate
    // counter (capacity is bufferSize - 1).
    //
    // Sized to comfortably hold the largest single enqueue used anywhere: a
    // full NodeLib frame (2 sync + 1 len + 3 header + 32 data + 2 crc = 40
    // bytes, Id.h's MAX_DATA) or the longest AT command MainController sends
    // NINA (AT+UWSC's SSID/PSK strings, under 100 bytes) -- with headroom for
    // the main loop to fall behind briefly (e.g. a blocking bit-banged log
    // line elsewhere) without losing bytes the way the old single-byte
    // hardware RDR did.
    const size_t bufferSize = 128;

    struct RingBuffer
    {
        uint8_t         buffer[bufferSize];
        volatile size_t head;
        volatile size_t tail;
    };
    RingBuffer rxBuffers[2] = {};
    RingBuffer txBuffers[2] = {};

    USART_TypeDef* Regs(const Uart::Instance instance)
    {
        return instance == Uart::Instance::Usart2 ? USART2 : USART1;
    }

    UART_HandleTypeDef& Handle(const Uart::Instance instance)
    {
        return handles[static_cast<int>(instance)];
    }

    RingBuffer& RxBuffer(const Uart::Instance instance)
    {
        return rxBuffers[static_cast<int>(instance)];
    }

    RingBuffer& TxBuffer(const Uart::Instance instance)
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

    // Shared by both Init() overloads: clocks, TX/RX pin mux, the common
    // UART_InitTypeDef fields, and the RX interrupt (always on -- RXNEIE is
    // armed once here and never turned off; TXEIE is armed/disarmed per
    // transfer by WriteBytes()/the ISR instead, see below). Returns the
    // handle for the caller to finish (RS485Ex vs. plain HAL_UART_Init).
    UART_HandleTypeDef& ConfigureHandle(const uint32_t       baudRate,
                                        const Uart::Instance instance,
                                        const Hal::UartPin&  tx,
                                        const Hal::UartPin&  rx)
    {
        __HAL_RCC_GPIOA_CLK_ENABLE();
        __HAL_RCC_GPIOB_CLK_ENABLE();

        const IRQn_Type irqn = (instance == Uart::Instance::Usart2) ? USART2_IRQn : USART1_IRQn;
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

        RxBuffer(instance) = RingBuffer{};
        TxBuffer(instance) = RingBuffer{};

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

        // Highest priority (STM32G0's 2 priority bits -> 0..3) -- byte time
        // at this baud is tens of microseconds, far tighter than anything
        // else in this firmware; SysTick runs at the HAL default, lowest
        // priority (3), so it never delays servicing a byte.
        HAL_NVIC_SetPriority(irqn, 0, 0);
        HAL_NVIC_EnableIRQ(irqn);
        __HAL_UART_ENABLE_IT(&handle, UART_IT_RXNE);

        return handle;
    }

    // Common to both instances' USARTx_IRQHandler -- empties RDR into the RX
    // ring buffer (dropping on a full buffer, same backstop philosophy as
    // everywhere else in this codebase: reading RDR either way is mandatory,
    // not just to keep the byte -- leaving it unread would leave RXNE set and
    // re-fire this same interrupt forever) and, if TXEIE is enabled and the
    // hardware's ready, feeds the next queued TX byte to TDR.
    void ServiceIrq(const Uart::Instance instance)
    {
        UART_HandleTypeDef& handle = Handle(instance);

        if (__HAL_UART_GET_FLAG(&handle, UART_FLAG_ORE))
        {
            // Single-byte-deep RDR overrun -- the byte under it is already
            // lost either way; clear so RXNE isn't stuck low and framing
            // resyncs on the next valid frame (RS485-Node-Protocol-Spec-
            // STM32G030.md §5).
            __HAL_UART_CLEAR_OREFLAG(&handle);
        }

        if (__HAL_UART_GET_IT_SOURCE(&handle, UART_IT_RXNE) && __HAL_UART_GET_FLAG(&handle, UART_FLAG_RXNE))
        {
            RingBuffer&   rx   = RxBuffer(instance);
            const uint8_t byte = static_cast<uint8_t>(Regs(instance)->RDR); // clears RXNE
            const size_t  next = (rx.head + 1) % bufferSize;
            if (next != rx.tail)
            {
                rx.buffer[rx.head] = byte;
                rx.head            = next;
            }
            // else: RX ring buffer full -- drop, same as an unread overrun.
        }

        if (__HAL_UART_GET_IT_SOURCE(&handle, UART_IT_TXE) && __HAL_UART_GET_FLAG(&handle, UART_FLAG_TXE))
        {
            RingBuffer& tx = TxBuffer(instance);
            if (tx.head == tx.tail)
            {
                // Nothing left queued -- stop asking for TXE, or this fires
                // continuously (TXE reads back set whenever TDR is empty).
                __HAL_UART_DISABLE_IT(&handle, UART_IT_TXE);
            }
            else
            {
                Regs(instance)->TDR = tx.buffer[tx.tail];
                tx.tail             = (tx.tail + 1) % bufferSize;
            }
        }
    }
} // namespace

// Clock + GPIO are brought up in Init() below (per instance, no shared state),
// so the HAL's MspInit hook has nothing to do.
extern "C" void HAL_UART_MspInit(UART_HandleTypeDef*)
{
}

// startup_stm32g031xx.s leaves these weakly aliased to Default_Handler (an
// infinite-loop trap, same pitfall as SysTick_Handler before Tick.cpp's own
// fix) -- without real definitions, enabling either NVIC line here would
// hang the CPU on the first byte.
extern "C" void USART1_IRQHandler()
{
    ServiceIrq(Uart::Instance::Usart1);
}

extern "C" void USART2_IRQHandler()
{
    ServiceIrq(Uart::Instance::Usart2);
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
    const RingBuffer& rx = RxBuffer(instance);
    return rx.head != rx.tail;
}

uint8_t Uart::ReadByte()
{
    RingBuffer& rx = RxBuffer(instance);
    if (rx.head == rx.tail)
    {
        return 0;
    }
    const uint8_t byte = rx.buffer[rx.tail];
    rx.tail            = (rx.tail + 1) % bufferSize;
    return byte;
}

bool Uart::WriteBytes(const uint8_t* const data, const size_t len)
{
    UART_HandleTypeDef& handle = Handle(instance);
    RingBuffer&         tx     = TxBuffer(instance);

    // Free space = capacity (bufferSize - 1 usable slots) minus what's
    // already queued. Safe to read 'tail' here without excluding the ISR:
    // this is the buffer's one producer (only WriteBytes() ever writes
    // 'head', never re-entered -- called only from the single super-loop)
    // and the ISR is the one consumer (only ever writes 'tail'), so if the
    // ISR drains more bytes while this runs, occupied space only shrinks --
    // never invalidates a size check already based on a smaller free space.
    const size_t occupied = (tx.head >= tx.tail) ? (tx.head - tx.tail) : (bufferSize - tx.tail + tx.head);
    if (len > bufferSize - 1 - occupied)
    {
        return false; // wouldn't fully fit -- reject atomically, queue nothing
    }

    for (size_t i = 0; i < len; i++)
    {
        tx.buffer[tx.head] = data[i];
        tx.head            = (tx.head + 1) % bufferSize;
    }

    // Arms (or re-arms, harmlessly idempotent if a transfer is already in
    // flight) TXEIE so the ISR starts draining this buffer -- TDR is empty
    // whenever nothing's transmitting, so TXE is already set and the
    // interrupt fires as soon as this bit goes high.
    __HAL_UART_ENABLE_IT(&handle, UART_IT_TXE);
    return true;
}
