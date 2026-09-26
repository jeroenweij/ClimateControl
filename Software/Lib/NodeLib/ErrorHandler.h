/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include "Gpio.h"

namespace NodeLib
{
    // Drives the error LED and watches the user button -- both are fixed by
    // Lib/Board/BoardPins.h, not configurable.
    class ErrorHandler
    {
      public:
        ErrorHandler();

        void Error(const bool recoverable) const;

        // Steady error-LED state for a condition the node rides out (e.g. the
        // bus gone quiet) -- as opposed to Error(), which never returns.
        void Indicate(const bool on) const;

      private:
        mutable Hal::Gpio led;
        Hal::Gpio         button;
    };
} // namespace NodeLib
