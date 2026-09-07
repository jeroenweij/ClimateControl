/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <optional>

#include "Gpio.h"

namespace NodeLib
{
    class ErrorHandler
    {
      public:
        ErrorHandler(const Hal::Pin ledPin, const std::optional<Hal::Pin> buttonPin = std::nullopt);

        void Error(const bool recoverable) const;

      private:
        mutable Hal::Gpio        led;
        std::optional<Hal::Gpio> button;
    };
} // namespace NodeLib
