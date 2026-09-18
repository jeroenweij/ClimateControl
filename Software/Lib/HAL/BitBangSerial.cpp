/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "Tick.h"

#include "BitBangSerial.h"

using Hal::BitBangSerial;
using Hal::Pin;

BitBangSerial::BitBangSerial(const Pin pin, const uint32_t baudRate) :
    gpio(pin, Gpio::Mode::Output),
    bitPeriodUs(1000000u / baudRate)
{
    gpio.Write(true); // idle high
}

namespace
{
    // Busy-wait until Hal::Tick::Micros() reaches 'deadline' -- wraparound-safe
    // signed comparison, see Tick.h.
    void WaitUntil(const uint32_t deadline)
    {
        while (static_cast<int32_t>(Hal::Tick::Micros() - deadline) < 0)
        {
        }
    }
} // namespace

void BitBangSerial::WriteByte(const uint8_t byte)
{
    uint32_t deadline = Hal::Tick::Micros();

    gpio.Write(false); // start bit
    deadline += bitPeriodUs;
    WaitUntil(deadline);

    for (uint8_t bit = 0; bit < 8; bit++)
    {
        gpio.Write(((byte >> bit) & 0x01) != 0);
        deadline += bitPeriodUs;
        WaitUntil(deadline);
    }

    gpio.Write(true); // stop bit
    deadline += bitPeriodUs;
    WaitUntil(deadline);
}

void BitBangSerial::WriteString(const char* const text)
{
    for (const char* p = text; *p != '\0'; p++)
    {
        WriteByte(static_cast<uint8_t>(*p));
    }
}
