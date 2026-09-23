/*************************************************************
 * Created by J. Weij
 *
 * MainController's own bootloader -- a separate image from Modules/
 * Bootloader (the shared RS485-bus-resident one for ControllerNode/
 * TemperatureNode/Thermostat). MainController has no bus to be OTA'd over
 * while resident here; instead it dials out to the server itself over the
 * on-board NINA-W152, the same uplink the running app uses
 * (MainController-Server-Link-Spec.md §5, §8 step 7).
 *
 * Boot decision (Node-Flash-Layout-and-Bootloader-Spec.md §5):
 *   - Board::EnterBootloaderMagic in backup reg  -> stay resident (app asked)
 *   - no valid application image                 -> stay resident
 *   - too many boots without the app ever proving itself healthy
 *     (Tools::BootHealth) -> stay resident
 *   - otherwise                                  -> jump to the app
 *
 * Stay-resident behaviour: bring up the NINA uplink (UplinkHandler) and
 * drive Firmware's erase/write/verify/activate state machine from whatever
 * Endpoint::OtaControl/OtaData messages the server sends over it.
 *************************************************************/

#include "Backup.h"
#include "BoardPins.h"
#include "BootHealth.h"
#include "MemoryMap.h"
#include "System.h"

#include "AppImage.h"
#include "Firmware.h"
#include "UplinkHandler.h"

namespace Backup = Hal::Backup;

namespace
{
    bool EnterBootloaderRequested()
    {
        if (Backup::Read(Backup::Reg::Boot) != Board::EnterBootloaderMagic)
        {
            return false;
        }
        Backup::Write(Backup::Reg::Boot, 0); // consume it
        return true;
    }

    [[noreturn]] void StayResident()
    {
        Boot::Firmware firmware;
        UplinkHandler  uplink(firmware);

        firmware.Init();
        uplink.Init();

        while (true)
        {
            uplink.Loop();
        }
    }
} // namespace

int main()
{
    Hal::System::Init();

    const bool forced = EnterBootloaderRequested();

    // Short-circuits deliberately: TooManyFailedBoots() records an attempt as
    // a side effect, and a forced entry (a normal OTA request) or an already-
    // invalid image are not boot failures -- only count an attempt when we're
    // actually about to trust a CRC-valid image and jump to it.
    if (!forced && Boot::AppImage::IsValid() && !Tools::BootHealth::TooManyFailedBoots())
    {
        Hal::System::JumpToApplication(Board::Flash::AppBase); // never returns
    }

    StayResident();
}
