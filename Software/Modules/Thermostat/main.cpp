/*************************************************************
 * Created by J. Weij
 *
 * Thermostat firmware.
 * NodeLib slave on the point-to-point link to its paired ControllerNode
 * (USART2). Source of truth for the Room* endpoints; caches the damper state
 * for the display. See Spec/ControllerNode-Thermostat-Link-Spec.md.
 *************************************************************/

#include "BoardPins.h"
#include "BootHealth.h"
#include "MemoryMap.h"
#include "System.h"

#include "Node.h"

#include "ThermostatHandler.h"

int main()
{
    Hal::System::SetVectorTable(Board::Flash::AppBase);
    Hal::System::Init();

    // TODO: clock tree to 64 MHz (HSI16 -> PLL). Board::BusBaudRate (115200)
    // isn't an exact divisor at either clock, but the resulting generator
    // error is negligible next to the HSI16 spread itself
    // (Node-Bus-Hardware-Design-Spec.md §6.1).

    NodeLib::Node     node(Hal::Uart::Instance::Usart2, Board::LinkUart, 0);
    ThermostatHandler handler(node);

    handler.Init();
    node.RegisterHandler(&handler);
    node.Init(); // reads the provisioned NodeId (shared with its ControllerNode)

    while (true)
    {
        handler.Loop();
        node.Loop();
        Tools::BootHealth::ConfirmBootHealthy();
    }
}
