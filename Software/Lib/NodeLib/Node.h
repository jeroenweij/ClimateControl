/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <optional>

#include "DelayTimer.h"

#include "ErrorHandler.h"
#include "Frame.h"
#include "IVariableHandler.h"
#include "Id.h"

namespace NodeLib
{
    class Node
    {
      public:
        // numNodes is a constructor parameter rather than a compile-time constant,
        // per RS485-Node-Protocol-Spec-STM32G030.md §8 migration notes ("cheap to
        // generalize now while you're rewriting anyway").
        //
        // Note: this constructor builds Hal::Gpio/Hal::Crc members, which touch
        // peripheral registers immediately -- Hal::System::Init() must already
        // have run before a Node is constructed. In practice that means
        // constructing Node inside main() after Hal::System::Init(), not as a
        // file-scope global (C++ static-init order isn't guaranteed relative to
        // HAL bring-up). UART itself is deferred to Init() below, matching how
        // the original AVR version deferred Serial1.begin() there too.
        Node(const uint8_t                 numNodes,
             const Hal::UartPins&          uartPins,
             const uint32_t                baudRate,
             const Hal::Pin                ledPin,
             const Hal::Pin                errorLedPin,
             const std::optional<Hal::Pin> buttonPin = std::nullopt);

        void RegisterHandler(IVariableHandler* handler);
        void QueueMessage(const Message& m);
        void QueueMessage(const Id& id, const uint8_t* const data, const uint8_t len);
        void QueueMessage(const Id& id, const uint8_t value);

        void    SetId(const uint8_t newId);
        uint8_t GetId();

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
        // Hard cap for fixed-size arrays (e.g. NodeMaster::activeNodes) -- matches
        // Node-Bus-Hardware-Design-Spec.md §1 ("up to 20 slave nodes"). numNodes
        // below is the runtime-configured *actual* node count for a given
        // deployment (must be <= maxNodes), per protocol spec §8 migration notes.
        static const uint8_t maxNodes = 20;
        const uint8_t        numNodes;
        static const int     queueSize = 25;

        IVariableHandler* handler;
        uint8_t           nodeId;
        int               messagesQueued;

      private:
        void HandlePollRequest();
        void HandleMessage(const Message& m);
        void HandleInternalMessage(const Message& m);

        Hal::UartPins uartPins;
        uint32_t      baudRate;

        Hal::Uart         uart;
        Hal::Crc          crc;
        Frame             frame;
        Hal::Gpio         led;
        Message           messageQueue[queueSize];
        Tools::DelayTimer hearthBeatTimer;
    };
} // namespace NodeLib
