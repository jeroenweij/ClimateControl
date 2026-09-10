/*************************************************************
 * Created by J. Weij
 *
 * Software reimplementation of the STM32G0 CRC peripheral as Lib/HAL wraps
 * it (Lib/HAL/Crc.cpp):
 *   Compute   -> CRC-16/CCITT-FALSE  poly 0x1021, init 0xFFFF, no reflection
 *   Compute32 -> CRC-32/MPEG-2       poly 0x04C11DB7, init 0xFFFFFFFF,
 *                                    no reflection, no final xor
 * Byte-at-a-time, MSB first -- matches CRC_INPUTDATA_FORMAT_BYTES with both
 * inversion modes disabled. Hal::Crc carries no per-instance state, so the
 * constructed Poly is implied by which Compute* the caller uses.
 *************************************************************/

#include "Crc.h"

using Hal::Crc;

Crc::Crc(const Poly)
{
}

uint16_t Crc::Compute(const uint8_t* const data, const size_t len)
{
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; i++)
    {
        crc ^= static_cast<uint16_t>(static_cast<uint16_t>(data[i]) << 8);
        for (int bit = 0; bit < 8; bit++)
        {
            crc = (crc & 0x8000u) ? static_cast<uint16_t>((crc << 1) ^ 0x1021u)
                                  : static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

uint32_t Crc::Compute32(const uint8_t* const data, const size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
    {
        crc ^= static_cast<uint32_t>(data[i]) << 24;
        for (int bit = 0; bit < 8; bit++)
        {
            crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : (crc << 1);
        }
    }
    return crc;
}
