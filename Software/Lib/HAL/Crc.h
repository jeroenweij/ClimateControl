/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace Hal
{
    // Hardware CRC peripheral. One configuration per instance -- the peripheral
    // is reprogrammed in the constructor. The two polynomials in use:
    //   Ccitt16 : CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) -- the RS485
    //             frame CRC, RS485-Node-Protocol-Spec-STM32G030.md §4.
    //   Ieee32  : CRC-32 (poly 0x04C11DB7, init 0xFFFFFFFF, no reflection, no
    //             final xor) -- the OTA whole-image check,
    //             Node-Flash-Layout-and-Bootloader-Spec.md §4.
    //
    // The peripheral is a single shared resource: a binary that needs both
    // widths at once would have to re-init between calls. Today the apps use
    // only Ccitt16 (via Frame) and the bootloader only Ieee32, so one config
    // per image is enough.
    class Crc
    {
      public:
        enum class Poly
        {
            Ccitt16,
            Ieee32,
        };

        explicit Crc(Poly poly = Poly::Ccitt16);

        // Resets the peripheral and computes over data..data+len. Use the call
        // that matches the Poly this instance was constructed with.
        uint16_t Compute(const uint8_t* data, size_t len);
        uint32_t Compute32(const uint8_t* data, size_t len);
    };
} // namespace Hal
