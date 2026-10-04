/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

// Per-probe offset correction (TemperatureNode-Spec.md §4.3), added to every
// raw DS18B20 reading before it goes on the bus. Set over the bus through the
// SupplyTempOffset / ReturnTempOffset endpoints and kept in the runtime
// settings page (Board::Flash::SettingsBase), so it survives resets, OTA and
// re-provisioning.
//
// The page holds one small CRC-protected record. A blank or corrupt page reads
// as zero offsets. The page is only erased and rewritten when a value actually
// changes -- a re-sent identical Set costs no flash cycle.
class Calibration
{
  public:
    enum class Probe : uint8_t
    {
        Return = 0,
        Supply = 1,
    };

    // Accepted range, centi-degC: +-10.00 degC is far beyond any sensor error
    // worth correcting, so anything larger is a typo rather than a calibration.
    static constexpr int16_t MaxOffset = 1000;

    Calibration();

    void Load();

    int16_t Offset(const Probe probe) const;

    // False when the value is out of range or the flash write failed; the
    // stored and in-RAM offsets are then unchanged.
    bool SetOffset(const Probe probe, const int16_t centiDegC);

  private:
    bool Save(const int16_t (&values)[2]);

    int16_t offsets[2];
};
