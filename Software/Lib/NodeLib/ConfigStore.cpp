/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include <stddef.h>

#include "MemoryMap.h"

#include "ConfigStore.h"
#include "Id.h" // NodeLib::MAX_NODES

using NodeLib::ConfigStore;

namespace
{
    constexpr uint32_t configMagic = 0x4E4F4443; // 'NODC'

    // Layout matches the provisioning tool -- see
    // Spec/Node-Flash-Layout-and-Bootloader-Spec.md Sec6.3.
    struct __attribute__((packed)) ConfigRecord
    {
        uint32_t magic;
        uint16_t schemaVersion;
        uint8_t  nodeId;
        uint8_t  module;
        uint8_t  settings[16];
        uint8_t  reserved[4];
        uint32_t crc32;
    };
    static_assert(sizeof(ConfigRecord) == 32, "ConfigRecord must be 32 bytes");

    const ConfigRecord& Record()
    {
        return *reinterpret_cast<const ConfigRecord*>(Board::Flash::ConfigBase);
    }

    // Standard reflected CRC-32 (zlib / PNG): poly 0xEDB88320, init 0xFFFFFFFF,
    // final XOR 0xFFFFFFFF. Table-less on purpose -- it runs once over 28 bytes
    // at boot, so a 256-entry table would cost more flash than it saves time.
    uint32_t Crc32(const uint8_t* const data, const size_t len)
    {
        uint32_t crc = 0xFFFFFFFFu;
        for (size_t i = 0; i < len; i++)
        {
            crc ^= data[i];
            for (int bit = 0; bit < 8; bit++)
            {
                if (crc & 1u)
                {
                    crc = (crc >> 1) ^ 0xEDB88320u;
                }
                else
                {
                    crc >>= 1;
                }
            }
        }
        return crc ^ 0xFFFFFFFFu;
    }
} // namespace

bool ConfigStore::Valid()
{
    const ConfigRecord& record = Record();

    if (record.magic != configMagic)
    {
        return false;
    }

    const uint32_t computed =
        Crc32(reinterpret_cast<const uint8_t*>(&record), sizeof(ConfigRecord) - sizeof(uint32_t));
    if (computed != record.crc32)
    {
        return false;
    }

    return record.nodeId >= 1 && record.nodeId < NodeLib::MAX_NODES;
}

uint8_t ConfigStore::NodeId()
{
    return Record().nodeId;
}

ConfigStore::Module ConfigStore::GetModule()
{
    return static_cast<Module>(Record().module);
}

const uint8_t* ConfigStore::Settings()
{
    return Record().settings;
}
