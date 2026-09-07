/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "Logger.h"

#include "Id.h"
#include "NodeMaster.h"

using NodeLib::ChannelId;
using NodeLib::IVariableHandler;
using NodeLib::Message;
using NodeLib::NodeMaster;
using NodeLib::Operation;

NodeMaster::NodeMaster(const uint8_t                 numNodes,
                       const Hal::UartPins&          uartPins,
                       const uint32_t                baudRate,
                       const Hal::Pin                ledPin,
                       const Hal::Pin                errorLedPin,
                       const std::optional<Hal::Pin> buttonPin) :
    Node(numNodes, uartPins, baudRate, ledPin, errorLedPin, buttonPin),
    activeNodes{},
    nodesFound(false)
{
    nodeId = masterNodeId;

    if (numNodes > maxNodes)
    {
        LOG_ERROR("numNodes exceeds maxNodes cap: " << numNodes);
        errorHandler.Error(false);
    }
}

void NodeMaster::Init(const uint8_t expectedNumNodes)
{
    Node::Init();

    bool nodesOk = false;
    while (!nodesOk)
    {
        DetectNodes();
        nodesOk = ActiveNodeCount() >= expectedNumNodes;

        if (!nodesOk)
        {
            errorHandler.Error(true);
        }
    }
}

void NodeMaster::Loop()
{
    Node::Loop();
}

void NodeLib::NodeMaster::FlushNow(const bool force)
{
    if (force || messagesQueued > (queueSize * 8 / 10))
    {
        flushQueue();
    }
}

void NodeMaster::StartPollingNodes()
{
    // Start polling nodes
    PollNextNode(0);
}

void NodeMaster::DetectNodes()
{
    LOG_INFO("Detecting Nodes");
    const Message poll(0, Operation::DETECTNODES);
    WriteMessage(poll);

    Tools::DelayTimer timeout(static_cast<Tools::time_a>(nodeSpacing * (numNodes + 1)));
    while (timeout.IsRunning() && !timeout.Finished())
    {
        Node::Loop();
    }

    LOG_INFO("Done Detecting Nodes");
}

const uint8_t NodeMaster::ActiveNodeCount() const
{
    uint8_t nodeCount = 0;
    for (const auto& nodeActive : activeNodes)
    {
        if (nodeActive)
        {
            nodeCount++;
        }
    }
    return nodeCount;
}

void NodeMaster::PollNextNode(const int prevNodeId)
{
    // Flush any queued messages
    flushQueue();

    if (nodesFound)
    {
        // Determine next active node
        int nodeId = prevNodeId;
        do
        {
            nodeId++;
            if (nodeId > numNodes)
            {
                nodeId = 1;
            }

        } while (!activeNodes[nodeId - 1]);

        const Message poll(static_cast<uint8_t>(nodeId), Operation::SENDQ);
        WriteMessage(poll);
    }
}

void NodeMaster::HandleInternalOperation(const Message& m)
{
    switch (m.id.operation)
    {
        case Operation::HELLOWORLD:
        {
            NodeHello(m.id.node);
            break;
        }
        case Operation::ENDOFQ:
        {
            PollNextNode(m.id.node);
            ResetHearthBeat();
            break;
        }
        default:
            break;
    }
}

void NodeMaster::HandleMasterMessage(const Message& m)
{
    if (m.id.channel == ChannelId::INTERNAL_MSG)
    {
        HandleInternalOperation(m);
    }
    else
    {
        if (handler)
        {
            handler->ReceivedMessage(m);
        }
    }
}

void NodeMaster::NodeHello(int nodeId)
{
    if (nodeId > 0 && nodeId <= numNodes)
    {
        LOG_INFO("Hello Node " << static_cast<uint8_t>(nodeId));
        activeNodes[nodeId - 1] = true;
        nodesFound              = true;
    }
}
