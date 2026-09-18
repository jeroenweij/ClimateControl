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
        NodeMaster();

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

        // A node's reply can still be lost outright (a real bus always has
        // some residual risk -- electrical noise, a marginal edge case, even
        // with the write path non-blocking) -- without this, PollNextNode()
        // only ever re-fires on that node's own Done, so one lost reply
        // wedges the whole round-robin forever. Started on every poll sent,
        // stopped by DelayTimer::Finished() itself the moment either the
        // real Done arrives (HandleInternalOperation restarts it for the
        // next node) or this fires first and Loop() treats the timeout the
        // same as a Done -- move on, same self-healing every round.
        static const uint32_t pollTimeoutMs = 100;
        Tools::DelayTimer     pollTimeout;
        int                   pendingPollNode;
    };
} // namespace NodeLib
