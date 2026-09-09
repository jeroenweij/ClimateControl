/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

#include "Gpio.h"
#include "Pin.h"

namespace Hal
{
    // Bit-bang 1-Wire master (standard speed) on one open-drain GPIO with an
    // external pull-up. Bit-slot timing follows Maxim AN126; interrupts are
    // masked only across each time-critical low pulse / sample, never the
    // recovery gaps.
    //
    // Single-device-per-line use only (the DS18B20 duct probes each get their
    // own line, TemperatureNode-Spec.md §4.1) -- ROM search is not implemented,
    // callers use Skip-ROM.
    class OneWire
    {
      public:
        explicit OneWire(const Pin pin);

        // Reset pulse + presence detect. True if at least one device pulled the
        // line low in the presence window.
        bool Reset();

        void    WriteByte(const uint8_t byte); // LSB first
        uint8_t ReadByte(); // LSB first

        // Dallas/Maxim CRC-8 (poly x^8+x^5+x^4+1, reflected) -- the DS-family
        // ROM / scratchpad check byte.
        static uint8_t Crc8(const uint8_t* const data, const uint8_t len);

      private:
        void WriteBit(const bool bit);
        bool ReadBit();

        void driveLow();
        void release();
        bool sample() const;

        Gpio gpio; // owns pin config (open-drain); I/O below is raw BSRR/IDR
        Pin  pin;
    };
} // namespace Hal
