/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "BoardPins.h"
#include "Tick.h"

#include "ErrorHandler.h"

using NodeLib::ErrorHandler;

ErrorHandler::ErrorHandler() :
    led(Board::ErrorLed, Hal::Gpio::Mode::Output),
    button(Board::UserButton, Hal::Gpio::Mode::InputPullUp)
{
    led.Write(false);
}

void ErrorHandler::Error(const bool recoverable) const
{
    // button is active-low (InputPullUp) -- Read() is true while released.
    while (!recoverable || button.Read())
    {
        led.Write(true);
        Hal::Tick::DelayMs(500);
        led.Write(false);
        Hal::Tick::DelayMs(500);
    }
}
