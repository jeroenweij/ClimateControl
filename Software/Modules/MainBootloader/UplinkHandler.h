/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include "Crc.h"
#include "DelayTimer.h"

#include "Frame.h"

#include "Firmware.h"
#include "NinaAt.h"

// MainController's bridge to the server over the on-board NINA-W152
// (MainController-Server-Link-Spec.md). Owns the NINA AT engine, brings the
// module up through Wi-Fi join and the uplink TCP peer, then relays NodeLib
// frames verbatim in both directions once in data mode:
//   - bus -> uplink: registered as the NodeMaster's INodeHandler, so every
//     frame NodeMaster sees reaches ReceivedMessage() (NodeMaster.cpp already
//     forwards there for anything past its own Transport bookkeeping).
//     ReceivedMessage() also feeds BudgetAllocator::Observe() -- unconditionally,
//     even with the uplink down, since that's bus-side supervision, not
//     something the uplink should gate (Damper-Budget-Spec.md §5.4).
//     ReceivedMessage() only enqueues -- it must never block, since NodeMaster
//     calls it synchronously from inside its own byte-at-a-time bus receive
//     loop (Node::PumpRx()); a blocking NINA write there, multiplied by a
//     burst of several queued node messages arriving back-to-back, previously
//     stalled bus servicing long enough to miss bytes (and the terminating
//     Done) on a real bus with real traffic. The actual NINA writes happen
//     from Loop()/DrainDataMode() instead, decoupled from bus reception.
//   - uplink -> bus: relayed-range frames decoded from the NINA UART are
//     pushed onto the bus via NodeMaster::QueueMessage().
//
// Entirely non-blocking -- Loop() does at most one AT command's worth of
// progress per call, ticked from the same super-loop as NodeMaster::Loop()
// (MainController-Server-Link-Spec.md §3).
class UplinkHandler
{
  public:
    UplinkHandler(Boot::Firmware& firmware);

    void Init();
    void Loop();

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
        NetworkUpSettle, // +UUNU fires before the module can reliably open a peer -- AT+UDCP right after it errors (see UplinkHandler.cpp's NetworkUpSettleMs comment)
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

    // Shared tail of every plain "RunCommand -> Ok transitions, Error/Timeout
    // retries" state: ConfiguringSsid, ConfiguringAuth, ConfiguringPsk,
    // ActivatingWifi, ConnectingPeer, EnteringDataMode. Returns true (having
    // already called TransitionTo(next)) on Ok, false otherwise -- states
    // that need extra work before transitioning check the return value and
    // do that work themselves rather than calling TransitionTo again.
    bool AdvanceOnOk(const NinaAt::Result result, const State next);

    // Shared shape of WaitingNetworkUp / WaitingPeerConnected: drains queued
    // events until 'success' arrives (returns true, caller transitions), one
    // of 'failureEvents' arrives, or stateTimeout expires (either calls
    // Fail() and returns false).
    bool WaitForEvent(const NinaAt::Event success, const NinaAt::Event* const failureEvents, const size_t failureCount);

    void DrainDataMode();
    void DrainOutboundQueue(); // writes everything queued below to NINA
    void HandleUplinkFrame(const NodeLib::Message& message);
    void SendUplinkHello();
    void SendKeepalive();

    void EnqueueUplink(const NodeLib::Message& message); // used by Send* above too, for the same reason

    // Encodes 'm' onto the wire by hand and writes it via nina.WriteBytes().
    // Can't reuse NodeLib::Frame::Write() here -- it takes a Hal::Uart&, and
    // this module deliberately doesn't link Hal::Uart (NinaUart.h's class
    // comment) to stay inside the 10 KB bootloader budget.
    void WriteFrame(const NodeLib::Message& m);

    NinaAt          nina;
    Boot::Firmware& firmware;

    static const uint8_t outboundQueueSize = 4;
    NodeLib::Message     outboundQueue[outboundQueueSize];
    uint8_t              outboundQueued;

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
