/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "DelayTimer.h"
#include "Gpio.h"
#include "NinaUart.h"

#include "NinaLineParser.h"

using Boot::NinaUart;

// Hardware-owning driver for the on-board NINA-W152 (MainController-Spec.md
// §5): RESET_NINA GPIO, USART2 with hardware RTS/CTS flow control (NinaUart
// -- NinaCts/NinaRts are USART2 alternate-function pins, not plain GPIOs;
// see NinaUart.cpp), and the line parser above. Two operating modes:
//
//   - Command mode: one outstanding AT command at a time via SendCommand() /
//     PollResult(); unsolicited URCs drain through NextEvent() regardless of
//     whether a command is outstanding.
//   - Data mode (after "AT+UDCP=..." then "ATO" both succeed): raw
//     Available()/ReadByte()/WriteBytes() -- there are no AT lines to parse
//     any more, so Loop() stops feeding the parser once EnterDataMode() is
//     called. UplinkHandler encodes/decodes NodeLib frames by hand onto
//     these (NodeLib::Frame::Write() needs a Hal::Uart&, which this module
//     deliberately doesn't link -- see NinaUart.h).
class NinaAt
{
  public:
    NinaAt();

    // Releases RESET_NINA, holds NinaRts low (4-wire HW flow control is on by
    // default in u-connectXpress; nothing else on this link asserts it -- see
    // Software/NinaEnable, which proved exactly this sequence on the bench),
    // and brings up USART2 at 115200 8N1.
    void Init();

    // Pumps received bytes through the line parser. No-op in data mode --
    // the caller drains DataMode-mode bytes itself via Available()/ReadByte().
    void Loop();

    enum class Result
    {
        Pending,
        Ok,
        Error,
        Timeout,
    };

    // Queues one AT command (no trailing CRLF needed). Returns false if a
    // command is already outstanding -- the caller must PollResult() first.
    bool   SendCommand(const char* const command, const uint32_t timeoutMs = 2000);
    Result PollResult() const;

    // The most recently seen "+UDCP:<n>" peer handle (-1 if none yet).
    int LastPeerHandle() const;

    using Event = NinaLineParser::Event;
    bool NextEvent(Event& event);

    void    EnterDataMode();
    bool    InDataMode() const;
    bool    Available() const;
    uint8_t ReadByte();
    void    WriteBytes(const uint8_t* const data, const size_t len);

    // Wedge recovery (MainController-Server-Link-Spec.md §10): pulses
    // RESET_NINA, drops back to command mode, and resets the line parser.
    // Re-run the bring-up sequence (probe AT, Wi-Fi join, ...) afterwards.
    void PulseReset();

  private:
    Hal::Gpio ninaReset;
    NinaUart  uart;

    NinaLineParser parser;
    bool           dataMode;

    bool              commandPending;
    Result            pendingResult;
    Tools::DelayTimer commandTimeout;

    // PulseReset() used to hold RESET_NINA low via a blocking
    // Hal::Tick::DelayMs(100) -- harmless the one time Init() calls it before
    // the super-loop starts, but UplinkHandler::Fail() also calls it live,
    // every time a bring-up attempt fails (empirically, reliably at least
    // once on every boot on this network) -- a 100ms stall with nothing else
    // running is enough to blow through an entire bus poll/reply cycle and
    // lose a node's Done. Async now: PulseReset() only asserts reset and
    // arms this timer; Loop() releases it once the timer's done.
    bool              resetPending;
    Tools::DelayTimer resetTimer;
};
