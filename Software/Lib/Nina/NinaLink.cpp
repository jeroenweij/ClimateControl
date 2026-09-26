/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "NinaLink.h"

// The application logs the state machine's progress; the bootloader (no
// Logger, 10 KB budget) compiles it out.
#ifdef CC_NINA_LOG
#include "Logger.h"
#define NINA_LOG_INFO(message) LOG_INFO(message)
#define NINA_LOG_WARN(message) LOG_WARN(message)
#else
#define NINA_LOG_INFO(message) \
    {                          \
    }
#define NINA_LOG_WARN(message) \
    {                          \
    }
#endif

using NodeLib::Endpoint;
using NodeLib::Id;
using NodeLib::Message;
using NodeLib::Operation;

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
    const uint32_t KeepaliveIntervalMs   = 10000;
    // A keepalive must be answered (by any valid frame) within this long or the
    // link is treated as dead: detects a dropped peer, a half-open connection
    // or a server that is up but not answering, in KeepaliveIntervalMs +
    // KeepaliveReplyMs at worst, independent of how (or whether) the module
    // signals the drop.
    const uint32_t KeepaliveReplyMs = 10000;
    // u-connectXpress: "After executing the data mode command ..., a delay
    // of 50 ms is required before start of data transmission" -- doubled for
    // margin.
    const uint32_t DataModeSettleMs = 100;
    // Undocumented, found on the bench: +UUNU fires before AT+UDCP reliably
    // succeeds -- an AT+UDCP right after it gets ERROR, then a second +UUNU
    // fires (0.3-0.9 s after the first) and only then does it succeed. The
    // network counts as settled once no network event has been seen for this
    // long (a further event restarts the wait), so ConnectingPeer's retry
    // budget is not spent on the gap.
    const uint32_t NetworkUpSettleMs = 1000;
    // Pause before re-sending a command that got ERROR or no reply: retrying
    // at once burns all attempts inside the very window the module needs to
    // get ready (the +UUNU gap above), then costs a full module restart.
    const uint32_t RetryDelayMs = 500;
    // A real serial link occasionally drops or delays a response -- retry the
    // same command this many times before treating it as a real failure.
    const uint8_t MaxAttemptsPerState = 3;
    // The "AT" probe after a reset gets far more slack than a config step --
    // a module still booting is normal -- but not unlimited: a NINA-W152 has
    // been seen on the bench to boot (+STARTUP) and then never answer "AT"
    // at all, and only another reset brings it back. A normal boot answers
    // within ~2 s; this is ~20 s of probing (ProbeTimeoutMs each).
    const uint8_t MaxProbeAttempts = 10;
    // AT+CPWROFF answers OK and then restarts; give it a moment before the
    // AT probe (which itself waits out a slow boot).
    const uint32_t ModuleRestartMs = 1000;
    // Backstop behind the keepalive deadline: a very long silence with no
    // valid frame received forces a reset and full re-join even if the
    // keepalive bookkeeping itself were ever wedged. Well above the server's
    // reply cadence, and above the server's own read deadline margin
    // (Webserver/internal/uplink/server.go).
    const uint32_t LinkWatchdogMs = 60000;

    // Minimal, allocation-free command building -- avoids pulling in
    // snprintf's general formatting engine (nano's _svfprintf_r, which drags
    // in memmove/memcpy/_realloc_r) for what's just string concatenation.
    // Each returns the new length written so far; 'cap' includes room for
    // the terminating '\0'.
    size_t AppendStr(char* const buf, const size_t pos, const size_t cap, const char* s)
    {
        size_t i = pos;
        while (*s && i + 1 < cap)
        {
            buf[i++] = *s++;
        }
        buf[i] = '\0';
        return i;
    }

    size_t AppendUInt(char* const buf, const size_t pos, const size_t cap, unsigned value)
    {
        char   digits[10];
        size_t n = 0;
        do
        {
            digits[n++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value != 0);

        size_t i = pos;
        while (n > 0 && i + 1 < cap)
        {
            buf[i++] = digits[--n];
        }
        buf[i] = '\0';
        return i;
    }
} // namespace

NinaLink::NinaLink(NinaPort& port, const NinaLinkConfig& config, NinaLinkHandler& handler, Message* const queue, const uint8_t queueSize) :
    nina(port),
    config(config),
    handler(handler),
    queue(queue),
    queueSize(queueSize),
    queued(0),
    uplinkCrc(),
    uplinkFrame(uplinkCrc),
    state(State::Booting),
    commandSent(false),
    attemptsInState(0),
    stateTimeout(),
    backoffTimer(),
    backoffMs(BackoffInitialMs),
    keepaliveTimer(),
    keepaliveReplyTimer(),
    linkWatchdog(),
    retryTimer()
{
}

void NinaLink::Init()
{
    nina.Init();
    state = State::Booting;
    stateTimeout.Start(500); // let the module's boot ROM banner settle
}

bool NinaLink::InDataMode() const
{
    return state == State::DataMode;
}

bool NinaLink::Send(const Message& message)
{
    if (queued >= queueSize)
    {
        return false; // dropped -- queue full, the backstop from MainController-Server-Link-Spec.md §7.2
    }
    queue[queued++] = message;
    return true;
}

void NinaLink::Flush()
{
    nina.Flush();
}

uint8_t NinaLink::Queued() const
{
    return queued;
}

const Message& NinaLink::QueuedAt(const uint8_t index) const
{
    return queue[index];
}

void NinaLink::ClearQueue()
{
    queued = 0;
}

void NinaLink::DrainOutboundQueue()
{
    for (uint8_t i = 0; i < queued; i++)
    {
        uint8_t      wire[NodeLib::Frame::MaxFrameBytes];
        const size_t length = uplinkFrame.Encode(queue[i], wire);
        nina.WriteBytes(wire, length);
    }
    queued = 0;
}

void NinaLink::StartSession()
{
    // Whatever was staged before now belonged to the previous connection (a
    // keepalive queued just as its deadline reset the link, frames from the
    // settle window): the server rejects a connection whose first frame is not
    // the hello, and the roster that follows brings it up to date anyway.
    queued = 0;

    Message hello;
    handler.BuildHello(hello);
    Send(hello);
    keepaliveTimer.Start(KeepaliveIntervalMs);
    handler.OnConnected();
}

void NinaLink::DrainDataMode()
{
    handler.BeforeFrames();

    // Liveness is a decoded frame, not a byte: once the peer is gone the
    // module is back in command mode and can answer stray writes with text
    // ("ERROR", "+UUDPD"), which must not keep a dead link looking alive.
    bool sawFrame          = false;
    bool sawKeepaliveReply = false;
    while (nina.Available())
    {
        Message decoded;
        if (uplinkFrame.FeedByte(nina.ReadByte(), decoded))
        {
            sawFrame = true;
            if (decoded.id.endpoint == Endpoint::Keepalive && decoded.id.operation == Operation::Get)
            {
                Send(Message(Id(0, Endpoint::Keepalive, Operation::Report)));
            }
            else if (decoded.id.endpoint == Endpoint::Keepalive)
            {
                sawKeepaliveReply = true;
            }
            else
            {
                handler.OnFrame(decoded);
            }
            // Out as they are produced: a run of requests read in one pass
            // (an OTA window) would otherwise overflow the short send queue
            // and lose the replies behind the first few.
            DrainOutboundQueue();
        }
    }
    uplinkFrame.Update();

    DrainOutboundQueue();
    handler.AfterFrames(); // may reset the MCU and never return

    if (sawFrame)
    {
        linkWatchdog.Start(LinkWatchdogMs);
    }
    else if (linkWatchdog.Finished())
    {
        NINA_LOG_WARN("Uplink: watchdog -- no valid frame for " << LinkWatchdogMs << " ms, resetting NINA");
        Fail();
        return;
    }
    if (sawKeepaliveReply)
    {
        // Only the answer to our own keepalive proves the path *back to the
        // server* works. Other frames (an OTA transfer's writes) prove only
        // that the server can still reach us, which is exactly what stays true
        // when the module stops forwarding our bytes.
        keepaliveReplyTimer.Stop();
    }

    if (keepaliveTimer.Finished())
    {
        Send(Message(Id(0, Endpoint::Keepalive, Operation::Get)));
        keepaliveTimer.Start(KeepaliveIntervalMs);
        if (!keepaliveReplyTimer.IsRunning())
        {
            keepaliveReplyTimer.Start(KeepaliveReplyMs);
        }
    }

    if (keepaliveReplyTimer.Finished())
    {
        NINA_LOG_WARN("Uplink: keepalive not answered within " << KeepaliveReplyMs << " ms, resetting NINA");
        Fail();
    }
}

void NinaLink::TransitionTo(const State next)
{
    NINA_LOG_INFO("Uplink: state " << static_cast<int>(state) << " -> " << static_cast<int>(next));
    state           = next;
    commandSent     = false;
    attemptsInState = 0;
    retryTimer.Stop();
}

void NinaLink::Fail()
{
    NINA_LOG_WARN("Uplink: bring-up failed in state " << static_cast<int>(state) << ", backing off " << backoffMs << " ms");
    nina.PulseReset();
    backoffTimer.Start(backoffMs);
    backoffMs = (backoffMs * 2 > BackoffMaxMs) ? BackoffMaxMs : backoffMs * 2;
    TransitionTo(State::Backoff);
}

void NinaLink::HandleCommandFailure()
{
    attemptsInState++;
    if (attemptsInState >= MaxAttemptsPerState)
    {
        Fail();
    }
    else
    {
        retryTimer.Start(RetryDelayMs);
    }
    // else: commandSent is already false (RunCommand reset it on the
    // non-Pending result), so the next Loop() call just resends the same
    // command -- attemptsInState only resets on a real state change.
}

bool NinaLink::AdvanceOnOk(const NinaAt::Result result, const State next)
{
    if (result == NinaAt::Result::Ok)
    {
        TransitionTo(next);
        return true;
    }
    if (result == NinaAt::Result::Error || result == NinaAt::Result::Timeout)
    {
        HandleCommandFailure();
    }
    return false;
}

NinaAt::Result NinaLink::RunCommand(const char* const command, const uint32_t timeoutMs)
{
    if (!commandSent)
    {
        if (retryTimer.IsRunning() && !retryTimer.Finished())
        {
            return NinaAt::Result::Pending;
        }
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

void NinaLink::Loop()
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
                TransitionTo(State::CheckingBluetooth);
            }
            else if (result != NinaAt::Result::Pending && ++attemptsInState >= MaxProbeAttempts)
            {
                Fail(); // booted but deaf -- reset it again (MaxProbeAttempts)
            }
            // else Error/Timeout: the module may still be booting --
            // RunCommand's own per-attempt timeout already paces the retry,
            // just try again.
            break;
        }

        // Bluetooth off. u-connectXpress lists Wi-Fi + Bluetooth running
        // together as a known cause of "unexpected disconnections,
        // difficulties to establish connections and increased latency"
        // (NINA-W15 v6.0.1 release notes, UCS_DEV-3483), and a module first
        // set up by older firmware keeps Bluetooth on in its stored settings
        // (AT+UBTMODE factory default 3). Changing it needs a store and a
        // restart, so it is done once, here, before Wi-Fi is active (storing
        // while the station is up can crash the module, UCS_DEV-2228); every
        // later bring-up just reads 0 and moves on.
        case State::CheckingBluetooth:
        {
            const NinaAt::Result result = RunCommand("AT+UBTMODE?", ConfigTimeoutMs);
            if (result == NinaAt::Result::Ok)
            {
                TransitionTo(nina.LastBtMode() == 0 ? State::ConfiguringSsid : State::DisablingBluetooth);
            }
            else if (result != NinaAt::Result::Pending)
            {
                // Can't tell (a firmware without the command) -- never let
                // this check cost the uplink.
                TransitionTo(State::ConfiguringSsid);
            }
            break;
        }

        case State::DisablingBluetooth:
            AdvanceOnOk(RunCommand("AT+UBTMODE=0", ConfigTimeoutMs), State::StoringSettings);
            break;

        case State::StoringSettings:
            AdvanceOnOk(RunCommand("AT&W", ConfigTimeoutMs), State::RestartingModule);
            break;

        case State::RestartingModule:
            if (AdvanceOnOk(RunCommand("AT+CPWROFF", ConfigTimeoutMs), State::Booting))
            {
                NINA_LOG_INFO("Uplink: Bluetooth disabled, module restarting");
                stateTimeout.Start(ModuleRestartMs);
            }
            break;

        case State::ConfiguringSsid:
        {
            // Only formatted when actually about to send -- doing it on every
            // waiting-for-reply Loop() call was slow enough to occasionally
            // miss the single-byte RX buffer window (see Uart::Available()'s
            // overrun comment).
            char cmd[64];
            if (!commandSent)
            {
                size_t n = AppendStr(cmd, 0, sizeof(cmd), "AT+UWSC=0,2,\"");
                n        = AppendStr(cmd, n, sizeof(cmd), config.ssid);
                AppendStr(cmd, n, sizeof(cmd), "\"");
            }
            const NinaAt::Result result = RunCommand(cmd, ConfigTimeoutMs);
            AdvanceOnOk(result, State::ConfiguringAuth);
            break;
        }

        case State::ConfiguringAuth:
        {
            const NinaAt::Result result = RunCommand("AT+UWSC=0,5,2", ConfigTimeoutMs);
            AdvanceOnOk(result, State::ConfiguringPsk);
            break;
        }

        case State::ConfiguringPsk:
        {
            char cmd[96];
            if (!commandSent)
            {
                size_t n = AppendStr(cmd, 0, sizeof(cmd), "AT+UWSC=0,8,\"");
                n        = AppendStr(cmd, n, sizeof(cmd), config.password);
                AppendStr(cmd, n, sizeof(cmd), "\"");
            }
            const NinaAt::Result result = RunCommand(cmd, ConfigTimeoutMs);
            if (AdvanceOnOk(result, State::ActivatingWifi))
            {
                // Network events seen before this point belong to the module's
                // previous life (one that arrives just as Fail() resets it is
                // still read in), not to the connection about to be made.
                nina.DiscardEvents();
            }
            break;
        }

        case State::ActivatingWifi:
        {
            const NinaAt::Result result = RunCommand("AT+UWSCA=0,3", WifiActivateTimeoutMs);
            if (AdvanceOnOk(result, State::WaitingNetworkUp))
            {
                stateTimeout.Start(NetworkUpWaitMs);
            }
            break;
        }

        case State::WaitingNetworkUp:
        {
            // URCs arrive in bursts (IP up, down, up again, ...): drain all
            // that are queued and go by where things stand at the end. A
            // link/network-down seen before the IP is just the module
            // settling -- only the timeout fails this wait.
            NinaAt::Event event;
            bool          up = false;
            while (nina.NextEvent(event))
            {
                if (event == NinaAt::Event::NetworkUp)
                {
                    up = true;
                }
                else if (event == NinaAt::Event::NetworkDown || event == NinaAt::Event::LinkDown)
                {
                    up = false;
                }
            }
            if (up)
            {
                stateTimeout.Start(NetworkUpSettleMs);
                TransitionTo(State::NetworkUpSettle);
            }
            else if (stateTimeout.Finished())
            {
                Fail();
            }
            break;
        }

        case State::NetworkUpSettle:
        {
            // Quiet-period settle: any further network event restarts the
            // wait, and one that leaves the network down sends us back to
            // waiting for it.
            NinaAt::Event event;
            bool          seen = false;
            bool          up   = true;
            while (nina.NextEvent(event))
            {
                if (event == NinaAt::Event::NetworkUp)
                {
                    seen = true;
                    up   = true;
                }
                else if (event == NinaAt::Event::NetworkDown || event == NinaAt::Event::LinkDown)
                {
                    seen = true;
                    up   = false;
                }
            }
            if (seen && up)
            {
                stateTimeout.Start(NetworkUpSettleMs);
            }
            else if (seen)
            {
                TransitionTo(State::WaitingNetworkUp);
                stateTimeout.Start(NetworkUpWaitMs);
            }
            else if (stateTimeout.Finished())
            {
                TransitionTo(State::ConnectingPeer);
            }
            break;
        }

        case State::ConnectingPeer:
        {
            char cmd[80];
            if (!commandSent)
            {
                size_t n = AppendStr(cmd, 0, sizeof(cmd), "AT+UDCP=\"tcp://");
                n        = AppendStr(cmd, n, sizeof(cmd), config.host);
                n        = AppendStr(cmd, n, sizeof(cmd), ":");
                n        = AppendUInt(cmd, n, sizeof(cmd), static_cast<unsigned>(config.port));
                AppendStr(cmd, n, sizeof(cmd), "/\"");
            }
            const NinaAt::Result result = RunCommand(cmd, ConnectPeerTimeoutMs);
            if (AdvanceOnOk(result, State::WaitingPeerConnected))
            {
                stateTimeout.Start(PeerConnectedWaitMs);
            }
            break;
        }

        case State::WaitingPeerConnected:
        {
            // Same burst handling: judged by end state, not one event at a
            // time. The peer closing is final; the network going down only
            // counts if it did not come back up.
            NinaAt::Event event;
            bool          connected = false;
            bool          peerLost  = false;
            bool          netDown   = false;
            while (nina.NextEvent(event))
            {
                switch (event)
                {
                    case NinaAt::Event::PeerConnected:
                        connected = true;
                        break;
                    case NinaAt::Event::PeerDisconnected:
                        peerLost = true;
                        break;
                    case NinaAt::Event::NetworkDown:
                    case NinaAt::Event::LinkDown:
                        netDown = true;
                        break;
                    case NinaAt::Event::NetworkUp:
                        netDown = false;
                        break;
                    case NinaAt::Event::LinkUp:
                        break; // nothing to decide on a link that is already up
                }
            }
            if (peerLost || netDown || stateTimeout.Finished())
            {
                Fail();
            }
            else if (connected)
            {
                TransitionTo(State::EnteringDataMode);
            }
            break;
        }

        case State::EnteringDataMode:
        {
            const NinaAt::Result result = RunCommand("ATO", DataModeTimeoutMs);
            if (AdvanceOnOk(result, State::DataModeSettle))
            {
                nina.EnterDataMode();
                backoffMs = BackoffInitialMs; // a clean bring-up resets the backoff
                stateTimeout.Start(DataModeSettleMs);
            }
            break;
        }

        case State::DataModeSettle:
            if (stateTimeout.Finished())
            {
                StartSession();
                linkWatchdog.Start(LinkWatchdogMs);
                keepaliveReplyTimer.Stop();
                TransitionTo(State::DataMode);
                NINA_LOG_INFO("Uplink Online");
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
