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

NodeMaster::NodeMaster() :
    Node(),
    state(EMasterState::Start),
    activeNodes{},
    nodeModules{},
    nodesFound(false),
    pollTimeout(),
    pendingPollNode(0),
    timeoutTimer(),
    detectTimer(),
    pollGapTimer()
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

    switch (state)
    {
        case EMasterState::Start:
            break;

        case EMasterState::Detecting:
            if (timeoutTimer.Finished())
            {
                state = EMasterState::Flush;
                LOG_INFO("Done Detecting Nodes " << ActiveNodeCount());
            }
            break;

        case EMasterState::Flush:
        {
            if (messagesQueued > 0)
            {
                flushQueue();
                state = EMasterState::Pollgap;
                pollGapTimer.Start(pollGapMs);
            }
            else
            {
                PollNextNode(pendingPollNode);
            }
            break;
        }

        case EMasterState::Pollgap:
            if (pollGapTimer.Finished())
            {
                PollNextNode(pendingPollNode);
            }
            break;

        case EMasterState::Polling:
            if (pollTimeout.Finished())
            {
                activeNodes[pendingPollNode - 1] = false;
                nodesFound                       = ActiveNodeCount() > 0;
                state                            = EMasterState::Flush;
            }

            break;
    }
}

void NodeMaster::DetectNodes()
{
    LOG_INFO("Detecting Nodes");
    state = EMasterState::Detecting;
    detectTimer.Start(detectIntervalMs);
    const Message poll(BROADCAST_NODE, Operation::Discover);
    WriteMessage(poll);

    timeoutTimer.Start(static_cast<Tools::time_a>(nodeSpacing * (maxNodes + 1)));
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
    if (detectTimer.Finished())
    {
        DetectNodes();
        return;
    }

    if (nodesFound)
    {
        state = EMasterState::Polling;

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

        pendingPollNode = nodeId;
        pollTimeout.Start(pollTimeoutMs);
    }
    else
    {
        state = EMasterState::Flush;
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
            if (m.id.node == pendingPollNode)
            {
                state = EMasterState::Flush;
                ResetHearthBeat();
            }
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
