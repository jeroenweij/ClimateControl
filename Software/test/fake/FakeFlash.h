/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stddef.h>
#include <stdint.h>

// Hal::Flash double: host-side buffers standing in for the app flash slot
// (Board::Flash::AppBase .. +AppSize) and the runtime settings page
// (Board::Flash::SettingsBase .. +SettingsSize), so Modules/Bootloader/
// FirmwareSlave.cpp and TemperatureNode's Calibration can be exercised without
// touching a real MCU address. Erase fills with 0xFF (matching real NOR
// flash's erased state); Program()/Read() pick the buffer by address.
namespace FakeFlash
{
    void Reset(); // fills both regions with 0xFF, as if freshly erased

    const uint8_t* Data(); // AppSize bytes, for tests to inspect directly
    uint8_t*       Settings(); // SettingsSize bytes, writable so tests can corrupt a record

    // Page erases since the last Reset() -- lets a test assert that an
    // unchanged setting costs no flash cycle.
    int EraseCount();

    // The next Program() call commits at most 'bytes' before returning early
    // (simulating a genuine Hal::Flash::Program() hardware failure partway
    // through) -- consumed by that one call, unlimited again afterward.
    void SetNextProgramLimit(const size_t bytes);
} // namespace FakeFlash
