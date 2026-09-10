/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "OneWire.h"

#include "FakeOneWire.h"

using Hal::OneWire;
using Hal::Pin;

namespace
{
    struct Line
    {
        bool          used;
        GPIO_TypeDef* port;
        uint16_t      pin;
        bool          present;
        uint8_t       readQueue[32];
        size_t        readLen;
        size_t        readPos;
        uint8_t       written[64];
        size_t        writtenLen;
        int           resets;
    };

    const int maxLines = 4;
    Line      lines[maxLines];

    Line& LineFor(const Pin& p)
    {
        for (int i = 0; i < maxLines; i++)
        {
            if (lines[i].used && lines[i].port == p.port && lines[i].pin == p.pin)
            {
                return lines[i];
            }
        }
        for (int i = 0; i < maxLines; i++)
        {
            if (!lines[i].used)
            {
                lines[i]      = Line{};
                lines[i].used = true;
                lines[i].port = p.port;
                lines[i].pin  = p.pin;
                return lines[i];
            }
        }
        return lines[0];
    }
} // namespace

namespace FakeOneWire
{
    void ResetAll()
    {
        for (int i = 0; i < maxLines; i++)
        {
            lines[i] = Line{};
        }
    }

    void SetPresent(const Pin& pin, const bool present)
    {
        LineFor(pin).present = present;
    }

    void QueueRead(const Pin& pin, const uint8_t* const data, const size_t len)
    {
        Line& line = LineFor(pin);
        for (size_t i = 0; i < len && line.readLen < sizeof(line.readQueue); i++)
        {
            line.readQueue[line.readLen++] = data[i];
        }
    }

    size_t WrittenLen(const Pin& pin)
    {
        return LineFor(pin).writtenLen;
    }

    const uint8_t* Written(const Pin& pin)
    {
        return LineFor(pin).written;
    }

    int ResetCount(const Pin& pin)
    {
        return LineFor(pin).resets;
    }
} // namespace FakeOneWire

OneWire::OneWire(const Pin pin) :
    gpio(pin, Hal::Gpio::Mode::OpenDrain),
    pin(pin)
{
}

bool OneWire::Reset()
{
    Line& line = LineFor(pin);
    line.resets++;
    line.readPos    = 0;
    line.writtenLen = 0;
    return line.present;
}

void OneWire::WriteByte(const uint8_t byte)
{
    Line& line = LineFor(pin);
    if (line.writtenLen < sizeof(line.written))
    {
        line.written[line.writtenLen++] = byte;
    }
}

uint8_t OneWire::ReadByte()
{
    Line& line = LineFor(pin);
    return line.readPos < line.readLen ? line.readQueue[line.readPos++] : 0xFFu;
}

uint8_t OneWire::Crc8(const uint8_t* const data, const uint8_t len)
{
    // Dallas/Maxim CRC-8, poly 0x8C (reflected) -- byte-for-byte identical to
    // Hal::OneWire::Crc8 in Lib/HAL/OneWire.cpp; both are pinned by the
    // scratchpad known-answer vector in OneWireTests.cpp / Ds18b20Tests.cpp.
    uint8_t crc = 0;
    for (uint8_t i = 0; i < len; i++)
    {
        uint8_t byte = data[i];
        for (uint8_t bit = 0; bit < 8u; bit++)
        {
            const uint8_t mix = (crc ^ byte) & 0x01u;
            crc >>= 1u;
            if (mix != 0u)
            {
                crc ^= 0x8Cu;
            }
            byte >>= 1u;
        }
    }
    return crc;
}
