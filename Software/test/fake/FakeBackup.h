/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

// Hal::Backup double: a small in-memory stand-in for the two backup
// registers this project uses (Hal::Backup::Reg). Reset() clears both back
// to 0, matching a power-on (not a warm reset, which is what these
// registers are meant to survive on real hardware -- tests that want to
// simulate a warm reset simply don't call Reset() between steps).
namespace FakeBackup
{
    void Reset();
} // namespace FakeBackup
