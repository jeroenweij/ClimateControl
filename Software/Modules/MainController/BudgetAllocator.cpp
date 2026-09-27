/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "ConfigStore.h"
#include "EEndpoint.h"
#include "EModuleType.h"
#include "EOperation.h"
#include "RoomDemand.h"

#include "BudgetAllocator.h"

using NodeLib::ConfigStore;
using NodeLib::Endpoint;
using NodeLib::Id;
using NodeLib::Message;
using NodeLib::ModuleType;
using NodeLib::NodeMaster;
using NodeLib::Operation;

namespace
{
    int16_t ReadI16(const uint8_t* const p)
    {
        return static_cast<int16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
    }

    // Round to nearest, not floor -- a plain '/' here would understate every
    // node's share a little, and the understatement compounds across the
    // proportional split and the water-fill redistribution below.
    uint32_t DivRoundNearest(const uint32_t numerator, const uint32_t denominator)
    {
        return (numerator + denominator / 2) / denominator;
    }
} // namespace

BudgetAllocator::SRoom::SRoom() :
    sawTemp(false),
    sawSetpoint(false),
    sawBudget(false),
    temp(0),
    setpoint(0),
    budget(0)
{
}

bool BudgetAllocator::SRoom::Known() const
{
    return sawTemp && sawSetpoint;
}

BudgetAllocator::BudgetAllocator(NodeMaster& master) :
    master(master),
    supplyTemp(),
    room{},
    recomputeTimer()
{
}

void BudgetAllocator::Observe(const Message& m)
{
    if (m.id.operation != Operation::Report)
    {
        return;
    }

    if (m.id.node == 0 || m.id.node > NodeLib::MAX_NODES)
    {
        return;
    }

    const uint8_t nodeIndex = m.id.node - 1;
    switch (m.id.endpoint)
    {
        case Endpoint::SupplyTemp:
            supplyTemp.Snoop(m);
            break;

        case Endpoint::RoomTemp:
            if (m.len >= 2)
            {
                room[nodeIndex].temp    = ReadI16(m.data);
                room[nodeIndex].sawTemp = true;
            }
            break;

        case Endpoint::RoomSetpoint:
            if (m.len >= 2)
            {
                room[nodeIndex].setpoint    = ReadI16(m.data);
                room[nodeIndex].sawSetpoint = true;
            }
            break;

        case Endpoint::DamperBudget:
            if (m.len >= 1)
            {
                room[nodeIndex].budget    = m.data[0];
                room[nodeIndex].sawBudget = true;
            }
            break;

        default:
            break;
    }
}

void BudgetAllocator::Loop()
{
    supplyTemp.Loop();

    if (!recomputeTimer.IsRunning())
    {
        recomputeTimer.Start(recomputeIntervalMs);
        Recompute(); // don't make a freshly-online node wait a full interval for its first budget
        return;
    }
    if (recomputeTimer.Finished())
    {
        recomputeTimer.Start(recomputeIntervalMs);
        Recompute();
    }
}

void BudgetAllocator::Recompute()
{
    uint8_t  onlineIds[NodeLib::MAX_NODES];
    uint32_t weight[NodeLib::MAX_NODES];
    uint8_t  onlineCount = 0;
    uint32_t weightSum   = 0;

    for (uint8_t id = 0; id < NodeLib::MAX_NODES; id++)
    {
        // Only a ControllerNode that is on the bus and running its app takes
        // part: a lost node keeps its moduleType in NodeMaster, and one in
        // the bootloader can't use a budget. Its cached room data is dropped
        // so a rejoin starts from fresh reports, not whatever was last seen
        // (Damper-Budget-Spec.md §5.1).
        const uint8_t nodeId = id + 1;
        if (!master.NodeActive(nodeId) || master.NodeInBootloader(nodeId) ||
            master.NodeModule(nodeId) != ModuleType::ControllerNode)
        {
            room[id] = SRoom();
            continue;
        }
        const uint8_t w        = (supplyTemp.Valid() && room[id].Known())
            ? NodeLib::RoomDemandPercent(supplyTemp.CentiDegC(), room[id].temp, room[id].setpoint)
            : 0;
        onlineIds[onlineCount] = nodeId;
        weight[onlineCount]    = w;
        weightSum += w;
        onlineCount++;
    }

    if (onlineCount == 0)
    {
        return;
    }

    if (!supplyTemp.Valid())
    {
        StepBudgetsTowardDefault(onlineIds, onlineCount);
        return;
    }

    // No demand anywhere -> split the pool evenly, which lands exactly back
    // on defaultBudgetPerNode with no special-casing (Damper-Budget-Spec.md §5.2).
    const uint32_t pool = static_cast<uint32_t>(defaultBudgetPerNode) * onlineCount;

    uint32_t give[NodeLib::MAX_NODES];
    bool     clamped[NodeLib::MAX_NODES];
    for (uint8_t i = 0; i < onlineCount; i++)
    {
        give[i]    = weightSum > 0 ? DivRoundNearest(pool * weight[i], weightSum) : DivRoundNearest(pool, onlineCount);
        clamped[i] = false;
    }

    // Water-fill anything over 100% into the still-open nodes, proportional to
    // weight. Bounded to onlineCount passes -- each pass clamps at least one
    // more node, so this always terminates well inside that bound (at most
    // MAX_NODES = 21 iterations, not an unbounded loop).
    for (uint8_t pass = 0; pass < onlineCount; pass++)
    {
        uint32_t overflow   = 0;
        uint32_t openWeight = 0;
        uint8_t  openCount  = 0;

        for (uint8_t i = 0; i < onlineCount; i++)
        {
            if (clamped[i])
            {
                continue;
            }
            if (give[i] > 100)
            {
                overflow += give[i] - 100;
                give[i]    = 100;
                clamped[i] = true;
            }
            else
            {
                openWeight += weight[i];
                openCount++;
            }
        }

        if (overflow == 0 || openCount == 0)
        {
            break;
        }
        for (uint8_t i = 0; i < onlineCount; i++)
        {
            if (!clamped[i])
            {
                give[i] += openWeight > 0 ? DivRoundNearest(overflow * weight[i], openWeight) : DivRoundNearest(overflow, openCount);
            }
        }
    }

    for (uint8_t i = 0; i < onlineCount; i++)
    {
        SendBudget(onlineIds[i], static_cast<uint8_t>(give[i] > 100 ? 100 : give[i]));
    }
}

// No supply reading: no room's demand can be judged, so no allocation --
// every node walks back to the un-arbitrated default, gradually, from wherever
// it is, and its damper follows (Damper-Budget-Spec.md §4.4). Re-sent every
// recompute even once there, which keeps the nodes' disconnect ramp cancelled
// (§5.3). A node whose budget isn't known yet starts at the default.
void BudgetAllocator::StepBudgetsTowardDefault(const uint8_t* const onlineIds, const uint8_t onlineCount)
{
    for (uint8_t i = 0; i < onlineCount; i++)
    {
        const SRoom& r    = room[onlineIds[i] - 1];
        uint8_t      next = r.sawBudget ? r.budget : defaultBudgetPerNode;
        if (next + budgetStepPercent < defaultBudgetPerNode)
        {
            next += budgetStepPercent;
        }
        else if (next > defaultBudgetPerNode + budgetStepPercent)
        {
            next -= budgetStepPercent;
        }
        else
        {
            next = defaultBudgetPerNode;
        }
        SendBudget(onlineIds[i], next);
    }
}

void BudgetAllocator::SendBudget(const uint8_t nodeId, const uint8_t percent)
{
    room[nodeId - 1].budget    = percent;
    room[nodeId - 1].sawBudget = true;
    master.QueueMessage(Id(nodeId, Endpoint::DamperBudget, Operation::Set), percent);
}
