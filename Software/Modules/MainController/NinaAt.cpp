/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include <string.h>

#include "BoardPins.h"

#include "NinaAt.h"

using Hal::UartPin;

namespace
{
    const uint32_t NinaBaud = 115200;
}

NinaAt::NinaAt() :
    ninaReset(Board::NinaReset, Hal::Gpio::Mode::OpenDrain),
    ninaRts(Board::NinaRts, Hal::Gpio::Mode::Output),
    uart(),
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
    // NinaRts (STM32 PA1) -> NINA UART_CTS (module pin 21): held low so the
    // module always considers itself clear to transmit -- u-connectXpress
    // ships with 4-wire HW flow control on by default and nothing else on
    // this link asserts it (proven on the bench by Software/NinaEnable).
    ninaRts.Write(false);

    uart.Init(NinaBaud, Hal::Uart::Instance::Usart2, UartPin{Board::Usart2Tx, Board::Usart2Af}, UartPin{Board::Usart2Rx, Board::Usart2Af});

    PulseReset();
}

void NinaAt::PulseReset()
{
    // Active-low, open-drain; >=50 us low per the module's datasheet
    // (MainController-Spec.md §5). Async -- see the resetPending/resetTimer
    // comment in NinaAt.h -- Loop() releases it once resetPulseMs elapses.
    // The 100 ms margin is generous but no longer costs anything: nothing
    // else here blocks waiting for it.
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

    while (uart.Available())
    {
        const NinaLineParser::LineResult lineResult = parser.FeedByte(uart.ReadByte());
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

    uart.WriteBytes(reinterpret_cast<const uint8_t*>(command), strlen(command));
    uart.WriteBytes(reinterpret_cast<const uint8_t*>("\r\n"), 2);

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

bool NinaAt::NextEvent(Event& event)
{
    return parser.NextEvent(event);
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
    return uart.Available();
}

uint8_t NinaAt::ReadByte()
{
    return uart.ReadByte();
}

void NinaAt::WriteBytes(const uint8_t* const data, const size_t len)
{
    uart.WriteBytes(data, len);
}

Hal::Uart& NinaAt::RawUart()
{
    return uart;
}
