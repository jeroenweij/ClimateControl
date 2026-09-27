/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "Backup.h"

#include "Fault.h"

// Record storage only -- no ST headers, so the host tests build this file as
// is. The HardFault handler and Init() live in FaultHandler.cpp.

Hal::Fault::Record Hal::Fault::Last()
{
    const uint32_t info = Hal::Backup::Read(Hal::Backup::Reg::FaultInfo);

    Record record;
    record.code     = static_cast<Code>(info >> 16);
    record.context  = static_cast<uint16_t>(info);
    record.uptimeMs = Hal::Backup::Read(Hal::Backup::Reg::FaultUptime);
    return record;
}

void Hal::Fault::Save(const Code code, const uint32_t uptimeMs, const uint16_t context)
{
    Hal::Backup::Write(Hal::Backup::Reg::FaultUptime, uptimeMs);
    Hal::Backup::Write(Hal::Backup::Reg::FaultInfo, (static_cast<uint32_t>(code) << 16) | context);
}

void Hal::Fault::Clear()
{
    Hal::Backup::Write(Hal::Backup::Reg::FaultInfo, 0);
    Hal::Backup::Write(Hal::Backup::Reg::FaultUptime, 0);
}
