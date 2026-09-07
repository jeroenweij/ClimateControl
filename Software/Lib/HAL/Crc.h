/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace Hal
{
    // Hardware CRC peripheral configured for CRC-16/CCITT-FALSE (poly 0x1021,
    // init 0xFFFF, no reflect, no final XOR), per RS485-Node-Protocol-Spec-STM32G030.md
    // Sec4.
    class Crc
    {
      public:
        Crc();

        // Resets the peripheral and computes the CRC over data..data+len, per the
        // spec's "Reset before each frame" requirement.
        uint16_t Compute(const uint8_t* const data, const size_t len);
    };
} // namespace Hal
