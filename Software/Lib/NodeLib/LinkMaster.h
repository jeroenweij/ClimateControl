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
    // The peer (the paired Thermostat) always has THERMOSTAT_NODE_ID -- every
    // Thermostat carries the same fixed id, independent of this ControllerNode's
    // own bus id (link is private; §5.2.1).
    class LinkMaster : public Node
    {
      public:
        explicit LinkMaster();

        void Init();
        void Loop();

        // --- state for the ControllerNode's Room* cache / RoomLink -----------
        bool    LinkUp() const;
        uint8_t PeerId() const;
        bool    PeerInBootloader() const; // learned from the peer's Announce

        // --- inject a frame to the peer (flushed on the next poll) -----------
        void SendToPeer(const Endpoint endpoint, const Operation op, const uint8_t* const data, const uint8_t len);
        void GetFromPeer(const Endpoint endpoint);

      private:
        void HandleMasterMessage(const Message& m) override;
        void NotePeerAlive();

        static const uint32_t pollIntervalMs = 100;
        // Link declared down after ~3 missed polls (§5.3).
        static const uint32_t linkTimeoutMs = 3 * pollIntervalMs + pollIntervalMs / 2;
        // Re-broadcast Discover periodically (not just once at Init()) so a
        // peer that reboots mid-session -- and so misses the initial
        // Discover -- still gets re-announced and its bootloader state
        // re-learned. Independent of pollIntervalMs; no need for it to be fast.
        static const uint32_t discoverIntervalMs = 5000;

        uint8_t           peerId;
        bool              linkUp;
        bool              peerInBootloader;
        bool              sendOk;
        bool              discovering;
        Tools::DelayTimer pollTimer;
        Tools::DelayTimer linkTimer;
        Tools::DelayTimer discoverTimer;
    };
} // namespace NodeLib
