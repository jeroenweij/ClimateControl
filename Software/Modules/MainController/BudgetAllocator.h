/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

#include "DelayTimer.h"

#include "Id.h"
#include "NodeMaster.h"
#include "SupplyTemp.h"

// Divides a shared airflow "budget" pool across every online ControllerNode,
// sent down as a per-node DamperBudget range (Damper-Budget-Spec.md §5): max
// is each room's fair share of the pool; min keeps the total opening at or
// above minTotalOpenPercent, because the HVAC unit's fan always runs and its
// air has to go somewhere -- the dump room is opened first, then the rooms
// that mind extra air least (§5.5).
// Never a room-level loop -- MainController-Spec.md §2/§4 item 2 -- this only
// narrows what each ControllerNode's own RoomControlLoop is permitted to do.
// MainController is already bus master, so Observe() needs no extra bus
// traffic: it just watches Report traffic UplinkHandler already sees.
class BudgetAllocator
{
  public:
    explicit BudgetAllocator(NodeLib::NodeMaster& master);

    void Loop(); // recomputes every recomputeIntervalMs
    void Observe(const NodeLib::Message& m); // fed from UplinkHandler::ReceivedMessage()

  private:
    struct SRoom
    {
        SRoom();

        // temp + setpoint reported, the temp within NodeLib::RoomTempStaleMs.
        bool Known() const;

        bool     sawTemp;
        bool     sawSetpoint;
        bool     sawBudget;
        bool     sawTarget;
        bool     dumpRoom; // the node's DumpRoom Report
        int16_t  temp; // centi-degC
        uint32_t tempAtMs; // Hal::Tick::Millis() of the last RoomTemp Report
        int16_t  setpoint; // centi-degC
        uint8_t  budget; // the node's DamperBudget max -- its own Report, else what was last sent
        uint8_t  mode; // the node's DamperMode Report; Auto until one arrives
        uint8_t  target; // the node's DamperTarget Report
    };

    void Recompute();
    void StepBudgetsTowardDefault(const uint8_t* const onlineIds, const uint8_t onlineCount, uint8_t* const maxes);
    // Sizes every online node's min so the total opening reaches
    // minTotalOpenPercent, then sends each node its [min, max] (§5.5).
    void AssignMinimumsAndSend(const uint8_t* const onlineIds, const uint8_t onlineCount, const uint8_t* const maxes);
    // Where a node's damper sits without any min: its room loop's own choice
    // inside 'max', or the commanded position of a node not in Auto.
    uint8_t Unfloored(const uint8_t nodeId, const uint8_t max) const;
    // Order in which rooms take extra air: the dump room first, then by how
    // little harm the supply air does there (lower = takes air sooner).
    int32_t SurplusRank(const uint8_t nodeId) const;
    void    SendBudget(const uint8_t nodeId, const uint8_t max, const uint8_t min);

    // pool = defaultBudgetPerNode * onlineCount, split by weight
    // (Damper-Budget-Spec.md §5.2).
    static const uint8_t defaultBudgetPerNode = 50;
    // Sum of all online ControllerNodes' damper positions, in percent, that
    // must stay open: 200 = the equivalent of two dampers fully open, on top
    // of the house's uncontrolled branch. A compile-time setting, to be
    // confirmed against the unit's minimum airflow on site
    // (Damper-Budget-Spec.md §5.5, §7).
    static const uint16_t minTotalOpenPercent = 200;
    static const uint32_t recomputeIntervalMs = 30000;
    // Without a supply reading, each budget moves this far toward
    // defaultBudgetPerNode per recompute -- at most 50 points, so back at the
    // default within 25 minutes (Damper-Budget-Spec.md §4.4).
    static const uint8_t budgetStepPercent = 1;

    NodeLib::NodeMaster& master;

    NodeLib::SupplyTemp supplyTemp;
    SRoom               room[NodeLib::MAX_NODES]; // indexed by nodeId - 1 (nodes 1..MAX_NODES)

    Tools::DelayTimer recomputeTimer;
};
