/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

namespace Tools
{
    using time_a = uint32_t;

    class DelayTimer
    {
      public:
        DelayTimer();
        DelayTimer(const time_a delayTime);

        void Start(const time_a delayTime);
        void ReStart();
        void Stop();

        const bool Finished();
        const bool IsRunning() const;

      private:
        bool   running;
        time_a startTime;
        time_a delayMs;
    };
} // namespace Tools
