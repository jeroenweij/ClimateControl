/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include <stdio.h>

#include "Backup.h"
#include "BoardPins.h"
#include "ImageDescriptor.h"
#include "LogRing.h"
#include "Logger.h"
#include "MemoryMap.h"
#include "System.h"
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
    nodePresenceActive{},
    nodePresenceBootloader{},
    port(),
    config{Secrets::WifiSsid, Secrets::WifiPassword, Secrets::ServerHost, static_cast<uint16_t>(Secrets::ServerPort)},
    link(port, config, *this, outboundQueue, outboundQueueSize),
    resetPending(false),
    resetToBootloader(false),
    errorLed(Board::ErrorLed, Hal::Gpio::Mode::Output),
    uplinkShown(false),
    uplinkUp(false)
{
}

void UplinkHandler::Init()
{
    link.Init();
}

void UplinkHandler::Loop()
{
    link.Loop();
    ShowUplinkState(link.InDataMode());
}

void UplinkHandler::ShowUplinkState(const bool up)
{
    if (uplinkShown && up == uplinkUp)
    {
        return;
    }
    uplinkShown = true;
    uplinkUp    = up;
    errorLed.Write(!up);
}

void UplinkHandler::EnqueueUplink(const Message& message)
{
    link.Send(message);
}

void UplinkHandler::OnFrame(const Message& message)
{
    HandleUplinkFrame(message);
}

void UplinkHandler::OnConnected()
{
    SendRoster();
}

void UplinkHandler::BeforeFrames()
{
    // Only after the roster dump has seeded the snapshot -- on the very pass
    // that sent it this would compare against last session's stale one.
    CheckNodePresence();
    // Likewise after the hello, so lines logged while the link was down
    // (bring-up, boot) flush right behind it.
    PushLog();
}

void UplinkHandler::AfterFrames()
{
    if (resetPending)
    {
        PerformPendingReset(); // never returns
    }
}

void UplinkHandler::BuildHello(Message& hello)
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

    hello     = Message(Id(0, Endpoint::UplinkHello, Operation::Report));
    hello.len = sizeof(payload);
    for (uint8_t i = 0; i < hello.len; i++)
    {
        hello.data[i] = payload[i];
    }
}

void UplinkHandler::HandleUplinkFrame(const Message& message)
{
    // NODE 0 is this MainController, never a bus node -- a relayed-range
    // endpoint addressed to it is for us, not for the bus.
    if (message.id.node == 0 && message.id.endpoint == Endpoint::SystemControl)
    {
        HandleSelfControl(message);
        return;
    }

    if (IsRelayedEndpoint(message.id.endpoint))
    {
        master.QueueMessage(message);
        return;
    }

    // Uplink-block frame from the server: Keepalive and an on-demand Roster
    // refresh are answered here. OtaControl/OtaData belong to MainBootloader,
    // never the running app.
    if (message.id.endpoint == Endpoint::Keepalive && message.id.operation == Operation::Get)
    {
        EnqueueUplink(Message(Id(0, Endpoint::Keepalive, Operation::Report)));
    }
    else if (message.id.endpoint == Endpoint::Roster && message.id.operation == Operation::Get)
    {
        SendRoster();
    }
}

void UplinkHandler::HandleSelfControl(const Message& message)
{
    if (message.id.operation != Operation::Set || message.len < 1 || (message.data[0] != 1 && message.data[0] != 2))
    {
        EnqueueUplink(Message(Id(0, Endpoint::SystemControl, Operation::Nack)));
        return;
    }

    EnqueueUplink(Message(Id(0, Endpoint::SystemControl, Operation::Ack)));
    resetToBootloader = (message.data[0] == 2);
    resetPending      = true;
}

void UplinkHandler::PerformPendingReset()
{
    LOG_INFO("Commanded reset (" << (resetToBootloader ? "bootloader" : "app") << ")");
    if (resetToBootloader)
    {
        Hal::Backup::Write(Hal::Backup::Reg::Boot, Board::EnterBootloaderMagic);
    }

    // The Ack was only queued into the TX ring buffer by DrainOutboundQueue()
    // -- wait for it to actually leave the UART before the reset kills the
    // peripheral (same reasoning as Node::PerformPendingReset()).
    link.Flush();

    Hal::System::Reset(); // never returns
    while (true)
    {
    }
}

void UplinkHandler::SendRoster()
{
    // One Report per active node -- nodeId(1) module(1) state(1)
    // lastSeenMs(4) -- terminated by nodeId 0xFF (MainController-Server-Link-
    // Spec.md §5). state is just the bootloader bit for now; Roster's shape
    // leaves room to widen it later without another wire change.
    //
    // Also (re)seeds nodePresenceActive/nodePresenceBootloader for every id,
    // active or not, so the CheckNodePresence() right after this always sees
    // "nothing changed" -- this dump just reported the current truth.
    for (uint8_t nodeId = 1; nodeId <= NodeLib::MAX_NODES; nodeId++)
    {
        const bool active                  = master.NodeActive(nodeId);
        const bool bootloader              = master.NodeInBootloader(nodeId);
        nodePresenceActive[nodeId - 1]     = active;
        nodePresenceBootloader[nodeId - 1] = bootloader;

        if (!active)
        {
            continue;
        }

        uint8_t payload[7];
        payload[0] = nodeId;
        payload[1] = static_cast<uint8_t>(master.NodeModule(nodeId));
        payload[2] = bootloader ? 1 : 0;
        PackU32(&payload[3], master.NodeLastContactMs(nodeId));

        Message entry(Id(0, Endpoint::Roster, Operation::Report));
        entry.len = sizeof(payload);
        for (uint8_t i = 0; i < entry.len; i++)
        {
            entry.data[i] = payload[i];
        }
        EnqueueUplink(entry);
    }

    EnqueueUplink(Message(Id(0, Endpoint::Roster, Operation::Report), static_cast<uint8_t>(0xFF)));
}

void UplinkHandler::CheckNodePresence()
{
    for (uint8_t nodeId = 1; nodeId <= NodeLib::MAX_NODES; nodeId++)
    {
        const uint8_t idx        = nodeId - 1;
        const bool    active     = master.NodeActive(nodeId);
        const bool    bootloader = master.NodeInBootloader(nodeId);
        if (active == nodePresenceActive[idx] && bootloader == nodePresenceBootloader[idx])
        {
            continue;
        }
        nodePresenceActive[idx]     = active;
        nodePresenceBootloader[idx] = bootloader;

        const uint8_t payload[4] = {
            nodeId,
            static_cast<uint8_t>(master.NodeModule(nodeId)),
            static_cast<uint8_t>(active ? 1 : 0),
            static_cast<uint8_t>(bootloader ? 1 : 0),
        };
        Message m(Id(0, Endpoint::NodePresence, Operation::Report));
        m.len = sizeof(payload);
        for (uint8_t i = 0; i < m.len; i++)
        {
            m.data[i] = payload[i];
        }
        EnqueueUplink(m);
    }
}

void UplinkHandler::PushLog()
{
    for (uint8_t i = 0; i < maxLogLinesPerPass; i++)
    {
        if (link.Queued() >= outboundQueueSize / 2)
        {
            return; // bus frames first -- the ring keeps the lines until there's room
        }

        // uptimeSec(3 LE) then the text, the same layout as a bus node's
        // DiagLog Report.
        Message      line(Id(0, Endpoint::MainLog, Operation::Report));
        uint32_t     uptimeSec = 0;
        const size_t length    = Tools::LogRing::Pop(&line.data[3], Tools::LogRing::LineSize, uptimeSec);
        if (length == 0)
        {
            return; // drained
        }
        line.data[0] = static_cast<uint8_t>(uptimeSec);
        line.data[1] = static_cast<uint8_t>(uptimeSec >> 8);
        line.data[2] = static_cast<uint8_t>(uptimeSec >> 16);
        line.len     = static_cast<uint8_t>(3 + length);
        EnqueueUplink(line);
    }
}

void UplinkHandler::ReceivedMessage(const Message& message)
{
    // Bus-side supervision runs regardless of uplink state -- see the class
    // comment.
    budgetAllocator.Observe(message);

    // Called synchronously from NodeMaster's bus receive path (see the class
    // comment in UplinkHandler.h) -- must only enqueue, never block on NINA.
    if (!link.InDataMode())
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
