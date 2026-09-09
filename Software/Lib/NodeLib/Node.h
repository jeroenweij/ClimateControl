/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include "DelayTimer.h"

#include "ErrorHandler.h"
#include "Frame.h"
#include "INodeHandler.h"
#include "Id.h"

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
        Node(const uint32_t baudRate);

        void RegisterHandler(INodeHandler* handler);
        void QueueMessage(const Message& m);
        void QueueMessage(const Id& id, const uint8_t* const data, const uint8_t len);
        void QueueMessage(const Id& id, const uint8_t value);

        // The node's bus address is fixed at factory provisioning and read from
        // flash in Init() (ControllerNode / TemperatureNode); it is not settable
        // at runtime. NodeMaster overrides it to the reserved master id 0.
        uint8_t GetId();

        DiagCounters Counters() const;

        void Init();
        void Loop();

      protected:
        bool         ReadMessage(const Message& m);
        void         flushQueue();
        void         WriteMessage(const Message& m);
        void         ResetHearthBeat();
        virtual void HandleMasterMessage(const Message&) {}

        ErrorHandler         errorHandler;
        static const uint8_t masterNodeId = 0;
        static const int     nodeSpacing  = 25;
        // Hard cap for fixed-size arrays (e.g. NodeMaster::activeNodes) -- one
        // definition, in Id.h, shared with ConfigStore.
        static const uint8_t maxNodes  = MAX_NODES;
        static const int     queueSize = 25;

        INodeHandler* handler;
        uint8_t       nodeId;
        int           messagesQueued;

      private:
        void HandlePollRequest();
        void HandleMessage(const Message& m);
        void HandleInternalMessage(const Message& m);

        uint32_t baudRate;

        uint32_t txFrames;
        uint32_t queueDrops;

        Hal::Uart         uart;
        Hal::Crc          crc;
        Frame             frame;
        Hal::Gpio         led;
        Message           messageQueue[queueSize];
        Tools::DelayTimer hearthBeatTimer;
    };
} // namespace NodeLib
