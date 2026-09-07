/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "Tick.h"

#include "DelayTimer.h"

using Tools::DelayTimer;
using Tools::time_a;

DelayTimer::DelayTimer() :
    running(false),
    startTime(0),
    delayMs(0)
{
}

DelayTimer::DelayTimer(const time_a delayTime) :
    running(false),
    startTime(0),
    delayMs(0)
{
    Start(delayTime);
}

void DelayTimer::Start(const time_a delayTime)
{
    startTime = Hal::Tick::Millis();
    delayMs   = delayTime;
    running   = true;
}

void DelayTimer::ReStart()
{
    startTime = Hal::Tick::Millis();
    running   = true;
}

void DelayTimer::Stop()
{
    running = false;
}

const bool DelayTimer::Finished()
{
    if (running && ((Hal::Tick::Millis() - startTime) >= delayMs))
    {
        Stop();
        return true;
    }
    return false;
}

const bool DelayTimer::IsRunning() const
{
    return running;
}
