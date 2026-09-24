/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "DelayTimer.h"
#include "Gpio.h"

#include "NinaLineParser.h"
#include "NinaPort.h"

// Driver for the on-board NINA-W152 (MainController-Spec.md §5) on top of a
// NinaPort: the RESET_NINA line, an AT command engine and the line parser.
// Shared by the application's and the bootloader's uplink. Two operating
// modes:
//
//   - Command mode: one outstanding AT command at a time via SendCommand() /
//     PollResult(); unsolicited URCs drain through NextEvent() regardless of
//     whether a command is outstanding.
//   - Data mode (after "AT+UDCP=..." then "ATO" both succeed): raw
//     Available()/ReadByte()/WriteBytes() -- there are no AT lines to parse any
//     more, so Loop() stops feeding the parser once EnterDataMode() is called.
class NinaAt
{
  public:
    explicit NinaAt(NinaPort& port);

    // Brings the port up at 115200 8N1 and pulses the module's reset.
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
    void DiscardEvents();

    void    EnterDataMode();
    bool    InDataMode() const;
    bool    Available() const;
    uint8_t ReadByte();
    void    WriteBytes(const uint8_t* const data, const size_t len);
    void    Flush();

    // Wedge recovery (MainController-Server-Link-Spec.md §10): pulses
    // RESET_NINA, drops back to command mode, and resets the line parser.
    // Re-run the bring-up sequence (probe AT, Wi-Fi join, ...) afterwards.
    void PulseReset();

  private:
    NinaPort& port;
    Hal::Gpio ninaReset;

    NinaLineParser parser;
    bool           dataMode;

    bool              commandPending;
    Result            pendingResult;
    Tools::DelayTimer commandTimeout;

    // PulseReset() is asynchronous: it only asserts reset and arms this
    // timer, and Loop() releases the line once it has run down -- a blocking
    // delay here stalled the bus loop every time a bring-up attempt failed.
    bool              resetPending;
    Tools::DelayTimer resetTimer;
};
