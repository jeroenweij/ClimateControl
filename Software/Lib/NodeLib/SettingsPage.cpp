/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include <string.h>

#include "Crc.h"
#include "Flash.h"
#include "MemoryMap.h"

#include "SettingsPage.h"

namespace
{
    struct __attribute__((packed)) Record
    {
        uint32_t magic;
        uint8_t  payload[NodeLib::SettingsPage::PayloadSize];
        uint16_t crc16; // CRC-16/CCITT-FALSE over the bytes before it
    };
    static_assert(sizeof(Record) == 16, "Settings record must be 16 bytes");

    uint16_t RecordCrc(const Record& record)
    {
        Hal::Crc crc(Hal::Crc::Poly::Ccitt16);
        return crc.Compute(reinterpret_cast<const uint8_t*>(&record), sizeof(Record) - sizeof(uint16_t));
    }
} // namespace

bool NodeLib::SettingsPage::Load(const uint32_t magic, uint8_t* const payload, const size_t len)
{
    Record record;
    Hal::Flash::Read(Board::Flash::SettingsBase, reinterpret_cast<uint8_t*>(&record), sizeof(record));

    if (len > PayloadSize || record.magic != magic || record.crc16 != RecordCrc(record))
    {
        return false;
    }
    memcpy(payload, record.payload, len);
    return true;
}

bool NodeLib::SettingsPage::Save(const uint32_t magic, const uint8_t* const payload, const size_t len)
{
    if (len > PayloadSize)
    {
        return false;
    }

    Record record;
    memset(&record, 0xFF, sizeof(record));
    record.magic = magic;
    memcpy(record.payload, payload, len);
    record.crc16 = RecordCrc(record);

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
