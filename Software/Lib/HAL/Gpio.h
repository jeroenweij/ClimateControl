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
            // Open-drain output: Write(false) pulls the line low, Write(true)
            // releases it (Hi-Z) for an external pull-up to raise. Read()
            // reflects the actual line level, so this doubles as a
            // single-wire bidirectional pin (e.g. 1-Wire). No internal pull.
            OpenDrain,
        };

        Gpio(const Pin pin, const Mode mode);

        void Write(const bool value);
        bool Read() const;

      private:
        Pin pin;
    };
} // namespace Hal
