/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "System.h"

namespace Hal
{
    namespace System
    {
        void Init()
        {
        }

        void SetVectorTable(const uint32_t)
        {
        }

        void JumpToApplication(const uint32_t)
        {
            while (true)
            {
            }
        }

        void Reset()
        {
            while (true)
            {
            }
        }

        uint8_t ResetCause()
        {
            return 0;
        }
    } // namespace System
} // namespace Hal
