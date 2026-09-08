/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

// Single source of truth for the STM32G031F8P6 flash partition map.
// See Spec/Node-Flash-Layout-and-Bootloader-Spec.md Sec3.
//
//   0x0800_0000  Bootloader    8 KB  (4 pages)  -- SWD-flashed only, never OTA
//   0x0800_2000  Application   52 KB (26 pages) -- OTA target; vectors at base,
//                                                 image descriptor at +0xC0
//   0x0800_F000  Config A      2 KB  (1 page)   -- factory-written node identity,
//                                                 read-only to firmware
//   0x0800_F800  Config B      2 KB  (1 page)   -- reserved (future runtime setting)

namespace Board
{
    namespace Flash
    {
        constexpr uint32_t Base     = 0x08000000;
        constexpr uint32_t Size     = 64 * 1024;
        constexpr uint32_t PageSize = 2 * 1024;

        constexpr uint32_t BootloaderBase = 0x08000000;
        constexpr uint32_t BootloaderSize = 8 * 1024;

        constexpr uint32_t AppBase             = 0x08002000;
        constexpr uint32_t AppSize             = 52 * 1024;
        constexpr uint32_t AppDescriptorOffset = 0xC0;

        // Factory-provisioned identity record lives at the base of Config A.
        constexpr uint32_t ConfigBase = 0x0800F000;
        constexpr uint32_t ConfigSize = 2 * 1024;

        constexpr uint32_t ConfigReservedBase = 0x0800F800;
        constexpr uint32_t ConfigReservedSize = 2 * 1024;
    } // namespace Flash

    // Written to Hal::Backup::Reg::Boot by a running app to ask the bootloader to
    // stay resident after the next warm reset (Node-Flash-Layout-and-Bootloader-
    // Spec.md §5). Bootloader consumes and clears it.
    constexpr uint32_t EnterBootloaderMagic = 0xB007C0DE;
} // namespace Board
