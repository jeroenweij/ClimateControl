/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

namespace Hal
{
    // The last fault that reset this node, kept in the backup registers
    // (Hal::Backup::Reg::FaultInfo/FaultUptime) so it survives the warm reset
    // it caused and can be reported afterwards (Endpoint::DiagLastError). Like
    // every backup register it is lost on a power cycle.
    //
    // HardFault_Handler (FaultHandler.cpp) records a HardFault and resets
    // instead of parking the CPU; an IWDG reset (Hal::Watchdog) is turned into
    // a record by Init() on the next boot.
    namespace Fault
    {
        enum class Code : uint8_t
        {
            None      = 0,
            HardFault = 1, // context: faulting PC - FLASH_BASE
            Watchdog  = 2, // context: 0; uptime: at the last Watchdog::Feed()
        };

        struct Record
        {
            Code     code;
            uint32_t uptimeMs;
            uint16_t context;
        };

        // Application side, once, first thing after Hal::System::Init():
        // latches this boot's reset cause (Hal::System::ResetCause()) and, if
        // it was the IWDG, records a Code::Watchdog fault.
        void Init();

        Record Last();
        void   Save(Code code, uint32_t uptimeMs, uint16_t context);
        void   Clear();
    } // namespace Fault
} // namespace Hal
