/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "Backup.h"
#include "BoardPins.h"
#include "ImageDescriptor.h"
#include "Logger.h"
#include "MemoryMap.h"
#include "System.h"
#include "Tick.h"

#include "ConfigStore.h"
#include "EFirmware.h"
#include "Node.h"

using NodeLib::ConfigStore;
using NodeLib::Endpoint;
using NodeLib::FirmwareOp;
using NodeLib::Id;
using NodeLib::INodeHandler;
using NodeLib::Message;
using NodeLib::Node;
using NodeLib::Operation;
using NodeLib::RxCounters;
using NodeLib::SystemStatus;

// Firmware version is baked into the per-module image descriptor at link time
// (Lib/Board/ImageDescriptor.h). The bootloader does not link this TU.
extern "C" const Board::ImageDescriptor gImageDescriptor;

namespace
{
    void PackU32(uint8_t* const out, const uint32_t value)
    {
        out[0] = static_cast<uint8_t>(value);
        out[1] = static_cast<uint8_t>(value >> 8);
        out[2] = static_cast<uint8_t>(value >> 16);
        out[3] = static_cast<uint8_t>(value >> 24);
    }
} // namespace

Node::Node(const uint32_t baudRate) :
    Node(baudRate, Hal::Uart::Instance::Usart1, Board::BusUart)
{
}

Node::Node(const uint32_t baudRate, const Hal::Uart::Instance instance, const Hal::UartPins& pins) :
    errorHandler(),
    handler(nullptr),
    nodeId(99), // sentinel until Init() reads it from flash, or NodeMaster sets 0
    messagesQueued(0),
    baudRate(baudRate),
    busInstance(instance),
    busPins(pins),
    txFrames(0),
    queueDrops(0),
    resetPending(false),
    resetToBootloader(false),
    identifyLedOn(false),
    uart(),
    crc(),
    frame(crc),
    led(Board::ActivityLed, Hal::Gpio::Mode::Output),
    messageQueue{},
    hearthBeatTimer(),
    identifyUntil(),
    identifyToggle()
{
    led.Write(false);
}

void Node::WriteMessage(const Message& m)
{
    LOG_DEBUG("WRITE Node: " << m.id.node << " Endpoint: " << m.id.endpoint << " Op: " << m.id.operation
                             << " Len: " << m.len);
    frame.Write(uart, m);
    txFrames++;
}

void Node::Init()
{
    // Master keeps the reserved id 0 (set in NodeMaster's constructor). A slave
    // takes its permanent bus address from the factory-provisioned flash record
    // -- see Node-Flash-Layout-and-Bootloader-Spec.md Sec6.3.
    if (nodeId != masterNodeId)
    {
        if (!ConfigStore::Valid())
        {
            LOG_ERROR("No valid node identity in flash -- node not provisioned");
            errorHandler.Error(false); // never returns
        }

        nodeId = ConfigStore::NodeId();
        LOG_INFO("Node identity from flash: " << nodeId);

        if (nodeId == masterNodeId || nodeId > maxNodes)
        {
            LOG_ERROR("Provisioned Node Id out of range: " << nodeId);
            errorHandler.Error(false); // never returns
        }
    }

    uart.Init(baudRate, busInstance, busPins);
}

void Node::PumpRx()
{
    while (uart.Available())
    {
        Message received;
        if (frame.FeedByte(uart.ReadByte(), received))
        {
            ReadMessage(received);
        }
    }
    frame.Update();
}

void Node::Loop()
{
    PumpRx();
    ServiceIdentify();

    if (hearthBeatTimer.Finished() && handler != nullptr)
    {
        handler->ConnectionLost();
    }
}

void Node::ResetHearthBeat()
{
    hearthBeatTimer.Start(500);
}

bool Node::ReadMessage(const Message& m)
{
    LOG_DEBUG("READ  Node: " << m.id.node << " Endpoint: " << m.id.endpoint << " Op: " << m.id.operation
                             << " Len: " << m.len);

    if (nodeId != masterNodeId)
    {
        HandleMessage(m);
    }
    else
    {
        HandleMasterMessage(m);
    }

    return true;
}

void Node::HandleMessage(const Message& m)
{
    // Discovery is a broadcast (node == BROADCAST_NODE) -- recognised by its
    // operation, before the address match.
    if (m.id.operation == Operation::Discover)
    {
        HandlePollRequest();
        return;
    }

    const bool addressedToUs = (m.id.node == nodeId);
    // Broadcast is Set-only and never answered (Spec/Node-Message-Model-Spec.md §2).
    const bool broadcastSet = (m.id.node == BROADCAST_NODE && m.id.operation == Operation::Set);

    if (!addressedToUs && !broadcastSet)
    {
        return;
    }

    // One dispatch on the endpoint's high-nibble block. Transport / System /
    // Firmware / Diagnostics are NodeLib-owned; only Application (0x3_) and Room
    // (0x4_) reach the module's handler (Node-Message-Model-Spec.md §3/§6).
    switch (static_cast<uint8_t>(m.id.endpoint) & 0xF0)
    {
        case static_cast<uint8_t>(Endpoint::Transport):
            HandleInternalMessage(m);
            break;
        case static_cast<uint8_t>(Endpoint::SystemInfo) & 0xF0:
            HandleSystemMessage(m);
            break;
        case static_cast<uint8_t>(Endpoint::Firmware) & 0xF0:
            // Endpoint::Firmware is NodeLib-owned; ThermostatFirmware (and any
            // future app endpoint in this block) goes to the module handler.
            if (m.id.endpoint == Endpoint::Firmware)
            {
                HandleFirmwareMessage(m);
            }
            else if (handler)
            {
                handler->ReceivedMessage(m);
            }
            break;
        case static_cast<uint8_t>(Endpoint::DiagRxCounters) & 0xF0:
            HandleDiagnosticsMessage(m);
            break;
        default:
            if (handler)
            {
                handler->ReceivedMessage(m);
            }
            break;
    }
}

void Node::HandleInternalMessage(const Message& m)
{
    LOG_DEBUG("Handle internal operation: " << m.id.operation);
    switch (m.id.operation)
    {
        case Operation::Poll:
        {
            flushQueue();
            ResetHearthBeat();
            break;
        }
        default:
            // Do nothing
            break;
    }
}

void Node::HandleSystemMessage(const Message& m)
{
    switch (m.id.endpoint)
    {
        case Endpoint::SystemInfo:
        {
            if (m.id.operation != Operation::Get)
            {
                SendNack(m);
                break;
            }
            // module(1) hwRev(1) fwVersionMajor(2 LE) fwVersionMinor(2 LE).
            // No UID -- physical-board id is not carried on the bus.
            const uint8_t info[6] = {
                static_cast<uint8_t>(ConfigStore::GetModule()),
                0, // hwRev -- TODO: strap pin / ConfigRecord settings
                static_cast<uint8_t>(gImageDescriptor.fwVersionMajor),
                static_cast<uint8_t>(gImageDescriptor.fwVersionMajor >> 8),
                static_cast<uint8_t>(gImageDescriptor.fwVersionMinor),
                static_cast<uint8_t>(gImageDescriptor.fwVersionMinor >> 8),
            };
            SendReport(Endpoint::SystemInfo, info, sizeof(info));
            break;
        }

        case Endpoint::SystemStatus:
        {
            if (m.id.operation != Operation::Get)
            {
                SendNack(m);
                break;
            }
            SystemStatus status{};
            if (handler)
            {
                handler->FillStatus(status);
            }
            const uint32_t uptimeSec = Hal::Tick::Millis() / 1000u;

            uint8_t payload[8];
            payload[0] = status.state;
            PackU32(&payload[1], uptimeSec);
            payload[5] = static_cast<uint8_t>(status.errorFlags);
            payload[6] = static_cast<uint8_t>(status.errorFlags >> 8);
            payload[7] = Hal::System::ResetCause();
            SendReport(Endpoint::SystemStatus, payload, sizeof(payload));
            break;
        }

        case Endpoint::SystemControl:
        {
            if (m.id.operation != Operation::Set || m.len < 1)
            {
                SendNack(m);
                break;
            }
            switch (m.data[0])
            {
                case 1: // reset -> application
                    SendAck(m);
                    RequestReset(false);
                    break;
                case 2: // reset -> bootloader
                    SendAck(m);
                    RequestReset(true);
                    break;
                case 3: // identify: blink for data[1] seconds (default 5)
                    StartIdentify(m.len >= 2 ? m.data[1] : 5);
                    SendAck(m);
                    break;
                default:
                    SendNack(m);
                    break;
            }
            break;
        }

        default:
            break;
    }
}

void Node::HandleFirmwareMessage(const Message& m)
{
    // A running app only ever honours EnterBootloader here -- the Begin/Write/
    // End/Activate transfer is served by the bootloader
    // (Node-Flash-Layout-and-Bootloader-Spec.md §6.2).
    if (m.id.operation == Operation::Set && m.len >= 1 &&
        static_cast<FirmwareOp>(m.data[0]) == FirmwareOp::EnterBootloader)
    {
        SendAck(m);
        RequestReset(true);
        return;
    }
    SendNack(m);
}

void Node::HandleDiagnosticsMessage(const Message& m)
{
    switch (m.id.endpoint)
    {
        case Endpoint::DiagRxCounters:
        {
            if (m.id.operation != Operation::Get)
            {
                SendNack(m);
                break;
            }
            const RxCounters& rx = frame.Counters();
            uint8_t           payload[16];
            PackU32(&payload[0], rx.frames);
            PackU32(&payload[4], rx.crcErrors);
            PackU32(&payload[8], rx.resyncs);
            PackU32(&payload[12], rx.interByteTimeouts);
            SendReport(Endpoint::DiagRxCounters, payload, sizeof(payload));
            break;
        }

        case Endpoint::DiagTxCounters:
        {
            if (m.id.operation != Operation::Get)
            {
                SendNack(m);
                break;
            }
            uint8_t payload[8];
            PackU32(&payload[0], txFrames);
            PackU32(&payload[4], queueDrops);
            SendReport(Endpoint::DiagTxCounters, payload, sizeof(payload));
            break;
        }

        case Endpoint::DiagLastError:
        {
            if (m.id.operation != Operation::Get)
            {
                SendNack(m);
                break;
            }
            // code(1) uptimeAtFault(4) context(2). TODO: capture in ErrorHandler
            // -- a non-recoverable Error() halts the CPU so this only ever
            // reports recoverable faults; zeroed until that path exists.
            const uint8_t payload[7] = {0, 0, 0, 0, 0, 0, 0};
            SendReport(Endpoint::DiagLastError, payload, sizeof(payload));
            break;
        }

        case Endpoint::DiagLog:
        {
            if (m.id.operation != Operation::Get)
            {
                SendNack(m);
                break;
            }
            // Empty => log drained. TODO: a small line ring buffer fed by
            // Tools::Logger, drained one line per Get.
            SendReport(Endpoint::DiagLog, nullptr, 0);
            break;
        }

        case Endpoint::DiagReset:
        {
            if (m.id.operation != Operation::Set)
            {
                SendNack(m);
                break;
            }
            frame.ResetCounters();
            txFrames   = 0;
            queueDrops = 0;
            SendAck(m);
            break;
        }

        default:
            break;
    }
}

void Node::SendReport(const Endpoint endpoint, const uint8_t* const data, const uint8_t len)
{
    QueueMessage(Id(nodeId, endpoint, Operation::Report), data, len);
}

void Node::SendAck(const Message& m)
{
    if (m.id.node == BROADCAST_NODE) // a broadcast is never answered (§2)
    {
        return;
    }
    QueueMessage(Id(nodeId, m.id.endpoint, Operation::Ack), static_cast<uint8_t>(0));
}

void Node::SendNack(const Message& m)
{
    if (m.id.node == BROADCAST_NODE)
    {
        return;
    }
    QueueMessage(Id(nodeId, m.id.endpoint, Operation::Nack), static_cast<uint8_t>(0));
}

void Node::RequestReset(const bool toBootloader)
{
    resetPending      = true;
    resetToBootloader = toBootloader;
}

void Node::PerformPendingReset()
{
    LOG_INFO("Commanded reset (" << (resetToBootloader ? "bootloader" : "app") << ")");
    if (handler)
    {
        handler->PrepareForReset();
    }
    if (resetToBootloader)
    {
        Hal::Backup::Write(Hal::Backup::Reg::Boot, Board::EnterBootloaderMagic);
    }
    Hal::System::Reset(); // never returns
    while (true)
    {
    }
}

void Node::StartIdentify(uint8_t seconds)
{
    if (seconds == 0)
    {
        seconds = 1;
    }
    if (seconds > 30)
    {
        seconds = 30;
    }
    LOG_INFO("Identify for " << seconds << "s");
    identifyUntil.Start(static_cast<Tools::time_a>(seconds) * 1000u);
    identifyToggle.Start(120);
    identifyLedOn = true;
    led.Write(true);
}

void Node::ServiceIdentify()
{
    if (!identifyUntil.IsRunning())
    {
        return;
    }
    if (identifyUntil.Finished())
    {
        identifyLedOn = false;
        led.Write(false);
        return;
    }
    if (identifyToggle.Finished())
    {
        identifyLedOn = !identifyLedOn;
        led.Write(identifyLedOn);
        identifyToggle.Start(120);
    }
}

void Node::flushQueue()
{
    LOG_DEBUG("Flush queue");

    // v1 rate-limited this loop (a delay every 5 messages) because the receiver
    // had to keep up with software-timed DE toggling. Hardware DE removes that
    // constraint (protocol spec §8), so the limiter is dropped here rather than
    // ported as-is.
    led.Write(true);
    for (int i = 0; i < messagesQueued; i++)
    {
        WriteMessage(messageQueue[i]);
    }
    messagesQueued = 0;

    if (nodeId != masterNodeId)
    {
        const Message end(nodeId, Operation::Done);
        WriteMessage(end);
    }
    led.Write(false);

    // A commanded reset waits until here so the Ack + Done actually reach the
    // master before the MCU goes down.
    if (resetPending && nodeId != masterNodeId)
    {
        PerformPendingReset(); // never returns
    }
}

void Node::HandlePollRequest()
{
    LOG_INFO("Handle poll request");
    if (nodeId != masterNodeId)
    {
        // Announce carries the module type (data[0]) so one discovery sweep
        // gives the master a typed roster -- no per-node SystemInfo round-trip
        // (Spec/Node-Message-Model-Spec.md §4).
        Message m(nodeId, Operation::Announce);
        m.data[0] = static_cast<uint8_t>(ConfigStore::GetModule());
        m.len     = 1;
        Hal::Tick::DelayMs(static_cast<uint32_t>((nodeId - 1) * nodeSpacing));
        LOG_INFO("Return Announce");
        WriteMessage(m);
    }
}

void Node::RegisterHandler(INodeHandler* handler)
{
    this->handler = handler;
}

uint8_t Node::GetId()
{
    return nodeId;
}

void Node::QueueMessage(const Id& id, const uint8_t value)
{
    QueueMessage(id, &value, 1);
}

void Node::QueueMessage(const Id& id, const uint8_t* const data, const uint8_t len)
{
    Message m(id);
    m.len = len;
    for (uint8_t i = 0; i < len && i < MAX_DATA; i++)
    {
        m.data[i] = data[i];
    }
    QueueMessage(m);
}

void Node::QueueMessage(const NodeLib::Message& m)
{
    if (messagesQueued < queueSize)
    {
        messageQueue[messagesQueued] = m;
        messagesQueued++;
    }
    else
    {
        queueDrops++;
    }
}

NodeLib::DiagCounters Node::Counters() const
{
    return {
        frame.Counters(),
        txFrames,
        queueDrops,
    };
}
