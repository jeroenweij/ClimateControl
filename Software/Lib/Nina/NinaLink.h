/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include "Crc.h"
#include "DelayTimer.h"

#include "Frame.h"

#include "NinaAt.h"

// What differs between the application's uplink and the bootloader's: which
// frames mean what, and what to say in the hello. NinaLink does everything
// else (MainController-Server-Link-Spec.md §3).
class NinaLinkHandler
{
  public:
    // Fills in the UplinkHello (endpoint, operation and payload); sent first
    // after every (re)connect.
    virtual void BuildHello(NodeLib::Message& hello) = 0;

    // A valid frame from the server (Keepalive Get is answered by NinaLink
    // itself and never reaches here).
    virtual void OnFrame(const NodeLib::Message& message) = 0;

    // Once per connection, right after the hello has been queued.
    virtual void OnConnected() {}

    // Each data-mode pass, before inbound frames are read (after the pass that
    // sent the hello).
    virtual void BeforeFrames() {}

    // Each data-mode pass, after the outbound queue has been written out --
    // the point where a pending reset can safely go ahead.
    virtual void AfterFrames() {}
};

struct NinaLinkConfig
{
    const char* ssid;
    const char* password;
    const char* host;
    uint16_t    port;
};

// The uplink connection to the server over the NINA: brings the module up
// through Wi-Fi join and the TCP peer, then carries NodeLib frames both ways in
// data mode, supervising the link (keepalive deadline, watchdog, backoff).
// Entirely non-blocking -- Loop() does at most one AT command's worth of
// progress per call, ticked from the super-loop.
class NinaLink
{
  public:
    // 'queue' is the caller's storage for outbound frames (sized to taste:
    // the bootloader needs a handful, the application a whole bus's worth).
    NinaLink(NinaPort& port, const NinaLinkConfig& config, NinaLinkHandler& handler, NodeLib::Message* const queue, const uint8_t queueSize);

    void Init();
    void Loop();

    // Queues a frame for the server. Never blocks; drops the frame when the
    // queue is full (MainController-Server-Link-Spec.md §7.2). Returns whether
    // it was queued.
    bool Send(const NodeLib::Message& message);

    // Blocks until everything written so far has left the UART.
    void Flush();

    // The uplink is up: the pipe to the server is open and carrying frames.
    bool InDataMode() const;

    // Outbound queue inspection, for host tests.
    uint8_t                 Queued() const;
    const NodeLib::Message& QueuedAt(const uint8_t index) const;
    void                    ClearQueue();

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
        NetworkUpSettle, // +UUNU fires before the module can reliably open a peer -- AT+UDCP right after it errors (see NinaLink.cpp's NetworkUpSettleMs comment)
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
    // is called, so the next Loop() just resends it) up to MaxAttemptsPerState
    // times before giving up via Fail().
    void HandleCommandFailure();

    // Shared tail of every plain "RunCommand -> Ok transitions, Error/Timeout
    // retries" state. Returns true (having already called TransitionTo(next))
    // on Ok, false otherwise -- states that need extra work before
    // transitioning check the return value and do that work themselves.
    bool AdvanceOnOk(const NinaAt::Result result, const State next);

    void StartSession(); // hello first, then OnConnected()
    void DrainDataMode();
    void DrainOutboundQueue();

    NinaAt                nina;
    const NinaLinkConfig& config;
    NinaLinkHandler&      handler;

    NodeLib::Message* const queue;
    const uint8_t           queueSize;
    uint8_t                 queued;

    // Byte-for-byte the same wire format as the bus (MainController-Server-
    // Link-Spec.md §4). Sharing the one physical CRC peripheral with a bus
    // Frame is safe: Hal::Crc::Compute() resets and computes over a whole
    // buffer per call, never holding state across calls.
    Hal::Crc       uplinkCrc;
    NodeLib::Frame uplinkFrame;

    State             state;
    bool              commandSent;
    uint8_t           attemptsInState;
    Tools::DelayTimer stateTimeout;
    Tools::DelayTimer backoffTimer;
    uint32_t          backoffMs;

    Tools::DelayTimer keepaliveTimer;
    Tools::DelayTimer keepaliveReplyTimer; // running while a keepalive is unanswered
    Tools::DelayTimer linkWatchdog;
    Tools::DelayTimer retryTimer; // running while a failed command waits to be re-sent
};
