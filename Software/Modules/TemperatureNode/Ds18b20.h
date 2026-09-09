/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

#include "OneWire.h"
#include "Pin.h"

// One DS18B20 on its own single-drop 1-Wire line (TemperatureNode-Spec.md §4.1).
// Skip-ROM addressing -- exactly one device per line, no ROM search.
//
// A conversion is not blocking: StartConversion() kicks it off, then after
// ConversionTimeMs the value is fetched with ReadTemperature().
class Ds18b20
{
  public:
    // Worst-case conversion time, 12-bit resolution (datasheet: 750 ms max).
    static constexpr uint32_t ConversionTimeMs = 750;

    explicit Ds18b20(const Hal::Pin oneWirePin);

    // Reset + Skip-ROM + CONVERT T. False if no device answered the reset.
    bool StartConversion();

    // Reset + Skip-ROM + READ SCRATCHPAD, verify CRC-8, decode. On success
    // 'centiDegC' holds the temperature in 1/100 °C; on failure it is untouched.
    bool ReadTemperature(int16_t& centiDegC);

  private:
    enum Command : uint8_t
    {
        SkipRom        = 0xCC,
        ConvertT       = 0x44,
        ReadScratchpad = 0xBE,
    };

    Hal::OneWire wire;
};
