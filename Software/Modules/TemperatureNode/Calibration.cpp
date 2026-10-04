/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include <string.h>

#include "Logger.h"
#include "SettingsPage.h"

#include "Calibration.h"

namespace Settings = NodeLib::SettingsPage;

namespace
{
    constexpr uint32_t RecordMagic = 0x4C414354; // 'TCAL'

    size_t Index(const Calibration::Probe probe)
    {
        return static_cast<size_t>(probe);
    }
} // namespace

Calibration::Calibration() :
    offsets{0, 0}
{
}

void Calibration::Load()
{
    int16_t stored[2];
    if (!Settings::Load(RecordMagic, reinterpret_cast<uint8_t*>(stored), sizeof(stored)))
    {
        offsets[0] = 0;
        offsets[1] = 0;
        return;
    }

    offsets[0] = stored[0];
    offsets[1] = stored[1];
    LOG_INFO("Probe offsets: return " << offsets[0] << ", supply " << offsets[1] << " centi-degC");
}

int16_t Calibration::Offset(const Probe probe) const
{
    return offsets[Index(probe)];
}

bool Calibration::SetOffset(const Probe probe, const int16_t centiDegC)
{
    if (centiDegC > MaxOffset || centiDegC < -MaxOffset)
    {
        return false;
    }
    if (offsets[Index(probe)] == centiDegC)
    {
        return true; // unchanged -- no flash cycle
    }

    int16_t next[2]    = {offsets[0], offsets[1]};
    next[Index(probe)] = centiDegC;
    if (!Settings::Save(RecordMagic, reinterpret_cast<const uint8_t*>(next), sizeof(next)))
    {
        LOG_ERROR("Probe offset write failed");
        return false;
    }

    offsets[0] = next[0];
    offsets[1] = next[1];
    return true;
}
