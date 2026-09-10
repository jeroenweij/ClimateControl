/*************************************************************
 * Created by J. Weij
 *
 * TemperatureNode firmware -- base skeleton.
 * RS485 bus slave: two duct temperature probes reported on the main bus.
 * See Spec/TemperatureNode-Spec.md.
 *************************************************************/

#include "BoardPins.h"
#include "MemoryMap.h"
#include "System.h"

#include "Node.h"

#include "TemperatureHandler.h"

namespace
{
    constexpr uint32_t BusBaud = Board::BusBaudRate;
} // namespace

int main()
{
    // App sits above the bootloader -- point the vector table at ourselves
    // before anything can fault (Node-Flash-Layout-and-Bootloader-Spec.md §4.1).
    Hal::System::SetVectorTable(Board::Flash::AppBase);
    Hal::System::Init();

    // TODO: clock tree to 64 MHz (HSI16 -> PLL). Running on HSI16 (16 MHz) for
    // now -- 16 MHz / 250000 = 64 exact, so the bus baud is fine either way.

    NodeLib::Node      node(BusBaud);
    TemperatureHandler handler(node);

    handler.Init();
    node.RegisterHandler(&handler);
    node.Init(); // reads the provisioned NodeId from flash; halts if unprovisioned

    while (true)
    {
        handler.Loop();
        node.Loop();
    }
}
