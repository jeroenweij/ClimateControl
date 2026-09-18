/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include "Crc.h"
#include "DelayTimer.h"

#include "Frame.h"
#include "INodeHandler.h"
#include "NodeMaster.h"

#include "NinaAt.h"

// MainController's bridge to the server over the on-board NINA-W152
// (MainController-Server-Link-Spec.md). Owns the NINA AT engine, brings the
// module up through Wi-Fi join and the uplink TCP peer, then relays NodeLib
// frames verbatim in both directions once in data mode:
//   - bus -> uplink: registered as the NodeMaster's INodeHandler, so every
//     frame NodeMaster sees reaches ReceivedMessage() (NodeMaster.cpp already
//     forwards there for anything past its own Transport bookkeeping).
//   - uplink -> bus: relayed-range frames decoded from the NINA UART are
//     pushed onto the bus via NodeMaster::QueueMessage().
//
// Entirely non-blocking -- Loop() does at most one AT command's worth of
// progress per call, ticked from the same super-loop as NodeMaster::Loop()
// (MainController-Server-Link-Spec.md §3).
class UplinkHandler : public NodeLib::INodeHandler
{
  public:
    explicit UplinkHandler(NodeLib::NodeMaster& master);

    void Init();
    void Loop();

    void ReceivedMessage(const NodeLib::Message& message) override;
    void ConnectionLost() override;

  private:
    enum class State
    {
        Booting, // just (re)reset -- give the module a moment before probing
        ProbingAt,
        ConfiguringSsid,
        ConfiguringAuth,
        ConfiguringPsk,
        ActivatingWifi,
        WaitingNetworkUp,
        ConnectingPeer,
        WaitingPeerConnected,
        EnteringDataMode,
        DataModeSettle, // u-connectXpress needs >=50 ms after ATO's OK before the first data-mode byte
        DataMode,
        Backoff,
    };

    void TransitionTo(const State next);
    void Fail(); // retries exhausted -> backoff and retry the whole bring-up from Booting

    // Issues 'command' the first time this is called after a state change,
    // then polls it to completion. Ok/Error/Timeout once resolved (and ready
    // for the next state's command), Pending while still in flight.
    NinaAt::Result RunCommand(const char* const command, const uint32_t timeoutMs);

    // A real serial link occasionally drops or delays one response -- a
    // single Timeout/Error shouldn't nuke the whole bring-up. Retries the
    // current state's command (commandSent is already false by the time this
    // is called, so the next Loop() just resends it) up to maxAttemptsPerState
    // times before giving up via Fail().
    void HandleCommandFailure();

    void DrainDataMode();
    void HandleUplinkFrame(const NodeLib::Message& message);
    void SendUplinkHello();
    void SendRoster();
    void SendKeepalive();

    NodeLib::NodeMaster& master;

    NinaAt nina;

    // Second Frame/Crc pair for the uplink socket -- byte-for-byte the same
    // wire format as the bus (MainController-Server-Link-Spec.md §4), sharing
    // the one physical CRC peripheral safely: Hal::Crc::Compute() resets and
    // computes over a whole buffer per call, never holding state across
    // calls, so this and the bus Frame never collide.
    Hal::Crc       uplinkCrc;
    NodeLib::Frame uplinkFrame;

    State             state;
    bool              commandSent;
    uint8_t           attemptsInState;
    Tools::DelayTimer stateTimeout;
    Tools::DelayTimer backoffTimer;
    uint32_t          backoffMs;

    bool              helloSent;
    Tools::DelayTimer keepaliveTimer;
    Tools::DelayTimer linkWatchdog;
};
