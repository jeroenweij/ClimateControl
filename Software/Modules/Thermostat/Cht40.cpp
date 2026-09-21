/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "Tick.h"

#include "Cht40.h"

namespace
{
    // Datasheet §4.4: 8.3 ms max at high repeatability.
    constexpr uint32_t MaxConversionTimeMs = 10;
} // namespace

Cht40::Cht40(Hal::I2c& bus, const uint8_t address7) :
    bus(bus),
    address(address7)
{
}

bool Cht40::Measure(int16_t& centiDegC, uint16_t& centiRH)
{
    const uint8_t cmd = MeasureHighRepeatability;
    if (!bus.Write(address, &cmd, 1))
    {
        return false;
    }

    Hal::Tick::DelayMs(MaxConversionTimeMs);

    uint8_t raw[6];
    if (!bus.Read(address, raw, sizeof(raw)))
    {
        return false;
    }
    if (Crc8(&raw[0], 2) != raw[2] || Crc8(&raw[3], 2) != raw[5])
    {
        return false;
    }

    const uint16_t rawT  = static_cast<uint16_t>((raw[0] << 8) | raw[1]);
    const uint16_t rawRH = static_cast<uint16_t>((raw[3] << 8) | raw[4]);

    // Datasheet §5.5: T[C] = -45 + 175*S_T/65535, RH[%] = -6 + 125*S_RH/65535 --
    // centi-units throughout, matching the rest of the codebase's convention.
    const int32_t t = -4500 + static_cast<int32_t>((17500u * static_cast<uint32_t>(rawT)) / 65535u);
    centiDegC       = static_cast<int16_t>(t);

    int32_t rh = -600 + static_cast<int32_t>((12500u * static_cast<uint32_t>(rawRH)) / 65535u);
    if (rh < 0)
    {
        rh = 0; // datasheet §5.5: the formula can run outside 0-100%, clamp for the physical quantity
    }
    if (rh > 10000)
    {
        rh = 10000;
    }
    centiRH = static_cast<uint16_t>(rh);

    return true;
}

uint8_t Cht40::Crc8(const uint8_t* const data, const uint8_t len)
{
    // Datasheet §5.3: CRC-8, poly 0x31 (x^8+x^5+x^4+1), init 0xFF, not
    // reflected, no final XOR -- identical to Sensirion's SHT4x CRC.
    uint8_t crc = 0xFFu;
    for (uint8_t i = 0; i < len; i++)
    {
        crc = static_cast<uint8_t>(crc ^ data[i]);
        for (uint8_t bit = 0; bit < 8u; bit++)
        {
            crc = (crc & 0x80u) ? static_cast<uint8_t>((crc << 1) ^ 0x31u) : static_cast<uint8_t>(crc << 1);
        }
    }
    return crc;
}
