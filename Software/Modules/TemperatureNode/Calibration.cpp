/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include <string.h>

#include "Crc.h"
#include "Flash.h"
#include "Logger.h"
#include "MemoryMap.h"

#include "Calibration.h"

namespace
{
    constexpr uint32_t RecordMagic = 0x4C414354; // 'TCAL'

    // 16 bytes -- two whole flash double-words.
    struct __attribute__((packed)) Record
    {
        uint32_t magic;
        int16_t  offsets[2]; // indexed by Calibration::Probe
        uint8_t  reserved[6];
        uint16_t crc16; // CRC-16/CCITT-FALSE over the bytes before it
    };
    static_assert(sizeof(Record) == 16, "Calibration record must be 16 bytes");

    uint16_t RecordCrc(const Record& record)
    {
        Hal::Crc crc(Hal::Crc::Poly::Ccitt16);
        return crc.Compute(reinterpret_cast<const uint8_t*>(&record), sizeof(Record) - sizeof(uint16_t));
    }

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
    Record record;
    Hal::Flash::Read(Board::Flash::SettingsBase, reinterpret_cast<uint8_t*>(&record), sizeof(record));

    if (record.magic != RecordMagic || record.crc16 != RecordCrc(record))
    {
        offsets[0] = 0;
        offsets[1] = 0;
        return;
    }

    offsets[0] = record.offsets[0];
    offsets[1] = record.offsets[1];
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
    if (!Save(next))
    {
        LOG_ERROR("Probe offset write failed");
        return false;
    }

    offsets[0] = next[0];
    offsets[1] = next[1];
    return true;
}

bool Calibration::Save(const int16_t (&values)[2])
{
    Record record;
    memset(&record, 0xFF, sizeof(record));
    record.magic      = RecordMagic;
    record.offsets[0] = values[0];
    record.offsets[1] = values[1];
    record.crc16      = RecordCrc(record);

    // Single-bank flash: the erase stalls the core (and so bus service) for
    // tens of milliseconds. Acceptable for a calibration that changes a
    // handful of times in the node's life.
    Hal::Flash::Unlock();
    bool ok = Hal::Flash::ErasePage(Board::Flash::SettingsBase);
    if (ok)
    {
        ok = Hal::Flash::Program(Board::Flash::SettingsBase, reinterpret_cast<const uint8_t*>(&record), sizeof(record)) ==
            sizeof(record);
    }
    Hal::Flash::Lock();
    return ok;
}
