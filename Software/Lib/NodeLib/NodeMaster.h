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

        // Module type reported in the node's Announce (0 = not seen / unknown;
        // values match NodeLib::ConfigStore::Module). nodeId is 1..maxNodes.
        uint8_t NodeModule(const uint8_t nodeId) const;

        // Nodes currently believed active (seen an Announce). Used e.g. for the
        // uplink's UplinkHello.nodeCount (MainController-Server-Link-Spec.md §5).
        uint8_t ActiveNodeCount() const;

      private:
        enum class EMasterState : uint8_t
        {
            Start,
            Detecting,
            Flush,
            Pollgap,
            Polling
        };

        struct SNode
        {
            SNode();

            bool    active;
            uint8_t moduleType;
            bool    inBootloader;
        };

        void DetectNodes();
        void PollNextNode(const int prevNodeId);
        void HandleMasterMessage(const Message& m) override;
        void NodeHello(int nodeId, uint8_t module, bool bootloader);
        void HandleInternalOperation(const Message& m);

        EMasterState state;

        // Sized at the compile-time maxNodes cap, not the runtime numNodes -- see
        // Node.h's comment on that split.
        SNode slaveNodes[maxNodes];
        bool  nodesFound;

        // A node's reply can still be lost outright (a real bus always has
        // some residual risk -- electrical noise, a marginal edge case, even
        // with the write path non-blocking) -- without this, PollNextNode()
        // only ever re-fires on that node's own Done, so one lost reply
        // wedges the whole round-robin forever. Started on every poll sent,
        // stopped by DelayTimer::Finished() itself the moment either the
        // real Done arrives (HandleInternalOperation restarts it for the
        // next node) or this fires first and Loop() treats the timeout the
        // same as a Done -- move on, same self-healing every round.
        static const uint32_t pollTimeoutMs = 200;
        Tools::DelayTimer     pollTimeout;
        int                   pendingPollNode;

        // A node only ever announces in reply to a Discover broadcast (see
        // Node::HandlePollRequest()) -- it never self-announces on power-up --
        // so a node plugged in after the initial DetectNodes() in Init() would
        // otherwise stay invisible forever. Re-run DetectNodes() on this
        // cadence to pick up newly plugged nodes.
        static const uint32_t detectIntervalMs = 15000;
        Tools::DelayTimer     timeoutTimer;
        Tools::DelayTimer     detectTimer;

        // Flushing the queue and then immediately writing the round-robin's
        // own Poll right after, with no gap, is fine against a node that's
        // actively cycling through Poll/Done already -- but found on the
        // bench to desync the bootloader's receiver (Boot::OtaUart), which at
        // the time was a plain poll-only Available()/Read() straight off
        // ISR/RDR -- busy elsewhere (e.g. a multi-page flash erase/program
        // run) for even one byte-time could lose whatever arrived right
        // behind a flush with no gap. OtaUart is now interrupt-driven with
        // its own ring buffer (same fix class as Hal::Uart's), which should
        // make byte loss from this specific pattern far less likely, but the
        // gap costs nothing and stays as cheap insurance: EMasterState::
        // Pollgap holds off sending that Poll (no opportunistic flush, no new
        // poll -- see Loop()'s Flush/Pollgap cases) until a real elapsed-time
        // gap has passed (matching the bus's own established inter-frame
        // spacing, NodeSpacingMs in FirmwareSlave), instead of writing it
        // back to back with the flush in the same state.
        static const uint32_t pollGapMs = 25;
        Tools::DelayTimer     pollGapTimer;
    };
} // namespace NodeLib
