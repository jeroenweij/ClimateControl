/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include <string.h>

#include "BoardPins.h"

#include "NinaAt.h"

namespace
{
    const uint32_t NinaBaud = 115200;
}

NinaAt::NinaAt(NinaPort& port) :
    port(port),
    ninaReset(Board::NinaReset, Hal::Gpio::Mode::OpenDrain),
    parser(),
    dataMode(false),
    commandPending(false),
    pendingResult(Result::Timeout),
    commandTimeout(),
    resetPending(false),
    resetTimer()
{
}

void NinaAt::Init()
{
    port.Init(NinaBaud);
    PulseReset();
}

void NinaAt::PulseReset()
{
    // Active-low, open-drain; >=50 us low per the module's datasheet
    // (MainController-Spec.md §5). Async -- Loop() releases it once
    // resetPulseMs elapses. The 100 ms margin is generous but costs nothing:
    // nothing else here blocks waiting for it.
    const uint32_t resetPulseMs = 100;

    ninaReset.Write(false);
    resetTimer.Start(resetPulseMs);
    resetPending = true;

    dataMode       = false;
    commandPending = false;
    pendingResult  = Result::Timeout;
    parser.Reset();
}

void NinaAt::Loop()
{
    if (resetPending && resetTimer.Finished())
    {
        ninaReset.Write(true);
        resetPending = false;
    }

    if (dataMode)
    {
        return; // caller drains raw bytes itself
    }

    while (port.Available())
    {
        const NinaLineParser::LineResult lineResult = parser.FeedByte(port.ReadByte());
        if (commandPending && lineResult != NinaLineParser::LineResult::None)
        {
            pendingResult  = (lineResult == NinaLineParser::LineResult::Ok) ? Result::Ok : Result::Error;
            commandPending = false;
        }
    }

    if (commandPending && commandTimeout.Finished())
    {
        pendingResult  = Result::Timeout;
        commandPending = false;
    }
}

bool NinaAt::SendCommand(const char* const command, const uint32_t timeoutMs)
{
    if (commandPending)
    {
        return false;
    }

    port.WriteBytes(reinterpret_cast<const uint8_t*>(command), strlen(command));
    port.WriteBytes(reinterpret_cast<const uint8_t*>("\r\n"), 2);

    commandPending = true;
    pendingResult  = Result::Pending;
    commandTimeout.Start(timeoutMs);
    return true;
}

NinaAt::Result NinaAt::PollResult() const
{
    return commandPending ? Result::Pending : pendingResult;
}

int NinaAt::LastPeerHandle() const
{
    return parser.LastPeerHandle();
}

int NinaAt::LastBtMode() const
{
    return parser.LastBtMode();
}

bool NinaAt::NextEvent(Event& event)
{
    return parser.NextEvent(event);
}

void NinaAt::DiscardEvents()
{
    parser.ClearEvents();
}

void NinaAt::EnterDataMode()
{
    dataMode = true;
}

bool NinaAt::InDataMode() const
{
    return dataMode;
}

bool NinaAt::Available() const
{
    return port.Available();
}

uint8_t NinaAt::ReadByte()
{
    return port.ReadByte();
}

void NinaAt::WriteBytes(const uint8_t* const data, const size_t len)
{
    port.WriteBytes(data, len);
}

void NinaAt::Flush()
{
    port.Flush();
}
