/*************************************************************
 * Created by J. Weij
 *
 * Thermostat firmware.
 * NodeLib slave on the point-to-point link to its paired ControllerNode
 * (USART2). Source of truth for the Room* endpoints; caches the damper state
 * for the display. See Spec/ControllerNode-Thermostat-Link-Spec.md.
 *************************************************************/

#include "BoardPins.h"
#include "MemoryMap.h"
#include "System.h"

#include "Node.h"

#include "ThermostatHandler.h"

int main()
{
    Hal::System::SetVectorTable(Board::Flash::AppBase);
    Hal::System::Init();

    // TODO: clock tree to 64 MHz (HSI16 -> PLL). 16 MHz / 250000 = 64 exact.

    NodeLib::Node     node(Board::BusBaudRate, Hal::Uart::Instance::Usart2, Board::LinkUart);
    ThermostatHandler handler(node);

    handler.Init();
    node.RegisterHandler(&handler);
    node.Init(); // reads the provisioned NodeId (shared with its ControllerNode)

    while (true)
    {
        handler.Loop();
        node.Loop();
    }
}
