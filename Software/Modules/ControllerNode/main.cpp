/*************************************************************
 * Created by J. Weij
 *
 * ControllerNode firmware.
 * RS485 main-bus slave (damper actuator) plus the point-to-point link master
 * to this room's Thermostat. See Spec/ControllerNode-Thermostat-Link-Spec.md
 * and Spec/Node-Message-Model-Spec.md.
 *************************************************************/

#include "BoardPins.h"
#include "BootHealth.h"
#include "Logger.h"
#include "MemoryMap.h"
#include "System.h"

#include "LinkMaster.h"
#include "Node.h"

#include "ControllerHandler.h"
#include "Damper.h"
#include "ThermostatLink.h"

int main()
{
    Hal::System::SetVectorTable(Board::Flash::AppBase);
    Hal::System::Init();

    // 64 MHz from the HSI16 through the PLL -- before any peripheral comes
    // up, since UART baud and the rest are derived from the clock. Still
    // fully working at 16 MHz if the PLL won't lock.
    if (!Hal::System::ClockTo64MHz())
    {
        LOG_WARN("PLL did not lock -- running at 16 MHz");
    }

    Damper damper;

    NodeLib::Node       busNode;
    NodeLib::LinkMaster link;

    ThermostatLink    thermostatLink(link, damper);
    ControllerHandler handler(busNode, damper, thermostatLink);

    damper.Init();

    busNode.RegisterHandler(&handler);
    link.RegisterHandler(&thermostatLink);

    busNode.Init(); // reads the provisioned NodeId; halts if unprovisioned
    link.Init(); // peer id = this node's id (Thermostat shares it)

    while (true)
    {
        busNode.Loop();
        link.Loop();
        thermostatLink.Loop();
        handler.Loop();
        Tools::BootHealth::ConfirmBootHealthy();
    }
}
