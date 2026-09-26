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
    temp(0),
    setpoint(0)
{
}

bool BudgetAllocator::SRoom::Known() const
{
    return sawTemp && sawSetpoint;
}

BudgetAllocator::BudgetAllocator(NodeMaster& master) :
    master(master),
    supplyTemp(0),
    supplyValid(false),
    room{},
    recomputeTimer()
{
}

void BudgetAllocator::Observe(const Message& m)
{
    if (m.id.operation != Operation::Report || m.len < 2)
    {
        return;
    }

    switch (m.id.endpoint)
    {
        case Endpoint::SupplyTemp:
            supplyTemp  = ReadI16(m.data);
            supplyValid = true;
            break;

        case Endpoint::RoomTemp:
            if (m.id.node < NodeLib::MAX_NODES)
            {
                room[m.id.node].temp    = ReadI16(m.data);
                room[m.id.node].sawTemp = true;
            }
            break;

        case Endpoint::RoomSetpoint:
            if (m.id.node < NodeLib::MAX_NODES)
            {
                room[m.id.node].setpoint    = ReadI16(m.data);
                room[m.id.node].sawSetpoint = true;
            }
            break;

        default:
            break;
    }
}

void BudgetAllocator::Loop()
{
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

    for (uint8_t id = 1; id < NodeLib::MAX_NODES; id++)
    {
        if (master.NodeModule(id) != ModuleType::ControllerNode)
        {
            continue;
        }
        const uint8_t w        = (supplyValid && room[id].Known())
            ? NodeLib::RoomDemandPercent(supplyTemp, room[id].temp, room[id].setpoint)
            : 0;
        onlineIds[onlineCount] = id;
        weight[onlineCount]    = w;
        weightSum += w;
        onlineCount++;
    }

    if (onlineCount == 0)
    {
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
    // MAX_NODES-1 = 20 iterations, not an unbounded loop).
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

void BudgetAllocator::SendBudget(const uint8_t nodeId, const uint8_t percent)
{
    master.QueueMessage(Id(nodeId, Endpoint::DamperBudget, Operation::Set), percent);
}
