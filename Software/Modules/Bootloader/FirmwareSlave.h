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

#include "OtaUart.h"

namespace Boot
{
    // The minimal RS485 slave the bootloader runs while resident on a
    // provisioned node: NodeLib framing only (no NodeMaster, no Logger), just
    // enough to serve Endpoint::Firmware and the Transport plumbing so the
    // master can push a new application image
    // (Node-Flash-Layout-and-Bootloader-Spec.md §6).
    //
    // Bus discipline is identical to a normal slave: answer Discover with
    // Announce, transmit only when polled. The transfer is strictly sequential
    // and every Write chunk (32 bytes, 4 double-words) is programmed to flash
    // immediately on arrival -- no RAM page buffer (Node-Flash-Layout-and-
    // Bootloader-Spec.md §6.2.1, v2).
    class FirmwareSlave
    {
      public:
        FirmwareSlave(const uint8_t nodeId, const uint8_t module);

        void Init();
        void Loop();

      private:
        // One queued Write reply (see writeReplies below).
        struct SWriteReply
        {
            bool     nack;
            bool     programFailed;
            uint16_t offset;
            uint16_t crc16;
        };

        // Reported in the Status message (data[1]); mirrors the spec's state
        // enum (0 = app, which the bootloader never reports).
        enum class State : uint8_t
        {
            Idle      = 1,
            Erasing   = 2,
            Receiving = 3,
            Valid     = 4,
            Error     = 5,
        };

        void OnMessage(const NodeLib::Message& m);
        void OnFirmware(const NodeLib::Message& m);

        void HandleBegin(const NodeLib::Message& m);
        void HandleWrite(const NodeLib::Message& m);
        void StageWrite(const NodeLib::Message& m);
        void CommitStagedWrites();
        void HandleEnd();
        void HandleActivate();
        void HandleAbort();

        bool EraseAppSlot();

        // Queues a Write reply (Ack or Nack) for the next Poll -- delivery
        // timing is unchanged from a Status Report, only the content differs
        // (Node-Flash-Layout-and-Bootloader-Spec.md §6.2.1 "Bus scheduling").
        void QueueWriteReply(const bool nack, const uint16_t offset, const uint16_t chunkCrc16, const bool programFailed);
        // Reads back already-flashed bytes at 'offset' (a duplicate write --
        // the data landed, only its ack was lost) and queues an Ack from that,
        // without ever calling Hal::Flash::Program() again for them.
        void     AckFromFlash(const uint16_t offset, const uint8_t count);
        uint16_t ChunkCrc(const uint32_t address, const uint8_t count);

        // Same correlation benefit as QueueWriteReply(), for the three ops
        // that happen once per job rather than ~2000 times
        // (Node-Flash-Layout-and-Bootloader-Spec.md §8 item 9): Begin/End/
        // Abort reply with Ack/Nack on Endpoint::Firmware, data[0] = lastError
        // (0 on Ack), instead of the generic Status Report.
        void QueueOpReply(const bool nack, const uint8_t error);
        void SendOpReply();

        void SendAnnounce();
        void SendStatus();
        void SendWriteReply(const SWriteReply& reply);
        void SendDone();
        void SendFrame(const NodeLib::Message& m);
        void Fault(const uint8_t error);
        // Fault() plus queuing this op's own Nack -- the common case for
        // Begin/End's validation failures.
        void FaultOp(const uint8_t error);
        void Heartbeat();

        OtaUart        uart;
        Hal::Crc       crc; // CRC-16/CCITT for the frame CRC; re-init to CRC-32 at End
        NodeLib::Frame frame; // RX parser only -- TX is SendFrame()

        const uint8_t nodeId;
        const uint8_t module;

        State    state;
        uint8_t  lastError;
        uint32_t imageSize;
        uint32_t imageCrc32;
        uint16_t fwVersion;
        uint32_t expectedOffset;

        // How many bytes of the chunk currently AT expectedOffset are already
        // committed to flash. Zero except right after a genuine Hal::Flash::
        // Program() failure partway through a chunk (a lost ack never leaves
        // this nonzero -- expectedOffset only advances past a chunk once it
        // fully committed, so a lost-ack resend always arrives as offset <
        // expectedOffset instead, handled by AckFromFlash() without touching
        // this). Lets a retry of the same offset resume from the first
        // not-yet-programmed double-word instead of re-programming ones
        // already locked in (STM32G0 PROGERR on any second write to an
        // already-programmed double-word, even identical data).
        uint8_t partialCommitted;

        bool statusPending;

        // Write replies waiting for the next Poll, oldest first. The master
        // keeps several chunks in flight (a window), so several Writes can
        // arrive between two Polls -- each gets its own Ack/Nack, sent in
        // order on the next Poll. Sized above the master's window; when full,
        // the oldest is dropped (acks are cumulative, so the newest carries
        // the most).
        // Writes received since the last Poll, programmed only when the Poll
        // arrives (CommitStagedWrites()). Programming flash stalls the CPU --
        // the code runs from flash -- and a windowed master sends its next
        // chunks back to back, so programming each chunk on arrival loses
        // bytes of the one behind it to a USART overrun. The master is quiet
        // from its Poll until our Done, so the Poll is the safe moment.
        static const uint8_t maxStagedWrites = 8;
        NodeLib::Message     stagedWrites[maxStagedWrites];
        uint8_t              stagedCount;

        static const uint8_t maxWriteReplies = 8;
        SWriteReply          writeReplies[maxWriteReplies];
        uint8_t              writeReplyCount;

        // Pending reply for whichever of Begin/End/Abort was last handled.
        bool    opReplyPending;
        bool    opReplyNack;
        uint8_t opReplyError;

        Hal::Gpio         activityLed;
        Hal::Gpio         errorLed;
        Tools::DelayTimer heartbeatTimer;
    };
} // namespace Boot
