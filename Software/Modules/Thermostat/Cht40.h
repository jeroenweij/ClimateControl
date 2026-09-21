/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

#include "I2c.h"

// CYBERSEN CHT40MEMS room temperature/humidity sensor, on the Thermostat's
// shared I2C1 bus with the OLED (ControllerNode-Thermostat-Link-Spec.md
// §4.1/§4.3). Confirmed against the CHT40MEMS datasheet (CYBERSEN CHT40MEMS.pdf,
// 2026-09) to be command/CRC/conversion-formula compatible with Sensirion's
// public SHT4x protocol -- §5.1 (I2C framing), §5.3 (CRC-8), §5.4 (commands),
// §5.5 (T/RH conversion) all match SHT4x byte-for-byte.
class Cht40
{
  public:
    // CHT40MEMS-AD; the -BD variant (0x45) is a straight swap if that's what's
    // populated -- see the part table on the datasheet's page 1.
    static constexpr uint8_t DefaultAddress = 0x44;

    explicit Cht40(Hal::I2c& bus, const uint8_t address7 = DefaultAddress);

    // Blocking: issues the high-repeatability measure command, waits out the
    // worst-case conversion time (datasheet §4.4: 8.3 ms max), reads back and
    // CRC-checks both fields. False (values left untouched) on an I2C NACK
    // (e.g. the sensor wasn't done -- it NACKs a read started too early,
    // datasheet §5.1) or a CRC mismatch.
    bool Measure(int16_t& centiDegC, uint16_t& centiRH);

    // Datasheet §5.3: CRC-8, poly 0x31 (x^8+x^5+x^4+1), init 0xFF, no
    // reflection, no final XOR. Exposed for the known-answer test
    // (CRC(0xBEEF) == 0x92, per the datasheet's own worked example).
    static uint8_t Crc8(const uint8_t* const data, const uint8_t len);

  private:
    enum Command : uint8_t
    {
        MeasureHighRepeatability = 0xFD,
    };

    Hal::I2c& bus;
    uint8_t   address;
};
