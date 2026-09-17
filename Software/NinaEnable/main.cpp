/*************************************************************
 * Created by J. Weij
 *
 * NinaEnable -- standalone MainController bring-up utility, not part of the
 * normal firmware. Releases the on-board NINA-W152 out of reset and then
 * touches nothing else, so a USB-TTL adapter wired to the NINA's own UART
 * pins can talk to its AT command interface directly. See README.md.
 *
 * USART2 (Board::Usart2Tx/Usart2Rx/NinaCts/NinaRts) is never initialised
 * here -- left at its GPIO reset state (Hi-Z/analog) so this firmware never
 * drives or listens on those lines.
 *************************************************************/

#include "BoardPins.h"
#include "Gpio.h"
#include "System.h"
#include "Tick.h"

using Hal::Gpio;

int main()
{
    Hal::System::Init();

    // NinaReset (net "RESET_NINA") is active-low, open-drain, with a 100k
    // pull-up on the module itself -- Write(true) releases it to Hi-Z, letting
    // that pull-up bring the line high and the NINA run. Never drive it
    // push-pull high (see Lib/Board/BoardPins.h).
    Gpio ninaReset(Board::NinaReset, Gpio::Mode::OpenDrain);
    ninaReset.Write(true);

    // Heartbeat only, to confirm this image is alive on the bench -- does not
    // touch any NINA/UART pin.
    Gpio activityLed(Board::ActivityLed, Gpio::Mode::Output);
    while (true)
    {
        activityLed.Write(true);
        Hal::Tick::DelayMs(200);
        activityLed.Write(false);
        Hal::Tick::DelayMs(200);
    }
}
