/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "Logger.h"
#include "Tick.h"

#include "Id.h"
#include "NodeMaster.h"

using NodeLib::Endpoint;
using NodeLib::Message;
using NodeLib::ModuleType;
using NodeLib::NodeMaster;
using NodeLib::Operation;

NodeMaster::NodeMaster() :
    Node(),
    state(EMasterState::Start),
    slaveNodes{},
    nodesFound(false),
    pollTimeout(),
    pendingPollNode(0),
    timeoutTimer(),
    detectTimer(),
    pollGapTimer()
{
    nodeId = masterNodeId;
}

NodeMaster::SNode::SNode() :
    active(false),
    moduleType(ModuleType::Unknown),
    inBootloader(0),
    lastContactMs(0)
{
}

ModuleType NodeMaster::NodeModule(const uint8_t nodeId) const
{
    if (nodeId < 1 || nodeId > maxNodes)
    {
        return ModuleType::Unknown;
    }
    return slaveNodes[nodeId - 1].moduleType;
}

bool NodeMaster::NodeActive(const uint8_t nodeId) const
{
    if (nodeId < 1 || nodeId > maxNodes)
    {
        return false;
    }
    return slaveNodes[nodeId - 1].active;
}

bool NodeMaster::NodeInBootloader(const uint8_t nodeId) const
{
    if (nodeId < 1 || nodeId > maxNodes)
    {
        return false;
    }
    return slaveNodes[nodeId - 1].inBootloader > 0;
}

uint32_t NodeMaster::NodeLastContactMs(const uint8_t nodeId) const
{
    if (nodeId < 1 || nodeId > maxNodes)
    {
        return 0;
    }
    return slaveNodes[nodeId - 1].lastContactMs;
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
                LOG_INFO("Done Detected " << ActiveNodeCount() << " Nodes");
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
                if (slaveNodes[pendingPollNode - 1].inBootloader)
                {
                    state = EMasterState::Flush;
                    ResetHearthBeat();
                    slaveNodes[pendingPollNode - 1].inBootloader--;
                    break;
                }
                LOG_WARN("Lost Node " << pendingPollNode);
                slaveNodes[pendingPollNode - 1].active = false;
                nodesFound                             = ActiveNodeCount() > 0;
                state                                  = EMasterState::Flush;
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

uint8_t NodeMaster::ActiveNodeCount() const
{
    uint8_t nodeCount = 0;
    for (const auto& nodeActive : slaveNodes)
    {
        if (nodeActive.active)
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

        } while (!slaveNodes[nodeId - 1].active);
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
            ModuleType type = m.len >= 1 ? static_cast<ModuleType>(m.data[0]) : ModuleType::Unknown;
            NodeHello(m.id.node, type, m.len >= 2 ? m.data[1] > 0 : false);
            break;
        }
        case Operation::Done:
        {
            if (m.id.node == pendingPollNode)
            {
                state = EMasterState::Flush;
                ResetHearthBeat();
                slaveNodes[pendingPollNode - 1].lastContactMs = Hal::Tick::Millis();
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

void NodeMaster::NodeHello(int nodeId, ModuleType module, bool bootloader)
{
    if (nodeId > 0 && nodeId <= maxNodes)
    {
        LOG_INFO("Found Node " << static_cast<uint8_t>(nodeId) << " m " << module << " " << (bootloader ? "Bootloader" : ""));
        slaveNodes[nodeId - 1].active        = true;
        slaveNodes[nodeId - 1].moduleType    = module;
        slaveNodes[nodeId - 1].inBootloader  = bootloader ? 200 : 0;
        slaveNodes[nodeId - 1].lastContactMs = Hal::Tick::Millis();
        nodesFound                           = true;
    }
}
