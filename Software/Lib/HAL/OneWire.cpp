/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "stm32g0xx_hal.h"

#include "Tick.h"

#include "OneWire.h"

using Hal::OneWire;
using Hal::Pin;

namespace
{
    // Maxim AN126 "standard speed" bit-slot timing, microseconds.
    constexpr uint32_t ResetLowUs        = 480; // H  master holds the line low
    constexpr uint32_t PresenceWaitUs    = 70; //  I  release -> sample presence
    constexpr uint32_t ResetRecoveryUs   = 410; // J  rest of the reset slot
    constexpr uint32_t WriteOneLowUs     = 6; //   A
    constexpr uint32_t WriteOneReleaseUs = 64; //  B
    constexpr uint32_t WriteZeroLowUs    = 60; //  C
    constexpr uint32_t WriteZeroRecovUs  = 10; //  D
    constexpr uint32_t ReadLowUs         = 6; //   A
    constexpr uint32_t ReadToSampleUs    = 8; //   E  release -> sample (must be < 15 us total)
    constexpr uint32_t ReadRecoveryUs    = 55; //  F

    // PRIMASK-preserving critical section -- masks interrupts across a single
    // time-critical low pulse / sample without clobbering a caller that already
    // had them disabled. Kept as short as possible (never across a recovery
    // delay) so a pending SysTick is always serviced in the gap between bits and
    // HAL_GetTick() does not drift.
    class IrqGuard
    {
      public:
        IrqGuard() :
            primask(__get_PRIMASK())
        {
            __disable_irq();
        }

        ~IrqGuard()
        {
            __set_PRIMASK(primask);
        }

        IrqGuard(const IrqGuard&)            = delete;
        IrqGuard& operator=(const IrqGuard&) = delete;

      private:
        uint32_t primask;
    };
} // namespace

OneWire::OneWire(const Pin pin) :
    gpio(pin, Gpio::Mode::OpenDrain),
    pin(pin)
{
    release();
}

void OneWire::driveLow()
{
    pin.port->BSRR = static_cast<uint32_t>(pin.pin) << 16U; // reset -> output low
}

void OneWire::release()
{
    pin.port->BSRR = pin.pin; // set -> Hi-Z (open-drain), pull-up raises the line
}

bool OneWire::sample() const
{
    return (pin.port->IDR & pin.pin) != 0U;
}

bool OneWire::Reset()
{
    // The 480 us low and the wide presence window are timing-forgiving, so this
    // slot runs with interrupts live; only the presence sample is fenced.
    driveLow();
    Hal::Tick::DelayUs(ResetLowUs);
    release();

    bool present;
    {
        IrqGuard guard;
        Hal::Tick::DelayUs(PresenceWaitUs);
        present = !sample(); // a device answers by pulling the line low
    }

    Hal::Tick::DelayUs(ResetRecoveryUs);
    return present;
}

void OneWire::WriteBit(const bool bit)
{
    // Only the low pulse (and its terminating release) defines the bit; the
    // trailing slot time is not timing-critical, so it runs unmasked.
    {
        IrqGuard guard;
        driveLow();
        Hal::Tick::DelayUs(bit ? WriteOneLowUs : WriteZeroLowUs);
        release();
    }
    Hal::Tick::DelayUs(bit ? WriteOneReleaseUs : WriteZeroRecovUs);
}

bool OneWire::ReadBit()
{
    bool bit;
    {
        IrqGuard guard;
        driveLow();
        Hal::Tick::DelayUs(ReadLowUs);
        release();
        Hal::Tick::DelayUs(ReadToSampleUs);
        bit = sample();
    }
    Hal::Tick::DelayUs(ReadRecoveryUs);
    return bit;
}

void OneWire::WriteByte(const uint8_t byte)
{
    for (uint8_t i = 0; i < 8U; i++)
    {
        WriteBit((byte & (1U << i)) != 0U);
    }
}

uint8_t OneWire::ReadByte()
{
    uint8_t byte = 0;
    for (uint8_t i = 0; i < 8U; i++)
    {
        if (ReadBit())
        {
            byte |= static_cast<uint8_t>(1U << i);
        }
    }
    return byte;
}

uint8_t OneWire::Crc8(const uint8_t* const data, const uint8_t len)
{
    uint8_t crc = 0;
    for (uint8_t i = 0; i < len; i++)
    {
        uint8_t byte = data[i];
        for (uint8_t bit = 0; bit < 8U; bit++)
        {
            const uint8_t mix = (crc ^ byte) & 0x01U;
            crc >>= 1U;
            if (mix != 0U)
            {
                crc ^= 0x8CU;
            }
            byte >>= 1U;
        }
    }
    return crc;
}
