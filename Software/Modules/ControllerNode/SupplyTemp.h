/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

#include "DelayTimer.h"
#include "Id.h"

// Learns the shared duct SupplyTemp by passively observing whichever
// TemperatureNode reports it as it passes on the main bus (INodeHandler::
// Snoop() -- Damper-Budget-Spec.md §3.2, §4.1). Not tied to a specific
// TemperatureNode id -- whichever one reports SupplyTemp is trusted, matching
// TemperatureNode-Spec.md §1's single shared duct sensor.
class SupplyTemp
{
  public:
    SupplyTemp();

    // Feed every snooped frame; no-op unless it's a Report SupplyTemp.
    void Snoop(const NodeLib::Message& m);
    // Ages the reading out after staleTimeoutMs with nothing heard.
    void Loop();

    bool    Valid() const;
    int16_t CentiDegC() const;

  private:
    static const uint32_t staleTimeoutMs = 5 * 60 * 1000;

    int16_t           value;
    bool              valid;
    Tools::DelayTimer staleTimer;
};
