/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "Ds18b20.h"

using Hal::OneWire;
using Hal::Pin;

namespace
{
    constexpr uint8_t ScratchpadLen = 9; // bytes 0..7 payload, byte 8 = CRC-8

    // Temperature register power-on value (+85 degC). Read back with a valid
    // CRC when the conversion never ran -- the probe browned out / reset, or is
    // wired parasite-powered with no strong pull-up. Far above any duct air
    // temperature, so a real 85.00 reading is not a concern.
    constexpr int16_t PowerOnRaw = 0x0550;

    // DS18B20 raw reading is int16 in units of 1/16 °C. Convert to 1/100 °C:
    // centi = raw * 100 / 16 = raw * 25 / 4 (int32 keeps the full range).
    int16_t RawToCentiDeg(const int16_t raw)
    {
        return static_cast<int16_t>((static_cast<int32_t>(raw) * 25) / 4);
    }
} // namespace

Ds18b20::Ds18b20(const Pin oneWirePin) :
    wire(oneWirePin)
{
}

bool Ds18b20::StartConversion()
{
    if (!wire.Reset())
    {
        return false;
    }
    wire.WriteByte(SkipRom);
    wire.WriteByte(ConvertT);
    return true;
}

bool Ds18b20::ReadTemperature(int16_t& centiDegC)
{
    if (!wire.Reset())
    {
        return false;
    }
    wire.WriteByte(SkipRom);
    wire.WriteByte(ReadScratchpad);

    uint8_t scratchpad[ScratchpadLen] = {};
    uint8_t stuckBits                 = 0xFF; // AND of every byte
    uint8_t setBits                   = 0x00; // OR of every byte
    for (uint8_t i = 0; i < ScratchpadLen; i++)
    {
        scratchpad[i] = wire.ReadByte();
        stuckBits &= scratchpad[i];
        setBits |= scratchpad[i];
    }

    // A line stuck high (0xFF...) or low (0x00...) can still satisfy the CRC
    // (CRC-8 of all-zeros is zero), so reject an all-identical scratchpad first.
    if (stuckBits == setBits)
    {
        return false;
    }

    if (OneWire::Crc8(scratchpad, ScratchpadLen - 1) != scratchpad[ScratchpadLen - 1])
    {
        return false;
    }

    const int16_t raw = static_cast<int16_t>(static_cast<uint16_t>(scratchpad[0]) |
                                             (static_cast<uint16_t>(scratchpad[1]) << 8U));
    if (raw == PowerOnRaw)
    {
        return false;
    }

    centiDegC = RawToCentiDeg(raw);
    return true;
}
