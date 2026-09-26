/*************************************************************
 * Created by J. Weij
 *
 * TemperatureNode firmware -- base skeleton.
 * RS485 bus slave: two duct temperature probes reported on the main bus.
 * See Spec/TemperatureNode-Spec.md.
 *************************************************************/

#include "BoardPins.h"
#include "BootHealth.h"
#include "Logger.h"
#include "MemoryMap.h"
#include "System.h"

#include "Node.h"

#include "TemperatureHandler.h"

int main()
{
    // App sits above the bootloader -- point the vector table at ourselves
    // before anything can fault (Node-Flash-Layout-and-Bootloader-Spec.md §4.1).
    Hal::System::SetVectorTable(Board::Flash::AppBase);
    Hal::System::Init();

    // 64 MHz from the HSI16 through the PLL -- before any peripheral comes
    // up, since UART baud and the rest are derived from the clock. Still
    // fully working at 16 MHz if the PLL won't lock.
    if (!Hal::System::ClockTo64MHz())
    {
        LOG_WARN("PLL did not lock -- running at 16 MHz");
    }

    NodeLib::Node      node;
    TemperatureHandler handler(node);

    handler.Init();
    node.RegisterHandler(&handler);
    node.Init(); // reads the provisioned NodeId from flash; halts if unprovisioned

    while (true)
    {
        handler.Loop();
        node.Loop();
        Tools::BootHealth::ConfirmBootHealthy();
    }
}
