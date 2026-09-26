/*************************************************************
 * Created by J. Weij
 *
 * MainController firmware -- base skeleton.
 * RS485 bus master (node id 0). See Spec/MainController-Spec.md.
 *************************************************************/

#include "BoardPins.h"
#include "Logger.h"
#include "MemoryMap.h"
#include "System.h"

#include "BootHealth.h"
#include "NodeMaster.h"

#include "BudgetAllocator.h"
#include "UplinkHandler.h"

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

    NodeLib::NodeMaster master;
    BudgetAllocator     budgetAllocator(master);
    UplinkHandler       uplink(master, budgetAllocator);

    uplink.Init();
    master.RegisterHandler(&uplink);
    master.Init();

    while (true)
    {
        uplink.Loop();
        master.Loop();
        budgetAllocator.Loop();
        Tools::BootHealth::ConfirmBootHealthy();

        // TODO: further supervisory logic (MainController-Spec.md §2) --
        // expose aggregate state, detect faults.
    }
}
