/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include "SStream.h"

#include <stdint.h>

namespace NodeLib
{
    // data[0] sub-opcode on Endpoint::Firmware (Node-Message-Model-Spec.md §4,
    // Node-Flash-Layout-and-Bootloader-Spec.md §6.2). The Begin/Write/End/
    // Activate transfer runs in the bootloader; a running app only ever acts on
    // EnterBootloader (park outputs, set the backup-register magic, reset).
    enum class FirmwareOp : uint8_t
    {
        Begin           = 0x01,
        Write           = 0x02,
        End             = 0x03,
        Activate        = 0x04,
        Abort           = 0x05,
        EnterBootloader = 0x06,
        Status          = 0x07,
    };

    inline std::stringstream& operator<<(std::stringstream& oStrStream, const FirmwareOp op)
    {
        switch (op)
        {
            case FirmwareOp::Begin:
                oStrStream << "Begin";
                break;
            case FirmwareOp::Write:
                oStrStream << "Write";
                break;
            case FirmwareOp::End:
                oStrStream << "End";
                break;
            case FirmwareOp::Activate:
                oStrStream << "Activate";
                break;
            case FirmwareOp::Abort:
                oStrStream << "Abort";
                break;
            case FirmwareOp::EnterBootloader:
                oStrStream << "EnterBootloader";
                break;
            case FirmwareOp::Status:
                oStrStream << "Status";
                break;
        }

        return oStrStream;
    }
} // namespace NodeLib
