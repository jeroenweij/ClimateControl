/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include "Node.h"

namespace NodeLib
{
    class NodeMaster : public Node
    {
      public:
        NodeMaster(const uint8_t                 numNodes,
                   const Hal::UartPins&          uartPins,
                   const uint32_t                baudRate,
                   const Hal::Pin                ledPin,
                   const Hal::Pin                errorLedPin,
                   const std::optional<Hal::Pin> buttonPin = std::nullopt);

        void Init(const uint8_t expectedNumNodes);
        void Loop();
        void FlushNow(const bool force = false);
        void StartPollingNodes();

      private:
        void          DetectNodes();
        const uint8_t ActiveNodeCount() const;
        void          PollNextNode(const int prevNodeId);
        void          HandleMasterMessage(const Message& m) override;
        void          NodeHello(int nodeId);
        void          HandleInternalOperation(const Message& m);

        // Sized at the compile-time maxNodes cap, not the runtime numNodes -- see
        // Node.h's comment on that split.
        bool activeNodes[maxNodes];
        bool nodesFound;
    };
} // namespace NodeLib
