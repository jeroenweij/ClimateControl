/*************************************************************
 * Created by J. Weij
 *
 * Bus-resident bootloader -- base skeleton.
 * ONE binary for all four boards (Spec/Node-Flash-Layout-and-Bootloader-Spec.md
 * §7). The master and the nodes are updated by different mechanisms, but that
 * does not split the bootloader -- it branches on ConfigStore::Valid().
 *
 * Boot decision (§5):
 *   - Board::EnterBootloaderMagic in backup reg  -> stay resident (app asked)
 *   - no valid application image                 -> stay resident
 *   - otherwise                                  -> jump to the app
 *
 * Stay-resident behaviour:
 *   - provisioned node (ConfigStore::Valid())    -> RS485 OTA slave loop (§6)
 *   - MainController (no ConfigRecord) or an     -> passive wait on the error
 *     unprovisioned node                            LED; recovery is SWD / bench
 *
 * The RS485 OTA slave loop needs the NodeLib framing/bus split and is still
 * TODO -- for now both paths just blink and wait.
 *************************************************************/

#include "Backup.h"
#include "BoardPins.h"
#include "ConfigStore.h"
#include "Gpio.h"
#include "MemoryMap.h"
#include "System.h"
#include "Tick.h"

#include "AppImage.h"
#include "FirmwareSlave.h"

namespace Backup = Hal::Backup;
using NodeLib::ConfigStore;

namespace
{
    // OTA runs at the same rate as the application bus (RS485 spec §8 item 3
    // leaves an OTA-specific baud open).
    constexpr uint32_t BusBaud = 115200;

    bool EnterBootloaderRequested()
    {
        if (Backup::Read(Backup::Reg::Boot) != Board::EnterBootloaderMagic)
        {
            return false;
        }
        Backup::Write(Backup::Reg::Boot, 0); // consume it
        return true;
    }

    [[noreturn]] void BlinkForever(const uint32_t onMs, const uint32_t offMs)
    {
        Hal::Gpio errorLed(Board::ErrorLed, Hal::Gpio::Mode::Output);
        while (true)
        {
            errorLed.Write(true);
            Hal::Tick::DelayMs(onMs);
            errorLed.Write(false);
            Hal::Tick::DelayMs(offMs);
        }
    }

    [[noreturn]] void StayResident()
    {
        if (ConfigStore::Valid())
        {
            // Provisioned node: serve a firmware image over RS485 at
            // ConfigStore::NodeId() (Node-Flash spec §6).
            Boot::FirmwareSlave slave(BusBaud, ConfigStore::NodeId(), static_cast<uint8_t>(ConfigStore::GetModule()));
            slave.Init();
            while (true)
            {
                slave.Loop();
            }
        }

        // MainController (no ConfigRecord) or an unprovisioned node: no bus
        // address -> no over-the-bus update. The MainController updates itself
        // app-assisted over NINA (§7.1); a bare node needs the bench. Either way
        // the recovery path here is SWD.
        BlinkForever(500, 500);
    }
} // namespace

int main()
{
    Hal::System::Init();

    const bool forced = EnterBootloaderRequested();

    if (!forced && Boot::AppImage::IsValid())
    {
        Hal::System::JumpToApplication(Board::Flash::AppBase); // never returns
    }

    StayResident();
}
