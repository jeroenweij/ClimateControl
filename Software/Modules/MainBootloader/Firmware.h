/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

#include "Crc.h"
#include "DelayTimer.h"
#include "Flash.h"
#include "Gpio.h"

#include "Frame.h"

namespace Boot
{
    // MainController's own bootloader-side OTA target: erases/programs/
    // verifies the MC's single application slot in response to the server's
    // Endpoint::OtaControl / Endpoint::OtaData uplink messages
    // (MainController-Server-Link-Spec.md §5, §8 step 7).
    //
    // Point-to-point over NINA (always-on full duplex via UplinkHandler, no
    // RS485 poll/Announce discipline), so unlike Modules/Bootloader/
    // FirmwareSlave.cpp (which the flash state machine below was ported
    // from) there is no "module" target, no Poll-gated reply queueing, and
    // no Discover/Announce: OnControl()/OnData() each yield at most one
    // reply, ready for UplinkHandler to pop and send the moment the call
    // returns.
    class Firmware
    {
      public:
        Firmware();

        void Init();

        // Heartbeat LEDs -- how you can tell the unit is in its bootloader:
        // the activity LED blinks (slowly while idle or bringing the uplink
        // up, fast while an image is being received); after a fault it stops
        // and the error LED blinks instead. Call from the super-loop.
        void Loop();

        void OnControl(const NodeLib::Message& m);
        void OnData(const NodeLib::Message& m);

        // Pops the reply queued by the OnControl()/OnData() call just made,
        // if any. At most one reply is ever pending at a time -- each
        // inbound message yields exactly one.
        bool PopReply(NodeLib::Message& out);

      private:
        enum class State : uint8_t
        {
            Idle      = 1,
            Erasing   = 2,
            Receiving = 3,
            Valid     = 4,
            Error     = 5,
        };

        enum class ControlOp : uint8_t
        {
            Begin    = 1,
            Abort    = 2,
            End      = 3,
            Activate = 4,
        };

        void HandleBegin(const NodeLib::Message& m);
        void HandleAbort();
        void HandleEnd();
        void HandleActivate();

        bool EraseAppSlot();

        // Reads back already-flashed bytes at 'offset' (a duplicate write --
        // the data landed, only its ack was lost) and queues an Ack from
        // that, without ever calling Hal::Flash::Program() again for them.
        void     AckFromFlash(const uint16_t offset, const uint8_t count);
        uint16_t ChunkCrc(const uint32_t address, const uint8_t count);

        void QueueDataReply(const bool nack, const uint16_t offset, const uint16_t chunkCrc16, const bool programFailed);
        void QueueControlReply(const bool nack, const uint8_t error);
        void QueueStatus();

        void Fault(const uint8_t error);
        // Fault() plus queuing this control op's own Nack -- the common case
        // for Begin/End's validation failures.
        void FaultControl(const uint8_t error);

        Hal::Crc crc; // CRC-16/CCITT, chunk integrity only -- re-init to CRC-32 in HandleEnd()
        State    state;
        uint8_t  lastError;
        uint32_t imageSize;
        uint32_t imageCrc32;
        uint16_t fwVersion;
        uint32_t expectedOffset;

        // How many bytes of the chunk currently AT expectedOffset are already
        // committed to flash. Zero except right after a genuine Hal::Flash::
        // Program() failure partway through a chunk -- see Firmware.cpp's
        // HandleData() comment for the full reasoning (ported verbatim from
        // FirmwareSlave.cpp's HandleWrite()).
        uint8_t partialCommitted;

        bool             replyPending;
        NodeLib::Message pendingReply;

        Hal::Gpio         activityLed;
        Hal::Gpio         errorLed;
        Tools::DelayTimer heartbeatTimer;
    };
} // namespace Boot
