/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include "Pin.h"

namespace Hal
{
    class Gpio
    {
      public:
        enum class Mode
        {
            Output,
            Input,
            InputPullUp,
        };

        Gpio(const Pin pin, const Mode mode);

        void Write(const bool value);
        bool Read() const;

      private:
        Pin pin;
    };
} // namespace Hal
