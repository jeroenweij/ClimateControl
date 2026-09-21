/*************************************************************
 * Created by J. Weij
 *
 * MainController firmware -- base skeleton.
 * RS485 bus master (node id 0). See Spec/MainController-Spec.md.
 *************************************************************/

#include "BoardPins.h"
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

    // TODO: clock tree to 64 MHz (HSI16 -> PLL). Running on HSI16 (16 MHz) for
    // now -- Board::BusBaudRate (115200) isn't an exact divisor at either
    // clock, but the resulting generator error is negligible next to the
    // HSI16 spread itself (Node-Bus-Hardware-Design-Spec.md §6.1).

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
