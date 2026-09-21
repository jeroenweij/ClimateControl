/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

#include "DelayTimer.h"

#include "Id.h"
#include "NodeMaster.h"

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
        int16_t temp; // centi-degC
        int16_t setpoint; // centi-degC
    };

    void Recompute();
    void SendBudget(const uint8_t nodeId, const uint8_t percent);

    // pool = defaultBudgetPerNode * onlineCount, split by weight -- no floor,
    // the servo's own mechanical stop is the ventilation guarantee
    // (Damper-Budget-Spec.md §5.2).
    static const uint8_t  defaultBudgetPerNode = 50;
    static const uint32_t recomputeIntervalMs  = 30000;

    NodeLib::NodeMaster& master;

    int16_t supplyTemp;
    bool    supplyValid;
    SRoom   room[NodeLib::MAX_NODES]; // indexed by nodeId, [0] (master) unused

    Tools::DelayTimer recomputeTimer;
};
