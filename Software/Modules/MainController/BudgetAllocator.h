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
// sent down as a per-node DamperBudget ceiling (Damper-Budget-Spec.md §5).
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

        bool Known() const;

        bool    sawTemp;
        bool    sawSetpoint;
        bool    sawBudget;
        int16_t temp; // centi-degC
        int16_t setpoint; // centi-degC
        uint8_t budget; // the node's DamperBudget -- its own Report, else what was last sent
    };

    void Recompute();
    void StepBudgetsTowardDefault(const uint8_t* const onlineIds, const uint8_t onlineCount);
    void SendBudget(const uint8_t nodeId, const uint8_t percent);

    // pool = defaultBudgetPerNode * onlineCount, split by weight -- no floor,
    // the servo's own mechanical stop is the ventilation guarantee
    // (Damper-Budget-Spec.md §5.2).
    static const uint8_t  defaultBudgetPerNode = 50;
    static const uint32_t recomputeIntervalMs  = 30000;
    // Without a supply reading, each budget moves this far toward
    // defaultBudgetPerNode per recompute -- at most 50 points, so back at the
    // default within 25 minutes (Damper-Budget-Spec.md §4.4).
    static const uint8_t budgetStepPercent = 1;

    NodeLib::NodeMaster& master;

    NodeLib::SupplyTemp supplyTemp;
    SRoom               room[NodeLib::MAX_NODES]; // indexed by nodeId - 1 (nodes 1..MAX_NODES)

    Tools::DelayTimer recomputeTimer;
};
