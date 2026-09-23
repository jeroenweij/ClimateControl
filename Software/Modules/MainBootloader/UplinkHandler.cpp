/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "Tick.h"

#include "../MainController/Secrets.h"
#include "UplinkHandler.h"

using NodeLib::Endpoint;
using NodeLib::Id;
using NodeLib::MAX_DATA;
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

UplinkHandler::UplinkHandler(Boot::Firmware& firmware) :
    nina(),
    firmware(firmware),
    outboundQueue(),
    outboundQueued(0),
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
    state           = next;
    commandSent     = false;
    attemptsInState = 0;
}

void UplinkHandler::Fail()
{
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

bool UplinkHandler::AdvanceOnOk(const NinaAt::Result result, const State next)
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

bool UplinkHandler::WaitForEvent(const NinaAt::Event success, const NinaAt::Event* const failureEvents, const size_t failureCount)
{
    NinaAt::Event event;
    bool          failed = stateTimeout.Finished();
    while (!failed && nina.NextEvent(event))
    {
        if (event == success)
        {
            return true;
        }
        for (size_t i = 0; i < failureCount; i++)
        {
            if (event == failureEvents[i])
            {
                failed = true;
                break;
            }
        }
    }
    if (failed)
    {
        Fail();
    }
    return false;
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
            // Only formatted when actually about to send -- doing it on every
            // waiting-for-reply Loop() call was slow enough to occasionally
            // miss the single-byte RX buffer window (see Uart::Available()'s
            // overrun comment).
            char cmd[64];
            if (!commandSent)
            {
                size_t n = AppendStr(cmd, 0, sizeof(cmd), "AT+UWSC=0,2,\"");
                n        = AppendStr(cmd, n, sizeof(cmd), Secrets::WifiSsid);
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
                n        = AppendStr(cmd, n, sizeof(cmd), Secrets::WifiPassword);
                AppendStr(cmd, n, sizeof(cmd), "\"");
            }
            const NinaAt::Result result = RunCommand(cmd, ConfigTimeoutMs);
            AdvanceOnOk(result, State::ActivatingWifi);
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
            static const NinaAt::Event failures[] = {NinaAt::Event::LinkDown, NinaAt::Event::NetworkDown};
            if (WaitForEvent(NinaAt::Event::NetworkUp, failures, 2))
            {
                stateTimeout.Start(NetworkUpSettleMs);
                TransitionTo(State::NetworkUpSettle);
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
                size_t n = AppendStr(cmd, 0, sizeof(cmd), "AT+UDCP=\"tcp://");
                n        = AppendStr(cmd, n, sizeof(cmd), Secrets::ServerHost);
                n        = AppendStr(cmd, n, sizeof(cmd), ":");
                n        = AppendUInt(cmd, n, sizeof(cmd), static_cast<unsigned>(Secrets::ServerPort));
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
            static const NinaAt::Event failures[] = {NinaAt::Event::PeerDisconnected, NinaAt::Event::LinkDown, NinaAt::Event::NetworkDown};
            if (WaitForEvent(NinaAt::Event::PeerConnected, failures, 3))
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
                helloSent = false;
                linkWatchdog.Start(LinkWatchdogMs);
                TransitionTo(State::DataMode);
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
        Fail();
        return;
    }

    if (keepaliveTimer.Finished())
    {
        SendKeepalive();
        keepaliveTimer.Start(KeepaliveIntervalMs);
    }
}

void UplinkHandler::EnqueueUplink(const Message& message)
{
    if (outboundQueued < outboundQueueSize)
    {
        outboundQueue[outboundQueued] = message;
        outboundQueued++;
    }
    // else: drop -- queue full, same backstop as MainController's own
    // uplink queue (MainController-Server-Link-Spec.md §7.2).
}

void UplinkHandler::DrainOutboundQueue()
{
    for (uint8_t i = 0; i < outboundQueued; i++)
    {
        WriteFrame(outboundQueue[i]);
    }
    outboundQueued = 0;
}

void UplinkHandler::WriteFrame(const Message& m)
{
    // v2 wire format (RS485-Node-Protocol-Spec-STM32G030.md §3): SYNC(2)
    // LEN(1) NODE ENDPOINT OPERATION DATA CRC16.
    uint8_t frameBytes[2 + 1 + 3 + MAX_DATA + 2];
    frameBytes[0] = 0xEE;
    frameBytes[1] = 0x42;
    frameBytes[2] = m.len;
    frameBytes[3] = m.id.node;
    frameBytes[4] = static_cast<uint8_t>(m.id.endpoint);
    frameBytes[5] = static_cast<uint8_t>(m.id.operation);
    for (uint8_t i = 0; i < m.len; i++)
    {
        frameBytes[6 + i] = m.data[i];
    }

    const uint8_t  headerAndDataLen = static_cast<uint8_t>(3 + m.len);
    const uint16_t frameCrc         = uplinkCrc.Compute(&frameBytes[3], headerAndDataLen);
    frameBytes[6 + m.len]           = static_cast<uint8_t>(frameCrc);
    frameBytes[7 + m.len]           = static_cast<uint8_t>(frameCrc >> 8);

    nina.WriteBytes(frameBytes, static_cast<size_t>(8 + m.len));
}

void UplinkHandler::HandleUplinkFrame(const Message& message)
{
    if (IsRelayedEndpoint(message.id.endpoint))
    {
        // No bus while resident in the bootloader -- nothing to relay to.
        return;
    }

    if (message.id.endpoint == Endpoint::OtaControl)
    {
        firmware.OnControl(message);
    }
    else if (message.id.endpoint == Endpoint::OtaData)
    {
        firmware.OnData(message);
    }
    else if (message.id.endpoint == Endpoint::Keepalive && message.id.operation == Operation::Get)
    {
        EnqueueUplink(Message(Id(0, Endpoint::Keepalive, Operation::Report)));
    }
    // Roster is future work (Open item, MainController-Server-Link-Spec.md §11).

    Message reply;
    if (firmware.PopReply(reply))
    {
        EnqueueUplink(reply);
    }
}

void UplinkHandler::SendUplinkHello()
{
    uint8_t payload[23];

    PackU16(&payload[0], 0);
    PackU32(&payload[2], Hal::Tick::Millis() / 1000u);
    payload[6] = 0;
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

void UplinkHandler::SendKeepalive()
{
    EnqueueUplink(Message(Id(0, Endpoint::Keepalive, Operation::Get)));
}
