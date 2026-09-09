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
    // -- one 2 KB page buffered at a time.
    class FirmwareSlave
    {
      public:
        FirmwareSlave(const uint32_t baudRate, const uint8_t nodeId, const uint8_t module);

        void Init();
        void Loop();

      private:
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
        void HandleEnd();
        void HandleActivate();
        void HandleAbort();

        bool EraseAppSlot();
        bool AppendImageBytes(const uint8_t* const bytes, const uint8_t count);
        bool FlushPage();

        void SendAnnounce();
        void SendStatus();
        void SendDone();
        void SendFrame(const NodeLib::Message& m);
        void Fault(const uint8_t error);
        void Heartbeat();

        OtaUart        uart;
        Hal::Crc       crc; // CRC-16/CCITT for the frame CRC; re-init to CRC-32 at End
        NodeLib::Frame frame; // RX parser only -- TX is SendFrame()

        const uint32_t baudRate;
        const uint8_t  nodeId;
        const uint8_t  module;

        State    state;
        uint8_t  lastError;
        uint32_t imageSize;
        uint32_t imageCrc32;
        uint16_t fwVersion;
        uint32_t expectedOffset;

        uint32_t pageBase; // flash address the staging buffer currently maps to
        uint32_t pageLen; // bytes staged (the buffer itself is a file-scope
                          // static in the .cpp -- one node, one transfer)

        bool statusPending;

        Hal::Gpio         led;
        Tools::DelayTimer heartbeatTimer;
    };
} // namespace Boot
