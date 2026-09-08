/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "BoardPins.h"
#include "Logger.h"
#include "Tick.h"

#include "ConfigStore.h"
#include "Node.h"

using NodeLib::ChannelId;
using NodeLib::ConfigStore;
using NodeLib::Id;
using NodeLib::Message;
using NodeLib::Node;
using NodeLib::Operation;

Node::Node(const uint8_t numNodes, const uint32_t baudRate) :
    errorHandler(),
    numNodes(numNodes),
    handler(nullptr),
    nodeId(99), // sentinel until Init() reads it from flash, or NodeMaster sets 0
    messagesQueued(0),
    baudRate(baudRate),
    uart(),
    crc(),
    frame(crc),
    led(Board::ActivityLed, Hal::Gpio::Mode::Output),
    messageQueue{},
    hearthBeatTimer()
{
    led.Write(false);
}

void Node::WriteMessage(const Message& m)
{
    LOG_DEBUG("WRITE Node: " << m.id.node << " Chan: " << m.id.channel << " Op: " << m.id.operation << " Len: " << m.len);
    frame.Write(uart, m);
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

        if (nodeId == masterNodeId || nodeId > numNodes)
        {
            LOG_ERROR("Provisioned Node Id out of range: " << nodeId);
            errorHandler.Error(false); // never returns
        }
    }

    uart.Init(baudRate, Board::BusUart);
}

void Node::Loop()
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
    LOG_DEBUG("READ  Node: " << m.id.node << " Chan: " << m.id.channel << " Op: " << m.id.operation << " Len: " << m.len);

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
    if (m.id.node == nodeId)
    {
        if (m.id.channel == ChannelId::INTERNAL_MSG)
        {
            HandleInternalMessage(m);
        }
        else
        {
            if (handler)
            {
                handler->ReceivedMessage(m);
            }
        }
    }
    // Poll request from master node
    else if (m.id.node == masterNodeId && m.id.operation == Operation::DETECTNODES)
    {
        HandlePollRequest();
    }
}

void Node::HandleInternalMessage(const Message& m)
{
    LOG_DEBUG("Handle internal operation: " << m.id.operation);
    switch (m.id.operation)
    {
        case Operation::SENDQ:
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
        const Message end(nodeId, Operation::ENDOFQ);
        WriteMessage(end);
    }
    led.Write(false);
}

void Node::HandlePollRequest()
{
    LOG_INFO("Handle poll request");
    if (nodeId != masterNodeId)
    {
        const Message m(nodeId, Operation::HELLOWORLD);
        Hal::Tick::DelayMs(static_cast<uint32_t>((nodeId - 1) * nodeSpacing));
        LOG_INFO("Return Hello world");
        WriteMessage(m);
    }
}

void Node::RegisterHandler(IVariableHandler* handler)
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
}
