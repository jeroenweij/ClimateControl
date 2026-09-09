/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace Hal
{
    // Bare-metal STM32G031 embedded-flash writer: page erase + double-word
    // program, blocking. Used by the bus-resident bootloader to write the
    // application slot (Node-Flash-Layout-and-Bootloader-Spec.md §6).
    //
    // Single bank -- any program/erase stalls instruction fetch from flash
    // until it completes. That is fine for the bootloader (it does nothing else
    // meanwhile); the MainController's app-assisted self-update will need the
    // RAM-resident variant instead (spec §7.1).
    namespace Flash
    {
        constexpr uint32_t PageSize = 2048;

        void Unlock();
        void Lock();

        // Erase the 2 KB page that contains 'address'. False on a hardware error.
        bool ErasePage(const uint32_t address);

        // Program 'len' bytes at 'address' (both must be 8-byte aligned; a final
        // partial double-word is padded with 0xFF). The target range must have
        // been erased. False on a hardware error.
        bool Program(const uint32_t address, const uint8_t* const data, const size_t len);
    } // namespace Flash
} // namespace Hal
