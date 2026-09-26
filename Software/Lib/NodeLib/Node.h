/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include "DelayTimer.h"

#include "ErrorHandler.h"
#include "Frame.h"
#include "INodeHandler.h"
#include "Id.h"
#include "Publisher.h"

namespace NodeLib
{
    // Bus-health snapshot surfaced on Endpoint::DiagRxCounters / DiagTxCounters
    // (Spec/Node-Message-Model-Spec.md §3).
    struct DiagCounters
    {
        RxCounters rx;
        uint32_t   txFrames;
        uint32_t   queueDrops;
    };

    class Node
    {
      public:
        // Pin assignment is not configurable -- it is fixed by
        // Lib/Board/BoardPins.h. numNodes stays a runtime parameter rather than a
        // compile-time constant, per RS485-Node-Protocol-Spec-STM32G030.md §8
        // migration notes ("cheap to generalize now while you're rewriting anyway").
        //
        // Note: this constructor builds Hal::Gpio/Hal::Crc members, which touch
        // peripheral registers immediately -- Hal::System::Init() must already
        // have run before a Node is constructed. In practice that means
        // constructing Node inside main() after Hal::System::Init(), not as a
        // file-scope global (C++ static-init order isn't guaranteed relative to
        // HAL bring-up). UART itself is deferred to Init() below, matching how
        // the original AVR version deferred Serial1.begin() there too.
        //
        // The 1-arg form binds the RS485 main bus (USART1 / Board::BusUart). The
        // 3-arg form lets a ControllerNode also drive USART2 for its Thermostat
        // link (ControllerNode-Thermostat-Link-Spec.md §5).
        Node();
        Node(const Hal::Uart::Instance instance, const Hal::UartPins& pins);
        // announceSpacingOverride: skip the nodeId-based stagger below and
        // always wait this many ms before replying to a poll. Thermostat's
        // link is a fixed point-to-point pair (ControllerNode-Thermostat-Link-
        // Spec.md) with no other node to collide with, so it passes 0.
        Node(const Hal::Uart::Instance instance, const Hal::UartPins& pins, const int32_t announceSpacingOverride);

        void RegisterHandler(INodeHandler* handler);
        void QueueMessage(const Message& m);
        void QueueMessage(const Id& id, const uint8_t* const data, const uint8_t len);
        void QueueMessage(const Id& id, const uint8_t value);

        // Change-driven reporting (Node-Message-Model-Spec.md §6.1, see
        // Publisher): declare each endpoint this node reports by itself once,
        // then hand in its current value every loop. Loop() sends what's due
        // -- on change, as a staggered keepalive, and again after a lost
        // master -- and only while the master is polling. ClearPublished()
        // stops reporting a value that is no longer known.
        bool AddPublished(const Endpoint endpoint, const uint8_t size, const uint16_t minChange = 1);
        void PublishIfChanged(const Endpoint endpoint, const int32_t value);
        void ClearPublished(const Endpoint endpoint);

        // The node's bus address is fixed at factory provisioning and read from
        // flash in Init() (ControllerNode / TemperatureNode); it is not settable
        // at runtime. NodeMaster overrides it to the reserved master id 0.
        uint8_t GetId();

        DiagCounters Counters() const;

        void Init();
        void Loop();

      protected:
        bool ReadMessage(const Message& m);
        void flushQueue();
        void WriteMessage(const Message& m);
        void ResetHearthBeat();
        // Drain the UART through the framer, dispatching each decoded message.
        // The shared inner loop of Node::Loop() and LinkMaster::Loop().
        void         PumpRx();
        virtual void HandleMasterMessage(const Message&) {}

        ErrorHandler         errorHandler;
        static const uint8_t masterNodeId = 0;
        static const int     nodeSpacing  = 10;
        // Hard cap for fixed-size arrays (e.g. NodeMaster::activeNodes) -- one
        // definition, in Id.h, shared with ConfigStore.
        static const uint8_t maxNodes  = MAX_NODES;
        static const int     queueSize = 30;

        INodeHandler* handler;
        uint8_t       nodeId;
        int           messagesQueued;

      private:
        void HandleDiscoverRequest();
        void HandleMessage(const Message& m);
        void HandleInternalMessage(const Message& m);

        // NodeLib-owned endpoint blocks -- serviced here, never handed to the
        // application handler (Node-Message-Model-Spec.md §3/§6).
        void HandleSystemMessage(const Message& m);
        void HandleFirmwareMessage(const Message& m);
        void HandleDiagnosticsMessage(const Message& m);

        void SendReport(const Endpoint endpoint, const uint8_t* const data, const uint8_t len);
        void SendStatus(const SystemStatus& status);
        void ReportStatusIfChanged();
        // Error LED on while this slave isn't being polled (never yet since
        // boot, or the heartbeat ran out), off while it is. Pin touched only
        // on a change.
        void ShowBusState(const bool up);
        void SendAck(const Message& m);
        void SendNack(const Message& m);

        void              RequestReset(const bool toBootloader);
        [[noreturn]] void PerformPendingReset();
        void              StartIdentify(uint8_t seconds);
        void              ServiceIdentify();

        Hal::Uart::Instance busInstance;
        const Hal::UartPins busPins;

        uint32_t txFrames;
        uint32_t queueDrops;

        // -1 = auto, stagger by (nodeId - 1) * nodeSpacing ms; nodeId isn't
        // known until Init() runs, so this can't be resolved at construction.
        const int32_t announceSpacingOverride;

        bool resetPending;
        bool resetToBootloader;
        bool identifyLedOn;

        // Last SystemStatus state/errorFlags pushed unsolicited, so a fault
        // shows up upstream when it happens rather than on the next Get.
        bool     statusReported;
        uint8_t  reportedState;
        uint16_t reportedErrorFlags;

        bool busShown; // ShowBusState() has driven the LED at least once
        bool busUp;

        Publisher publisher;

        Hal::Uart         uart;
        Hal::Crc          crc;
        Frame             frame;
        Hal::Gpio         led;
        Message           messageQueue[queueSize];
        Tools::DelayTimer hearthBeatTimer;
        Tools::DelayTimer identifyUntil;
        Tools::DelayTimer identifyToggle;
    };
} // namespace NodeLib
