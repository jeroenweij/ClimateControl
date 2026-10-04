/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "Tick.h"

#include "ConfigStore.h"
#include "EEndpoint.h"
#include "EModuleType.h"
#include "EOperation.h"
#include "RoomDemand.h"

#include <stdint.h>

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
    // DamperMode wire values (Node-Message-Model-Spec.md §3).
    constexpr uint8_t AutoMode    = 2;
    constexpr uint8_t StalledMode = 4;

    // DamperBudget max when the supply reading is missing -- the room loops
    // then sit at neutral (Damper-Budget-Spec.md §4.4).
    constexpr uint8_t NeutralPercent = 50;

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
    sawTarget(false),
    dumpRoom(false),
    temp(0),
    tempAtMs(0),
    setpoint(0),
    budget(0),
    mode(AutoMode),
    target(0)
{
}

bool BudgetAllocator::SRoom::Known() const
{
    // A room whose temperature stopped arriving -- its Thermostat's sensor
    // died, the ControllerNode dropped it -- weighs nothing and counts as
    // closed in the minimum-opening total, rather than living on frozen.
    return sawTemp && sawSetpoint && Hal::Tick::Millis() - tempAtMs < NodeLib::RoomTempStaleMs;
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
                room[nodeIndex].temp     = ReadI16(m.data);
                room[nodeIndex].tempAtMs = Hal::Tick::Millis();
                room[nodeIndex].sawTemp  = true;
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
                room[nodeIndex].budget    = m.data[0]; // max; the min is ours to choose
                room[nodeIndex].sawBudget = true;
            }
            break;

        case Endpoint::DamperMode:
            if (m.len >= 1)
            {
                room[nodeIndex].mode = m.data[0];
            }
            break;

        case Endpoint::DamperTarget:
            if (m.len >= 1)
            {
                room[nodeIndex].target    = m.data[0];
                room[nodeIndex].sawTarget = true;
            }
            break;

        case Endpoint::DumpRoom:
            if (m.len >= 1)
            {
                room[nodeIndex].dumpRoom = m.data[0] == 1;
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
    bool     known[NodeLib::MAX_NODES];
    uint8_t  onlineCount = 0;
    uint8_t  knownCount  = 0;
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
        // A room without data (no Thermostat, or its temperature stopped
        // arriving) can't claim or give up air by demand: its loop sits at
        // neutral, so it keeps a fixed default share outside the split below
        // (Damper-Budget-Spec.md §4.4, §5.2).
        known[onlineCount]     = supplyTemp.Valid() && room[id].Known();
        const uint8_t w        = known[onlineCount]
            ? NodeLib::RoomDemandPercent(supplyTemp.CentiDegC(), room[id].temp, room[id].setpoint)
            : 0;
        onlineIds[onlineCount] = nodeId;
        weight[onlineCount]    = w;
        weightSum += w;
        knownCount += known[onlineCount] ? 1 : 0;
        onlineCount++;
    }

    if (onlineCount == 0)
    {
        return;
    }

    uint8_t maxes[NodeLib::MAX_NODES];
    if (!supplyTemp.Valid())
    {
        StepBudgetsTowardDefault(onlineIds, onlineCount, maxes);
        AssignMinimumsAndSend(onlineIds, onlineCount, maxes);
        return;
    }

    // No demand anywhere -> split the pool evenly, which lands exactly back
    // on defaultBudgetPerNode with no special-casing (Damper-Budget-Spec.md §5.2).
    // Only the rooms with data share the pool; the rest hold the default.
    const uint32_t pool = static_cast<uint32_t>(defaultBudgetPerNode) * knownCount;

    uint32_t give[NodeLib::MAX_NODES];
    bool     clamped[NodeLib::MAX_NODES];
    for (uint8_t i = 0; i < onlineCount; i++)
    {
        if (!known[i])
        {
            give[i]    = defaultBudgetPerNode;
            clamped[i] = true; // out of the water-fill below
            continue;
        }
        give[i]    = weightSum > 0 ? DivRoundNearest(pool * weight[i], weightSum) : DivRoundNearest(pool, knownCount);
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
        maxes[i] = static_cast<uint8_t>(give[i] > 100 ? 100 : give[i]);
    }
    AssignMinimumsAndSend(onlineIds, onlineCount, maxes);
}

// No supply reading: no room's demand can be judged, so no allocation --
// every node walks back to the un-arbitrated default, gradually, from wherever
// it is, and its damper follows (Damper-Budget-Spec.md §4.4). Re-sent every
// recompute even once there, which keeps the nodes' disconnect ramp cancelled
// (§5.3). A node whose budget isn't known yet starts at the default.
void BudgetAllocator::StepBudgetsTowardDefault(const uint8_t* const onlineIds, const uint8_t onlineCount, uint8_t* const maxes)
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
        maxes[i] = next;
    }
}

// The fan always runs, so the dampers together must keep enough of the duct
// open (Damper-Budget-Spec.md §5.5). Each room's loop picks a position inside
// its max; if those together fall short of minTotalOpenPercent, the shortfall
// is handed out as minimums -- to the dump room first, then to the rooms the
// extra air harms least -- each filled up to 100 before the next takes any.
// Nodes not in Auto keep their commanded position and take no minimum.
void BudgetAllocator::AssignMinimumsAndSend(const uint8_t* const onlineIds, const uint8_t onlineCount, const uint8_t* const maxes)
{
    uint8_t  base[NodeLib::MAX_NODES];
    uint8_t  mins[NodeLib::MAX_NODES];
    uint32_t total = 0;
    for (uint8_t i = 0; i < onlineCount; i++)
    {
        base[i] = Unfloored(onlineIds[i], maxes[i]);
        mins[i] = 0;
        total += base[i];
    }

    uint32_t shortfall                = total < minTotalOpenPercent ? minTotalOpenPercent - total : 0;
    bool     used[NodeLib::MAX_NODES] = {};
    while (shortfall > 0)
    {
        // Next room in SurplusRank() order -- a selection pass over at most
        // MAX_NODES entries, so at most MAX_NODES passes in all.
        int16_t best = -1;
        for (uint8_t i = 0; i < onlineCount; i++)
        {
            const SRoom& r = room[onlineIds[i] - 1];
            if (used[i] || r.mode != AutoMode)
            {
                continue;
            }
            if (best < 0 || SurplusRank(onlineIds[i]) < SurplusRank(onlineIds[best]))
            {
                best = static_cast<int16_t>(i);
            }
        }
        if (best < 0)
        {
            break; // every Auto room is already fully open
        }
        used[best]         = true;
        const uint32_t add = (100u - base[best]) < shortfall ? (100u - base[best]) : shortfall;
        if (add > 0)
        {
            mins[best] = static_cast<uint8_t>(base[best] + add);
            shortfall -= add;
        }
    }

    for (uint8_t i = 0; i < onlineCount; i++)
    {
        // Sent as computed: a min above the fair-share max wins on the node
        // (RoomControlLoop::SetBudget()), so max keeps meaning the share.
        SendBudget(onlineIds[i], maxes[i], mins[i]);
    }
}

uint8_t BudgetAllocator::Unfloored(const uint8_t nodeId, const uint8_t max) const
{
    const SRoom& r = room[nodeId - 1];
    if (r.mode == StalledMode)
    {
        return 0; // no idea where it stopped -- count it as closed
    }
    if (r.mode != AutoMode)
    {
        return r.sawTarget ? r.target : 0; // Closed / Open / Manual: an explicit position
    }
    // Without a supply reading or room data its loop sits at neutral (§4.4).
    uint8_t desired = NeutralPercent;
    if (supplyTemp.Valid() && r.Known())
    {
        desired = NodeLib::RoomDemandPercent(supplyTemp.CentiDegC(), r.temp, r.setpoint);
    }
    return desired < max ? desired : max;
}

int32_t BudgetAllocator::SurplusRank(const uint8_t nodeId) const
{
    const SRoom& r = room[nodeId - 1];
    if (r.dumpRoom)
    {
        return INT32_MIN;
    }
    if (!supplyTemp.Valid() || !r.Known())
    {
        return INT32_MAX - 256 + nodeId; // nothing to judge by -- last, in id order
    }
    // How far the room already is past its setpoint in the direction the
    // supply air pushes it: negative = the room still wants this air.
    const int32_t past = supplyTemp.CentiDegC() >= r.temp ? r.temp - r.setpoint : r.setpoint - r.temp;
    return past * 256 + nodeId; // id breaks ties
}

void BudgetAllocator::SendBudget(const uint8_t nodeId, const uint8_t max, const uint8_t min)
{
    room[nodeId - 1].budget    = max;
    room[nodeId - 1].sawBudget = true;
    const uint8_t payload[2]   = {max, min};
    master.QueueMessage(Id(nodeId, Endpoint::DamperBudget, Operation::Set), payload, sizeof(payload));
}
