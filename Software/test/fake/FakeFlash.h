/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stddef.h>
#include <stdint.h>

// Hal::Flash double: a host-side buffer standing in for the app flash slot
// (Board::Flash::AppBase .. +AppSize), so Modules/Bootloader/FirmwareSlave.cpp
// can be exercised without touching a real MCU address. Erase fills with
// 0xFF (matching real NOR flash's erased state); Program()/Read() index into
// the buffer by address - Board::Flash::AppBase.
namespace FakeFlash
{
    void Reset(); // fills the whole slot with 0xFF, as if freshly erased

    const uint8_t* Data(); // AppSize bytes, for tests to inspect directly

    // The next Program() call commits at most 'bytes' before returning early
    // (simulating a genuine Hal::Flash::Program() hardware failure partway
    // through) -- consumed by that one call, unlimited again afterward.
    void SetNextProgramLimit(const size_t bytes);
} // namespace FakeFlash
