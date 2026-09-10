/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "OneWire.h"

#include "Test.h"

using Hal::OneWire;

CC_TEST(OneWireCrc8, EmptyAndZeroAreZero)
{
    CC_CHECK_EQ(OneWire::Crc8(nullptr, 0), 0);

    const uint8_t zero[1] = {0x00};
    CC_CHECK_EQ(OneWire::Crc8(zero, 1), 0);
}

CC_TEST(OneWireCrc8, KnownRomVector)
{
    // A DS18B20 ROM code (family 0x28) -- Maxim/Dallas CRC-8, poly 0x8C.
    const uint8_t rom[7] = {0x28, 0x1D, 0x39, 0x31, 0x02, 0x00, 0x00};
    CC_CHECK_EQ(OneWire::Crc8(rom, 7), 0xF0);
}

CC_TEST(OneWireCrc8, KnownScratchpadVector)
{
    const uint8_t scratchpad[8] = {0x90, 0x01, 0x4B, 0x46, 0x7F, 0xFF, 0x0C, 0x10};
    CC_CHECK_EQ(OneWire::Crc8(scratchpad, 8), 0x33);
}

CC_TEST(OneWireCrc8, AppendingTheCheckByteMakesTheWholeCrcZero)
{
    uint8_t frame[9] = {0x90, 0x01, 0x4B, 0x46, 0x7F, 0xFF, 0x0C, 0x10, 0x00};
    frame[8]         = OneWire::Crc8(frame, 8);
    CC_CHECK_EQ(OneWire::Crc8(frame, 9), 0);
}
