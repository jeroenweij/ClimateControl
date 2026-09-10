/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include "DelayTimer.h"

#include "Node.h"

namespace NodeLib
{
    // The ControllerNode's master half of the point-to-point Thermostat link
    // (ControllerNode-Thermostat-Link-Spec.md §5.3). NodeMaster with the
    // arbitration removed: a fixed single peer, one Discover at bring-up, then a
    // 200 ms Poll/Done cycle. It is a distinct class, not a NodeMaster mode --
    // it shares only the framing/Node plumbing, none of the discovery array or
    // round-robin cursor.
    //
    // The peer (the paired Thermostat) is provisioned with the SAME nodeId as
    // this ControllerNode (link is private; §5.2.1), so the peer address is just
    // ConfigStore::NodeId().
    class LinkMaster : public Node
    {
      public:
        explicit LinkMaster(const uint32_t baudRate);

        void Init();
        void Loop();

        // --- state for the ControllerNode's Room* cache / RoomLink -----------
        bool    LinkUp() const;
        uint8_t PeerId() const;
        bool    PeerInBootloader() const; // learned from the peer's Announce

        // --- inject a frame to the peer (flushed on the next poll) -----------
        void SendToPeer(const Endpoint endpoint, const Operation op, const uint8_t* const data, const uint8_t len);
        void GetFromPeer(const Endpoint endpoint);

        // Force the next poll immediately instead of waiting out the 200 ms
        // interval -- used to pace an OTA transfer (§5.4).
        void PollPeerNow();

      private:
        void HandleMasterMessage(const Message& m) override;
        void NotePeerAlive();

        static const uint32_t pollIntervalMs = 200;
        // Link declared down after ~3 missed polls (§5.3).
        static const uint32_t linkTimeoutMs = 3 * pollIntervalMs + pollIntervalMs / 2;

        uint8_t           peerId;
        bool              linkUp;
        bool              peerInBootloader;
        Tools::DelayTimer pollTimer;
        Tools::DelayTimer linkTimer;
    };
} // namespace NodeLib
