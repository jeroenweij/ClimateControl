/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "BoardPins.h"

#include "Ds18b20.h"

#include "FakeOneWire.h"
#include "Test.h"

using Hal::OneWire;

namespace
{
    const Hal::Pin line = Board::OneWire1;

    // 9-byte DS18B20 scratchpad for a given raw (1/16 degC) reading, with a
    // valid trailing CRC-8.
    void MakeScratchpad(const int16_t raw, uint8_t out[9])
    {
        out[0] = static_cast<uint8_t>(raw & 0xFF);
        out[1] = static_cast<uint8_t>((raw >> 8) & 0xFF);
        out[2] = 0x4B; // TH
        out[3] = 0x46; // TL
        out[4] = 0x7F; // config: 12-bit
        out[5] = 0xFF;
        out[6] = 0x0C;
        out[7] = 0x10;
        out[8] = OneWire::Crc8(out, 8);
    }
} // namespace

CC_TEST(Ds18b20, StartConversionIssuesSkipRomThenConvertT)
{
    FakeOneWire::ResetAll();
    FakeOneWire::SetPresent(line, true);

    Ds18b20 sensor(line);
    CC_CHECK(sensor.StartConversion());

    CC_CHECK_EQ(FakeOneWire::WrittenLen(line), 2);
    CC_CHECK_EQ(FakeOneWire::Written(line)[0], 0xCC); // SKIP ROM
    CC_CHECK_EQ(FakeOneWire::Written(line)[1], 0x44); // CONVERT T
}

CC_TEST(Ds18b20, NoPresencePulseFailsBothCalls)
{
    FakeOneWire::ResetAll();
    FakeOneWire::SetPresent(line, false);

    Ds18b20 sensor(line);
    CC_CHECK(!sensor.StartConversion());

    int16_t centi = 999;
    CC_CHECK(!sensor.ReadTemperature(centi));
    CC_CHECK_EQ(centi, 999); // left untouched
}

CC_TEST(Ds18b20, DecodesAPositiveReading)
{
    FakeOneWire::ResetAll();
    FakeOneWire::SetPresent(line, true);

    uint8_t scratchpad[9];
    MakeScratchpad(0x0190, scratchpad); // +25.0 degC == 400 * (1/16)
    FakeOneWire::QueueRead(line, scratchpad, 9);

    Ds18b20 sensor(line);
    int16_t centi = 0;
    CC_CHECK(sensor.ReadTemperature(centi));
    CC_CHECK_EQ(centi, 2500);
}

CC_TEST(Ds18b20, DecodesANegativeReading)
{
    FakeOneWire::ResetAll();
    FakeOneWire::SetPresent(line, true);

    uint8_t scratchpad[9];
    MakeScratchpad(static_cast<int16_t>(-400), scratchpad); // -25.0 degC
    FakeOneWire::QueueRead(line, scratchpad, 9);

    Ds18b20 sensor(line);
    int16_t centi = 0;
    CC_CHECK(sensor.ReadTemperature(centi));
    CC_CHECK_EQ(centi, -2500);
}

CC_TEST(Ds18b20, RejectsABadCrc)
{
    FakeOneWire::ResetAll();
    FakeOneWire::SetPresent(line, true);

    uint8_t scratchpad[9];
    MakeScratchpad(0x0190, scratchpad);
    scratchpad[8] ^= 0xFF; // corrupt the check byte
    FakeOneWire::QueueRead(line, scratchpad, 9);

    Ds18b20 sensor(line);
    int16_t centi = 123;
    CC_CHECK(!sensor.ReadTemperature(centi));
    CC_CHECK_EQ(centi, 123);
}

CC_TEST(Ds18b20, RejectsAStuckHighLine)
{
    FakeOneWire::ResetAll();
    FakeOneWire::SetPresent(line, true);

    const uint8_t stuck[9] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    FakeOneWire::QueueRead(line, stuck, 9);

    Ds18b20 sensor(line);
    int16_t centi = 0;
    CC_CHECK(!sensor.ReadTemperature(centi));
}

CC_TEST(Ds18b20, RejectsAStuckLowLine)
{
    FakeOneWire::ResetAll();
    FakeOneWire::SetPresent(line, true);

    const uint8_t stuck[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
    FakeOneWire::QueueRead(line, stuck, 9);

    Ds18b20 sensor(line);
    int16_t centi = 0;
    CC_CHECK(!sensor.ReadTemperature(centi));
}
