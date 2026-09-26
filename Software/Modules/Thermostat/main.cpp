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
#include "Logger.h"
#include "MemoryMap.h"
#include "System.h"

#include "Node.h"

#include "ThermostatHandler.h"

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
