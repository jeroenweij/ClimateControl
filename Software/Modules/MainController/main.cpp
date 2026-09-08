/*************************************************************
 * Created by J. Weij
 *
 * MainController firmware -- base skeleton.
 * RS485 bus master (node id 0). See Spec/MainController-Spec.md.
 *************************************************************/

#include "MemoryMap.h"
#include "System.h"

#include "NodeMaster.h"

namespace
{
    // TODO: real expected count once ControllerNode / TemperatureNode exist.
    // 0 == "don't block in discovery", so the base image runs on a bare bus.
    constexpr uint8_t  ExpectedNodes  = 0;
    constexpr uint8_t  MaxPolledNodes = NodeLib::MAX_NODES;
    constexpr uint32_t BusBaud        = 115200;
} // namespace

int main()
{
    // App sits above the bootloader -- point the vector table at ourselves
    // before anything can fault (Node-Flash-Layout-and-Bootloader-Spec.md §4.1).
    Hal::System::SetVectorTable(Board::Flash::AppBase);
    Hal::System::Init();

    // TODO: clock tree to 64 MHz (HSI16 -> PLL). Running on HSI16 (16 MHz) for
    // now -- fine for 115200 on USART1.

    NodeLib::NodeMaster master(MaxPolledNodes, BusBaud);
    master.Init(ExpectedNodes);
    master.StartPollingNodes();

    while (true)
    {
        master.Loop();

        // TODO: NINA-W152 link (MainController-Spec.md §5) and supervisory
        // logic (§2) -- aggregate temperatures, expose state, detect faults.
    }
}
