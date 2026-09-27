/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "Backup.h"

#include "FakeBackup.h"

namespace
{
    uint32_t registers[5] = {};
} // namespace

namespace FakeBackup
{
    void Reset()
    {
        for (uint32_t& value : registers)
        {
            value = 0;
        }
    }
} // namespace FakeBackup

namespace Hal
{
    namespace Backup
    {
        uint32_t Read(const Reg reg)
        {
            return registers[static_cast<int>(reg)];
        }

        void Write(const Reg reg, const uint32_t value)
        {
            registers[static_cast<int>(reg)] = value;
        }
    } // namespace Backup
} // namespace Hal
