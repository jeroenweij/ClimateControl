/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include <stdio.h>

#include "ImageDescriptor.h"
#include "Logger.h"
#include "Tick.h"

#include "Secrets.h"
#include "UplinkHandler.h"

using NodeLib::Endpoint;
using NodeLib::Id;
using NodeLib::Message;
using NodeLib::NodeMaster;
using NodeLib::Operation;

// Firmware version is baked into the image descriptor at link time, same as
// Node.cpp reads it for SystemInfo.
extern "C" const Board::ImageDescriptor gImageDescriptor;

namespace
{
    const uint32_t ProbeTimeoutMs        = 2000;
    const uint32_t ConfigTimeoutMs       = 2000;
    const uint32_t WifiActivateTimeoutMs = 5000;
    const uint32_t ConnectPeerTimeoutMs  = 5000;
    const uint32_t DataModeTimeoutMs     = 2000;
    const uint32_t NetworkUpWaitMs       = 20000;
    const uint32_t PeerConnectedWaitMs   = 10000;
    const uint32_t BackoffInitialMs      = 1000;
    const uint32_t BackoffMaxMs          = 30000;
    const uint32_t KeepaliveIntervalMs   = 25000;
    // u-connectXpress: "After executing the data mode command ..., a delay
    // of 50 ms is required before start of data transmission" -- doubled for
    // margin.
    const uint32_t DataModeSettleMs = 100;
    // Undocumented, found on the bench: +UUNU fires before AT+UDCP reliably
    // succeeds -- the first attempt(s) right after it get ERROR, then a
    // second +UUNU fires and the next attempt succeeds. Absorb that gap here
    // instead of spending ConnectingPeer's retry budget on it (a MaxAttempts-
    // PerState run of bad luck there used to blow the whole bring-up back to
    // Booting for no real reason).
    const uint32_t NetworkUpSettleMs = 500;
    // A real serial link occasionally drops or delays a response -- retry the
    // same command this many times before treating it as a real failure.
    const uint8_t MaxAttemptsPerState = 3;
    // Data mode has no AT lines to signal a drop (MainController-Server-Link-
    // Spec.md §3) -- this is the coarse fallback from §10: a very long silence
    // with nothing at all received forces a reset and full re-join. Well above
    // KeepaliveIntervalMs (25s) and the server's own 90s read deadline
    // (Webserver/internal/uplink/server.go), so it never fires just because
    // the server has nothing to say, but not so generous that a silently
    // dropped connection (e.g. the server process restarting) sits zombied
    // for minutes before anything notices.
    const uint32_t LinkWatchdogMs = 60000;

    void PackU16(uint8_t* const out, const uint16_t value)
    {
        out[0] = static_cast<uint8_t>(value);
        out[1] = static_cast<uint8_t>(value >> 8);
    }

    void PackU32(uint8_t* const out, const uint32_t value)
    {
        out[0] = static_cast<uint8_t>(value);
        out[1] = static_cast<uint8_t>(value >> 8);
        out[2] = static_cast<uint8_t>(value >> 16);
        out[3] = static_cast<uint8_t>(value >> 24);
    }

    bool IsRelayedEndpoint(const Endpoint endpoint)
    {
        const uint8_t value = static_cast<uint8_t>(endpoint);
        return value >= 0x10 && value <= 0x5F;
    }
} // namespace

UplinkHandler::UplinkHandler(NodeMaster& master, BudgetAllocator& budgetAllocator) :
    master(master),
    budgetAllocator(budgetAllocator),
    outboundQueue{},
    outboundQueued(0),
    nina(),
    uplinkCrc(),
    uplinkFrame(uplinkCrc),
    state(State::Booting),
    commandSent(false),
    attemptsInState(0),
    stateTimeout(),
    backoffTimer(),
    backoffMs(BackoffInitialMs),
    helloSent(false),
    keepaliveTimer(),
    linkWatchdog()
{
}

void UplinkHandler::Init()
{
    nina.Init();
    state = State::Booting;
    stateTimeout.Start(500); // let the module's boot ROM banner settle
}

void UplinkHandler::TransitionTo(const State next)
{
    LOG_INFO("Uplink: state " << static_cast<int>(state) << " -> " << static_cast<int>(next));
    state           = next;
    commandSent     = false;
    attemptsInState = 0;
}

void UplinkHandler::Fail()
{
    LOG_WARN("Uplink: bring-up failed in state " << static_cast<int>(state) << ", backing off "
                                                 << backoffMs << " ms");
    nina.PulseReset();
    backoffTimer.Start(backoffMs);
    backoffMs = (backoffMs * 2 > BackoffMaxMs) ? BackoffMaxMs : backoffMs * 2;
    TransitionTo(State::Backoff);
}

void UplinkHandler::HandleCommandFailure()
{
    attemptsInState++;
    if (attemptsInState >= MaxAttemptsPerState)
    {
        Fail();
    }
    // else: commandSent is already false (RunCommand reset it on the
    // non-Pending result), so the next Loop() call just resends the same
    // command -- attemptsInState only resets on a real state change.
}

NinaAt::Result UplinkHandler::RunCommand(const char* const command, const uint32_t timeoutMs)
{
    if (!commandSent)
    {
        commandSent = nina.SendCommand(command, timeoutMs);
        return NinaAt::Result::Pending;
    }

    const NinaAt::Result result = nina.PollResult();
    if (result != NinaAt::Result::Pending)
    {
        commandSent = false;
    }
    return result;
}

void UplinkHandler::Loop()
{
    nina.Loop();

    switch (state)
    {
        case State::Booting:
            if (stateTimeout.Finished())
            {
                TransitionTo(State::ProbingAt);
            }
            break;

        case State::ProbingAt:
        {
            const NinaAt::Result result = RunCommand("AT", ProbeTimeoutMs);
            if (result == NinaAt::Result::Ok)
            {
                TransitionTo(State::ConfiguringSsid);
            }
            // Error/Timeout: the module may still be booting -- RunCommand's
            // own per-attempt timeout already paces the retry, just try again
            // (unbounded here by design, unlike the states below).
            break;
        }

        case State::ConfiguringSsid:
        {
            // Only formatted when actually about to send -- snprintf on every
            // waiting-for-reply Loop() call was slow enough to occasionally
            // miss the single-byte RX buffer window (see Uart::Available()'s
            // overrun comment).
            char cmd[64];
            if (!commandSent)
            {
                snprintf(cmd, sizeof(cmd), "AT+UWSC=0,2,\"%s\"", Secrets::WifiSsid);
            }
            const NinaAt::Result result = RunCommand(cmd, ConfigTimeoutMs);
            if (result == NinaAt::Result::Ok)
            {
                TransitionTo(State::ConfiguringAuth);
            }
            else if (result == NinaAt::Result::Error || result == NinaAt::Result::Timeout)
            {
                HandleCommandFailure();
            }
            break;
        }

        case State::ConfiguringAuth:
        {
            const NinaAt::Result result = RunCommand("AT+UWSC=0,5,2", ConfigTimeoutMs);
            if (result == NinaAt::Result::Ok)
            {
                TransitionTo(State::ConfiguringPsk);
            }
            else if (result == NinaAt::Result::Error || result == NinaAt::Result::Timeout)
            {
                HandleCommandFailure();
            }
            break;
        }

        case State::ConfiguringPsk:
        {
            char cmd[96];
            if (!commandSent)
            {
                snprintf(cmd, sizeof(cmd), "AT+UWSC=0,8,\"%s\"", Secrets::WifiPassword);
            }
            const NinaAt::Result result = RunCommand(cmd, ConfigTimeoutMs);
            if (result == NinaAt::Result::Ok)
            {
                TransitionTo(State::ActivatingWifi);
            }
            else if (result == NinaAt::Result::Error || result == NinaAt::Result::Timeout)
            {
                HandleCommandFailure();
            }
            break;
        }

        case State::ActivatingWifi:
        {
            const NinaAt::Result result = RunCommand("AT+UWSCA=0,3", WifiActivateTimeoutMs);
            if (result == NinaAt::Result::Ok)
            {
                stateTimeout.Start(NetworkUpWaitMs);
                TransitionTo(State::WaitingNetworkUp);
            }
            else if (result == NinaAt::Result::Error || result == NinaAt::Result::Timeout)
            {
                HandleCommandFailure();
            }
            break;
        }

        case State::WaitingNetworkUp:
        {
            NinaAt::Event event;
            bool          failed = stateTimeout.Finished();
            while (!failed && nina.NextEvent(event))
            {
                if (event == NinaAt::Event::NetworkUp)
                {
                    stateTimeout.Start(NetworkUpSettleMs);
                    TransitionTo(State::NetworkUpSettle);
                    return;
                }
                if (event == NinaAt::Event::LinkDown || event == NinaAt::Event::NetworkDown)
                {
                    failed = true;
                }
            }
            if (failed)
            {
                Fail();
            }
            break;
        }

        case State::NetworkUpSettle:
            if (stateTimeout.Finished())
            {
                TransitionTo(State::ConnectingPeer);
            }
            break;

        case State::ConnectingPeer:
        {
            char cmd[80];
            if (!commandSent)
            {
                snprintf(cmd, sizeof(cmd), "AT+UDCP=\"tcp://%s:%u/\"", Secrets::ServerHost, static_cast<unsigned>(Secrets::ServerPort));
            }
            const NinaAt::Result result = RunCommand(cmd, ConnectPeerTimeoutMs);
            if (result == NinaAt::Result::Ok)
            {
                stateTimeout.Start(PeerConnectedWaitMs);
                TransitionTo(State::WaitingPeerConnected);
            }
            else if (result == NinaAt::Result::Error || result == NinaAt::Result::Timeout)
            {
                HandleCommandFailure();
            }
            break;
        }

        case State::WaitingPeerConnected:
        {
            NinaAt::Event event;
            bool          failed = stateTimeout.Finished();
            while (!failed && nina.NextEvent(event))
            {
                if (event == NinaAt::Event::PeerConnected)
                {
                    TransitionTo(State::EnteringDataMode);
                    return;
                }
                if (event == NinaAt::Event::PeerDisconnected || event == NinaAt::Event::LinkDown ||
                    event == NinaAt::Event::NetworkDown)
                {
                    failed = true;
                }
            }
            if (failed)
            {
                Fail();
            }
            break;
        }

        case State::EnteringDataMode:
        {
            const NinaAt::Result result = RunCommand("ATO", DataModeTimeoutMs);
            if (result == NinaAt::Result::Ok)
            {
                nina.EnterDataMode();
                backoffMs = BackoffInitialMs; // a clean bring-up resets the backoff
                stateTimeout.Start(DataModeSettleMs);
                TransitionTo(State::DataModeSettle);
            }
            else if (result == NinaAt::Result::Error || result == NinaAt::Result::Timeout)
            {
                HandleCommandFailure();
            }
            break;
        }

        case State::DataModeSettle:
            if (stateTimeout.Finished())
            {
                helloSent = false;
                linkWatchdog.Start(LinkWatchdogMs);
                TransitionTo(State::DataMode);
                LOG_INFO("Uplink Online");
            }
            break;

        case State::DataMode:
            DrainDataMode();
            break;

        case State::Backoff:
            if (backoffTimer.Finished())
            {
                TransitionTo(State::Booting);
                stateTimeout.Start(500);
            }
            break;
    }
}

void UplinkHandler::DrainDataMode()
{
    if (!helloSent)
    {
        SendUplinkHello();
        SendRoster();
        helloSent = true;
        keepaliveTimer.Start(KeepaliveIntervalMs);
    }

    bool sawByte = false;
    while (nina.Available())
    {
        sawByte = true;
        Message decoded;
        if (uplinkFrame.FeedByte(nina.ReadByte(), decoded))
        {
            HandleUplinkFrame(decoded);
        }
    }
    uplinkFrame.Update();

    DrainOutboundQueue();

    if (sawByte)
    {
        linkWatchdog.Start(LinkWatchdogMs);
    }
    else if (linkWatchdog.Finished())
    {
        LOG_WARN("Uplink: watchdog -- no inbound bytes for " << LinkWatchdogMs << " ms, resetting NINA");
        Fail();
        return;
    }

    if (keepaliveTimer.Finished())
    {
        SendKeepalive();
        keepaliveTimer.Start(KeepaliveIntervalMs);
    }
}

void UplinkHandler::DrainOutboundQueue()
{
    for (uint8_t i = 0; i < outboundQueued; i++)
    {
        uplinkFrame.Write(nina.RawUart(), outboundQueue[i]);
    }
    outboundQueued = 0;
}

void UplinkHandler::EnqueueUplink(const Message& message)
{
    if (outboundQueued < outboundQueueSize)
    {
        outboundQueue[outboundQueued] = message;
        outboundQueued++;
    }
    // else: drop -- queue full, same backstop as the bus-bound queue
    // (MainController-Server-Link-Spec.md §7.2).
}

void UplinkHandler::HandleUplinkFrame(const Message& message)
{
    if (IsRelayedEndpoint(message.id.endpoint))
    {
        master.QueueMessage(message);
        return;
    }

    // Uplink-block frame from the server. Only Keepalive is answered today --
    // Roster/OtaControl/OtaData handling is future work (Open item,
    // MainController-Server-Link-Spec.md §11).
    if (message.id.endpoint == Endpoint::Keepalive && message.id.operation == Operation::Get)
    {
        EnqueueUplink(Message(Id(0, Endpoint::Keepalive, Operation::Report)));
    }
}

void UplinkHandler::SendUplinkHello()
{
    uint8_t        payload[23];
    const uint16_t fwVersion = static_cast<uint16_t>((gImageDescriptor.fwVersionMajor << 8) |
                                                     (gImageDescriptor.fwVersionMinor & 0xFF));
    PackU16(&payload[0], fwVersion);
    PackU32(&payload[2], Hal::Tick::Millis() / 1000u);
    payload[6] = master.ActiveNodeCount();
    for (size_t i = 0; i < sizeof(Secrets::UplinkToken); i++)
    {
        payload[7 + i] = Secrets::UplinkToken[i];
    }

    Message hello(Id(0, Endpoint::UplinkHello, Operation::Report));
    hello.len = sizeof(payload);
    for (uint8_t i = 0; i < hello.len; i++)
    {
        hello.data[i] = payload[i];
    }
    EnqueueUplink(hello);
}

void UplinkHandler::SendRoster()
{
    // No physical nodes on this bus yet -- an empty roster is just the
    // terminator (MainController-Server-Link-Spec.md §5).
    EnqueueUplink(Message(Id(0, Endpoint::Roster, Operation::Report), static_cast<uint8_t>(0xFF)));
}

void UplinkHandler::SendKeepalive()
{
    EnqueueUplink(Message(Id(0, Endpoint::Keepalive, Operation::Get)));
}

void UplinkHandler::ReceivedMessage(const Message& message)
{
    // Bus-side supervision runs regardless of uplink state -- see the class
    // comment.
    budgetAllocator.Observe(message);

    // Called synchronously from NodeMaster's bus receive path (see the class
    // comment in UplinkHandler.h) -- must only enqueue, never block on NINA.
    if (!nina.InDataMode())
    {
        return; // uplink down -- dropped, not queued (MainController-Server-Link-Spec.md §10)
    }
    if (!IsRelayedEndpoint(message.id.endpoint))
    {
        return; // NodeLib-internal traffic (Transport etc.) never relays
    }
    EnqueueUplink(message);
}

void UplinkHandler::ConnectionLost()
{
    LOG_WARN("Uplink: bus heartbeat lost");
}
