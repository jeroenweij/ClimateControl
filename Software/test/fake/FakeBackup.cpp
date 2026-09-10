/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "Backup.h"

namespace
{
    uint32_t registers[2] = {0, 0};
} // namespace

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
