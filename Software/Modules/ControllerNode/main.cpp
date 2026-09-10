/*************************************************************
 * Created by J. Weij
 *
 * ControllerNode firmware.
 * RS485 main-bus slave (damper actuator) plus the point-to-point link master
 * to this room's Thermostat. See Spec/ControllerNode-Thermostat-Link-Spec.md
 * and Spec/Node-Message-Model-Spec.md.
 *************************************************************/

#include "BoardPins.h"
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

    // TODO: clock tree to 64 MHz (HSI16 -> PLL). 16 MHz / 250000 = 64 exact.

    Damper damper;

    NodeLib::Node       busNode(Board::BusBaudRate);
    NodeLib::LinkMaster link(Board::BusBaudRate);

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
    }
}
