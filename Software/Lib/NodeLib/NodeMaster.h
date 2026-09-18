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
        NodeMaster(const uint32_t baudRate);

        void Init();
        void Loop();
        void FlushNow(const bool force = false);
        void StartPollingNodes();

        // Module type reported in the node's Announce (0 = not seen / unknown;
        // values match NodeLib::ConfigStore::Module). nodeId is 1..maxNodes.
        uint8_t NodeModule(const uint8_t nodeId) const;

        // Nodes currently believed active (seen an Announce). Used e.g. for the
        // uplink's UplinkHello.nodeCount (MainController-Server-Link-Spec.md §5).
        const uint8_t ActiveNodeCount() const;

      private:
        void DetectNodes();
        void PollNextNode(const int prevNodeId);
        void HandleMasterMessage(const Message& m) override;
        void NodeHello(int nodeId, uint8_t module);
        void HandleInternalOperation(const Message& m);

        // Sized at the compile-time maxNodes cap, not the runtime numNodes -- see
        // Node.h's comment on that split.
        bool    activeNodes[maxNodes];
        uint8_t nodeModules[maxNodes];
        bool    nodesFound;
    };
} // namespace NodeLib
