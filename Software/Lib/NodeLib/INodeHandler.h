/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include "Id.h"

#include <stdint.h>

namespace NodeLib
{
    // App-contributed fields of the SystemStatus endpoint (0x11). NodeLib fills
    // uptime / resetCause / fwVersion itself.
    struct SystemStatus
    {
        uint8_t  state; // 0 = running / ok; app-defined otherwise
        uint16_t errorFlags; // app-defined bits
    };

    // Implemented by each module's firmware and registered with Node.
    // See Spec/Node-Message-Model-Spec.md §6.2.
    class INodeHandler
    {
      public:
        // Application + Room endpoints addressed to this node. Transport /
        // System* / Firmware / Diagnostics* are handled inside NodeLib and never
        // reach here.
        virtual void ReceivedMessage(const Message& message) = 0;

        // Transport liveness: no full poll round completed within the timeout.
        virtual void ConnectionLost() = 0;

        // Optional hooks -- NodeLib calls these while servicing the intercepted
        // blocks. Default no-op so a module only overrides what it needs.
        virtual void PrepareForReset() {} // park outputs before an OTA / commanded reset
        virtual void FillStatus(SystemStatus&) {} // contribute app-specific status bits
    };
} // namespace NodeLib
