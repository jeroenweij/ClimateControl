/*************************************************************
 * Created by J. Weij
 *
 * NinaEnable -- standalone MainController bring-up utility, not part of the
 * normal firmware. Releases the on-board NINA-W152 out of reset, statically
 * asserts the module's UART_CTS so it feels free to transmit, and otherwise
 * touches nothing else, so a USB-TTL adapter wired to the NINA's own TXD/RXD
 * pins can talk to its AT command interface directly. See README.md.
 *
 * u-connectXpress ships with 4-wire HW flow control on by default
 * (MainController-Spec.md Sec5) -- the module's UART_CTS input (module pin
 * 21) won't let it transmit unless driven low. That input is wired to
 * Board::NinaRts (STM32 PA1); a bench USB-TTL adapter only ever breaks out
 * TXD/RXD/GND (header H1), so nothing else on the bench drives it.
 *
 * Board::Usart2Tx/Usart2Rx/NinaCts are still never initialised here -- left
 * at their GPIO reset state (Hi-Z/analog) so this firmware never drives or
 * listens on those lines; only NinaRts is actively driven.
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

    // NinaRts (STM32 PA1) -> NINA UART_CTS (module pin 21). Drive it low
    // (asserted) permanently so the module always considers itself clear to
    // transmit to whatever is on TXD/RXD, regardless of what the bench
    // adapter does -- it has no CTS/RTS lines of its own to assert this.
    Gpio ninaRts(Board::NinaRts, Gpio::Mode::Output);
    ninaRts.Write(false);

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
