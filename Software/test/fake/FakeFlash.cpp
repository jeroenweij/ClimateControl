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
    size_t  nextProgramLimit = SIZE_MAX;
} // namespace

namespace FakeFlash
{
    void Reset()
    {
        memset(buffer, 0xFF, sizeof(buffer));
        nextProgramLimit = SIZE_MAX;
    }

    const uint8_t* Data()
    {
        return buffer;
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
    const uint32_t offset = address - Board::Flash::AppBase;
    const uint32_t page   = (offset / PageSize) * PageSize;
    if (page >= sizeof(buffer))
    {
        return false;
    }
    const uint32_t len = (page + PageSize <= sizeof(buffer)) ? PageSize : static_cast<uint32_t>(sizeof(buffer) - page);
    memset(&buffer[page], 0xFF, len);
    return true;
}

size_t Hal::Flash::Program(const uint32_t address, const uint8_t* const data, const size_t len)
{
    const uint32_t offset = address - Board::Flash::AppBase;
    size_t         limit  = len;
    if (nextProgramLimit != SIZE_MAX)
    {
        limit            = nextProgramLimit < len ? nextProgramLimit : len;
        nextProgramLimit = SIZE_MAX;
    }
    // Real Program() only ever commits whole double-words -- mirror that so
    // a forced partial failure lands on the same kind of boundary.
    limit = (limit / 8) * 8;

    for (size_t i = 0; i < limit; i++)
    {
        buffer[offset + i] = data[i];
    }
    return limit;
}

void Hal::Flash::Read(const uint32_t address, uint8_t* const out, const size_t len)
{
    const uint32_t offset = address - Board::Flash::AppBase;
    for (size_t i = 0; i < len; i++)
    {
        out[i] = buffer[offset + i];
    }
}
