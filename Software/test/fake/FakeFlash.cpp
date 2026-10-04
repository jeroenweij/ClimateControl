/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include <limits.h>
#include <string.h>

#include "Flash.h"
#include "MemoryMap.h"

#include "FakeFlash.h"

namespace
{
    uint8_t buffer[Board::Flash::AppSize];
    uint8_t settings[Board::Flash::SettingsSize];
    size_t  nextProgramLimit = SIZE_MAX;
    int     eraseCount       = 0;

    // The fake buffer backing 'address', and how many bytes remain in it from
    // there; null for an address outside both regions.
    uint8_t* Locate(const uint32_t address, size_t& room)
    {
        if (address >= Board::Flash::AppBase && address < Board::Flash::AppBase + Board::Flash::AppSize)
        {
            room = Board::Flash::AppBase + Board::Flash::AppSize - address;
            return &buffer[address - Board::Flash::AppBase];
        }
        if (address >= Board::Flash::SettingsBase && address < Board::Flash::SettingsBase + Board::Flash::SettingsSize)
        {
            room = Board::Flash::SettingsBase + Board::Flash::SettingsSize - address;
            return &settings[address - Board::Flash::SettingsBase];
        }
        room = 0;
        return nullptr;
    }
} // namespace

namespace FakeFlash
{
    void Reset()
    {
        memset(buffer, 0xFF, sizeof(buffer));
        memset(settings, 0xFF, sizeof(settings));
        nextProgramLimit = SIZE_MAX;
        eraseCount       = 0;
    }

    const uint8_t* Data()
    {
        return buffer;
    }

    uint8_t* Settings()
    {
        return settings;
    }

    int EraseCount()
    {
        return eraseCount;
    }

    void SetNextProgramLimit(const size_t bytes)
    {
        nextProgramLimit = bytes;
    }
} // namespace FakeFlash

void Hal::Flash::Unlock()
{
}

void Hal::Flash::Lock()
{
}

bool Hal::Flash::ErasePage(const uint32_t address)
{
    const uint32_t pageAddress = address - ((address - Board::Flash::Base) % PageSize);
    size_t         room        = 0;
    uint8_t* const page        = Locate(pageAddress, room);
    if (page == nullptr)
    {
        return false;
    }
    memset(page, 0xFF, room < PageSize ? room : PageSize);
    eraseCount++;
    return true;
}

size_t Hal::Flash::Program(const uint32_t address, const uint8_t* const data, const size_t len)
{
    size_t         room = 0;
    uint8_t* const dst  = Locate(address, room);
    if (dst == nullptr)
    {
        return 0;
    }
    size_t limit = len < room ? len : room;
    if (nextProgramLimit != SIZE_MAX)
    {
        limit            = nextProgramLimit < limit ? nextProgramLimit : limit;
        nextProgramLimit = SIZE_MAX;
    }
    // Real Program() only ever commits whole double-words -- mirror that so
    // a forced partial failure lands on the same kind of boundary.
    if (limit < len)
    {
        limit = (limit / 8) * 8;
    }

    for (size_t i = 0; i < limit; i++)
    {
        dst[i] = data[i];
    }
    return limit;
}

void Hal::Flash::Read(const uint32_t address, uint8_t* const out, const size_t len)
{
    size_t               room = 0;
    const uint8_t* const src  = Locate(address, room);
    for (size_t i = 0; i < len; i++)
    {
        out[i] = (src != nullptr && i < room) ? src[i] : 0xFF;
    }
}
