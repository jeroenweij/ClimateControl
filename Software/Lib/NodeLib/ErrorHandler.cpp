/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "Logger.h"
#include "Tick.h"

#include "ErrorHandler.h"

using NodeLib::ErrorHandler;

ErrorHandler::ErrorHandler(const Hal::Pin ledPin, const std::optional<Hal::Pin> buttonPin) :
    led(ledPin, Hal::Gpio::Mode::Output),
    button(buttonPin.has_value() ? std::optional<Hal::Gpio>(std::in_place, *buttonPin, Hal::Gpio::Mode::InputPullUp) : std::nullopt)
{
    led.Write(false);
}

void ErrorHandler::Error(const bool recoverable) const
{
    while (!recoverable || !button.has_value() || button->Read())
    {
        led.Write(true);
        Hal::Tick::DelayMs(500);
        led.Write(false);
        Hal::Tick::DelayMs(500);
    }
}
