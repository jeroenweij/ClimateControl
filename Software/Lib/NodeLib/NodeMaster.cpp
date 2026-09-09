/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "Logger.h"

#include "Id.h"
#include "NodeMaster.h"

using NodeLib::Endpoint;
using NodeLib::Message;
using NodeLib::NodeMaster;
using NodeLib::Operation;

NodeMaster::NodeMaster(const uint32_t baudRate) :
    Node(baudRate),
    activeNodes{},
    nodeModules{},
    nodesFound(false)
{
    nodeId = masterNodeId;
}

uint8_t NodeMaster::NodeModule(const uint8_t nodeId) const
{
    if (nodeId < 1 || nodeId > maxNodes)
    {
        return 0;
    }
    return nodeModules[nodeId - 1];
}

void NodeMaster::Init()
{
    Node::Init();

    DetectNodes();
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
    const Message poll(BROADCAST_NODE, Operation::Discover);
    WriteMessage(poll);

    Tools::DelayTimer timeout(static_cast<Tools::time_a>(nodeSpacing * (maxNodes + 1)));
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
            if (nodeId > maxNodes)
            {
                nodeId = 1;
            }

        } while (!activeNodes[nodeId - 1]);

        const Message poll(static_cast<uint8_t>(nodeId), Operation::Poll);
        WriteMessage(poll);
    }
}

void NodeMaster::HandleInternalOperation(const Message& m)
{
    switch (m.id.operation)
    {
        case Operation::Announce:
        {
            NodeHello(m.id.node, m.len >= 1 ? m.data[0] : 0);
            break;
        }
        case Operation::Done:
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
    if (m.id.endpoint == Endpoint::Transport)
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

void NodeMaster::NodeHello(int nodeId, uint8_t module)
{
    if (nodeId > 0 && nodeId <= maxNodes)
    {
        LOG_INFO("Hello Node " << static_cast<uint8_t>(nodeId) << " module " << module);
        activeNodes[nodeId - 1] = true;
        nodeModules[nodeId - 1] = module;
        nodesFound              = true;
    }
}
