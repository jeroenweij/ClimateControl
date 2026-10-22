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
#include "Fault.h"
#include "MemoryMap.h"
#include "System.h"
#include "Watchdog.h"

#include "Node.h"

#include "ThermostatHandler.h"

int main()
{
    Hal::System::SetVectorTable(Board::Flash::AppBase);
    Hal::System::Init();
    Hal::Fault::Init(); // records a watchdog reset before anything else runs

    // Stays at the reset-default 16 MHz HSI and sleeps between loop passes,
    // unlike the other boards' 64 MHz: the MCU's heat lands on the room
    // sensor a few centimetres away (ControllerNode-Thermostat-Link-Spec.md
    // §4.3), and a 2 s sensor read plus a small display need no more.

    NodeLib::Node     node(Hal::Uart::Instance::Usart2, Board::LinkUart, 0);
    ThermostatHandler handler(node);

    handler.Init();
    node.RegisterHandler(&handler);
    node.Init(); // reads the provisioned NodeId (shared with its ControllerNode)

    Hal::Watchdog::Start();
    while (true)
    {
        Hal::Watchdog::Feed();
        handler.Loop();
        node.Loop();
        Tools::BootHealth::ConfirmBootHealthy();
        Hal::System::Sleep(); // until the 1 ms SysTick or a link byte
    }
}
