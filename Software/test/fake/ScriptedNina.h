/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A scripted stand-in for the on-board NINA-W152 running u-connectXpress, at
// the level the firmware sees it: text over a UART. Transport-agnostic -- a
// harness feeds it the bytes the MCU wrote and the current time, and collects
// the bytes it wants delivered to the MCU -- so the same model drives both the
// application's Hal::Uart-based driver and the bootloader's own NinaUart.
//
// Behaviour is a list of rules (first match wins): an AT command prefix, the
// response text (after the echo), how long the module takes, and unsolicited
// result codes (URCs) that follow. Scenarios put special rules in front of the
// defaults to script failures ("ERROR" twice, then OK; no network; ...).
class ScriptedNina
{
  public:
    struct Rule
    {
        const char* prefix;
        bool        exact; // whole line must equal prefix (for plain "AT")
        const char* response; // sent after the echo, e.g. "OK\r\n"
        uint32_t    delayMs; // module latency before 'response'
        const char* urcs; // sent 'urcDelayMs' after the response (may be "")
        uint32_t    urcDelayMs;
        int         times; // uses left; -1 = unlimited
        bool        enterData; // after 'response', switch the UART to data mode
    };

    ScriptedNina()
    {
        Reset();
    }

    void Reset()
    {
        ruleCount      = 0;
        scheduledCount = 0;
        outLen         = 0;
        dataLen        = 0;
        lineLen        = 0;
        inDataMode     = false;
        strayMode      = false;
        echo           = true;
        bootDelayMs    = 150;
        resetHeld      = false;
        bootedAt       = 0;
        netReadyAt     = 0xFFFFFFFFu;
        udcpCount      = 0;
        btMode         = 0;
        storedBtMode   = 0;
        pendingBtMode  = -1;
        softRestarts   = 0;
        AddDefaults();
    }

    // Scenario rules go in front of what is already there.
    void AddFirst(const Rule& rule)
    {
        if (ruleCount == maxRules)
        {
            return;
        }
        memmove(&rules[1], &rules[0], ruleCount * sizeof(Rule));
        rules[0] = rule;
        ruleCount++;
    }

    // --- transport side ------------------------------------------------------

    // The module's reset line: low = held in reset (everything lost), release =
    // it boots and is silent for bootDelayMs.
    void OnResetLine(const bool low, const uint32_t now)
    {
        if (low)
        {
            // The module takes a moment to actually go down after its reset
            // line is pulled (seen on the bench): whatever it was about to say
            // still reaches the MCU.
            for (size_t i = 0; i < scheduledCount; i++)
            {
                if (scheduled[i].due <= now + resetLagMs)
                {
                    Emit(scheduled[i]);
                }
            }
            netReadyAt     = 0xFFFFFFFFu;
            resetHeld      = true;
            inDataMode     = false;
            strayMode      = false;
            lineLen        = 0;
            scheduledCount = 0;
            resets++;
        }
        else if (resetHeld)
        {
            resetHeld = false;
            bootedAt  = now + bootDelayMs;
        }
    }

    void OnMcuBytes(const uint8_t* const data, const size_t len, const uint32_t now)
    {
        if (resetHeld || now < bootedAt)
        {
            return; // in reset / still booting: bytes are lost
        }
        if (inDataMode)
        {
            const size_t room = dataCap - dataLen;
            const size_t n    = len < room ? len : room;
            memcpy(&dataBuf[dataLen], data, n);
            dataLen += n;
            return;
        }
        if (strayMode)
        {
            // The peer is gone and the module is back in command mode: worst
            // case, it answers every stray write with an error.
            if (strayErrors)
            {
                Schedule(now, "ERROR\r\n");
            }
            return;
        }
        for (size_t i = 0; i < len; i++)
        {
            if (data[i] == '\r' || data[i] == '\n')
            {
                if (lineLen > 0)
                {
                    line[lineLen] = '\0';
                    HandleLine(now);
                    lineLen = 0;
                }
            }
            else if (lineLen < sizeof(line) - 1)
            {
                line[lineLen++] = static_cast<char>(data[i]);
            }
        }
    }

    // Emits everything that has become due.
    void Tick(const uint32_t now)
    {
        for (size_t i = 0; i < scheduledCount;)
        {
            if (scheduled[i].due <= now)
            {
                Emit(scheduled[i]);
                memmove(&scheduled[i], &scheduled[i + 1], (scheduledCount - i - 1) * sizeof(Scheduled));
                scheduledCount--;
            }
            else
            {
                i++;
            }
        }
    }

    size_t TakeOutput(uint8_t* const buf, const size_t cap)
    {
        const size_t n = outLen < cap ? outLen : cap;
        memcpy(buf, outBuf, n);
        memmove(outBuf, &outBuf[n], outLen - n);
        outLen -= n;
        return n;
    }

    // --- peer side (what the "server" does) ---------------------------------

    // Bytes the MCU wrote while in data mode (i.e. what reached the server).
    size_t TakeData(uint8_t* const buf, const size_t cap)
    {
        const size_t n = dataLen < cap ? dataLen : cap;
        memcpy(buf, dataBuf, n);
        memmove(dataBuf, &dataBuf[n], dataLen - n);
        dataLen -= n;
        return n;
    }

    // Bytes from the server to the MCU (only while the pipe is up).
    void ServerSend(const uint8_t* const data, const size_t len)
    {
        if (!inDataMode)
        {
            return;
        }
        for (size_t i = 0; i < len && outLen < outCap; i++)
        {
            outBuf[outLen++] = data[i];
        }
    }

    // The TCP peer goes away: the module reports it and drops to command mode.
    void DropPeer(const uint32_t now)
    {
        if (!inDataMode)
        {
            return;
        }
        inDataMode = false;
        strayMode  = true;
        Schedule(now, "+UUDPD:0\r\n");
    }

    bool InDataMode() const
    {
        return inDataMode;
    }

    int Resets() const
    {
        return resets;
    }

    // AT+UDCP commands received so far.
    int UdcpCount() const
    {
        return udcpCount;
    }

    // Restarts by AT+CPWROFF (not the reset line -- Resets() doesn't count these).
    int SoftRestarts() const
    {
        return softRestarts;
    }

    // Bluetooth mode (AT+UBTMODE) currently in effect / stored for next start.
    // Starts at 0; set both to model a module whose stored settings have it on.
    int btMode;
    int storedBtMode;

    // Knobs.
    bool     echo;
    uint32_t bootDelayMs;
    bool     strayErrors = false; // after DropPeer(): answer every stray write with ERROR
    // Bench behaviour: after AT+UWSCA the IP-up URC (+UUNU) fires twice, this
    // far apart, and AT+UDCP answers ERROR until the second one has fired.
    uint32_t secondNetworkUpMs = 0;
    // How long the module keeps talking after its reset line is pulled.
    uint32_t resetLagMs = 0;

  private:
    static const size_t maxRules     = 24;
    static const size_t maxScheduled = 48;
    static const size_t outCap       = 4096;
    static const size_t dataCap      = 8192;

    struct Scheduled
    {
        uint32_t due;
        char     text[220];
        bool     enterData;
    };

    void AddDefaults()
    {
        // Wi-Fi station profile, then activation: the link and IP URCs follow.
        rules[ruleCount++] = {"AT+UWSC=", false, "OK\r\n", 10, "", 0, -1, false};
        rules[ruleCount++] = {"AT+UWSCA=", false, "OK\r\n", 20, "+UUWLE:0,001122334455,6\r\n+UUNU:0,192.168.2.50,255.255.255.0,192.168.2.1,192.168.2.1\r\n", 100, -1, false};
        // TCP peer: handle, then the connected URC.
        rules[ruleCount++] = {"AT+UDCP=", false, "+UDCP:0\r\nOK\r\n", 30, "+UUDPC:0,1,0,192.168.2.50,50000,192.168.2.89,9000\r\n", 80, -1, false};
        rules[ruleCount++] = {"ATO", true, "OK\r\n", 10, "", 0, -1, true};
        rules[ruleCount++] = {"AT", true, "OK\r\n", 5, "", 0, -1, false};
    }

    void Schedule(const uint32_t due, const char* const text, const bool enterData = false)
    {
        if (scheduledCount == maxScheduled)
        {
            return;
        }
        Scheduled& s = scheduled[scheduledCount++];
        s.due        = due;
        s.enterData  = enterData;
        strncpy(s.text, text, sizeof(s.text) - 1);
        s.text[sizeof(s.text) - 1] = '\0';
    }

    void Emit(const Scheduled& s)
    {
        for (const char* p = s.text; *p != '\0' && outLen < outCap; p++)
        {
            outBuf[outLen++] = static_cast<uint8_t>(*p);
        }
        if (s.enterData)
        {
            inDataMode = true;
        }
    }

    // AT+UBTMODE?/=, AT&W and AT+CPWROFF, modelled on the real module: the
    // write only lands in the stored settings on AT&W, and only takes effect
    // after the restart AT+CPWROFF performs (silent for bootDelayMs, like a
    // reset). Returns true if the line was one of these.
    bool HandleBluetoothSetting(const uint32_t now)
    {
        if (strcmp(line, "AT+UBTMODE?") == 0)
        {
            char r[32];
            snprintf(r, sizeof(r), "+UBTMODE:%d\r\nOK\r\n", btMode);
            Schedule(now + 5, r);
            return true;
        }
        if (strncmp(line, "AT+UBTMODE=", 11) == 0)
        {
            pendingBtMode = atoi(&line[11]);
            Schedule(now + 5, "OK\r\n");
            return true;
        }
        if (strcmp(line, "AT&W") == 0)
        {
            if (pendingBtMode >= 0)
            {
                storedBtMode = pendingBtMode;
            }
            Schedule(now + 20, "OK\r\n");
            return true;
        }
        if (strcmp(line, "AT+CPWROFF") == 0)
        {
            Schedule(now + 5, "OK\r\n");
            btMode        = storedBtMode;
            pendingBtMode = -1;
            bootedAt      = now + 10 + bootDelayMs; // restarts right after the OK
            softRestarts++;
            return true;
        }
        return false;
    }

    void HandleLine(const uint32_t now)
    {
        if (echo)
        {
            char e[sizeof(line) + 3];
            snprintf(e, sizeof(e), "%s\r\n", line);
            Schedule(now, e);
        }

        if (HandleBluetoothSetting(now))
        {
            return;
        }

        const bool isUdcp = strncmp(line, "AT+UDCP=", 8) == 0;
        if (isUdcp)
        {
            udcpCount++;
        }
        for (size_t i = 0; i < ruleCount; i++)
        {
            Rule& r = rules[i];
            if (r.times == 0)
            {
                continue;
            }
            const bool match = r.exact ? strcmp(line, r.prefix) == 0 : strncmp(line, r.prefix, strlen(r.prefix)) == 0;
            if (!match)
            {
                continue;
            }
            if (r.times > 0)
            {
                r.times--;
            }
            if (strncmp(line, "AT+UWSCA=", 9) == 0)
            {
                // The network is really up once the rule's IP-up URC has
                // fired -- or, in the bench's two-step case, its repeat.
                netReadyAt = now + r.delayMs + r.urcDelayMs + secondNetworkUpMs;
                if (secondNetworkUpMs != 0)
                {
                    Schedule(netReadyAt, "+UUNU:0,192.168.2.50,255.255.255.0,192.168.2.1,192.168.2.1\r\n");
                }
            }
            if (isUdcp && now < netReadyAt && strncmp(r.response, "+UDCP", 5) == 0)
            {
                Schedule(now + r.delayMs, "ERROR\r\n"); // the network is not really up yet
                return;
            }
            Schedule(now + r.delayMs, r.response, r.enterData);
            if (r.urcs[0] != '\0')
            {
                Schedule(now + r.delayMs + r.urcDelayMs, r.urcs);
            }
            return;
        }
        Schedule(now + 5, "ERROR\r\n");
    }

    Rule   rules[maxRules];
    size_t ruleCount;

    Scheduled scheduled[maxScheduled];
    size_t    scheduledCount;

    uint8_t outBuf[outCap];
    size_t  outLen;
    uint8_t dataBuf[dataCap];
    size_t  dataLen;

    char   line[160];
    size_t lineLen;

    bool     inDataMode;
    bool     strayMode;
    bool     resetHeld;
    uint32_t bootedAt;
    int      resets = 0;
    uint32_t netReadyAt;
    int      udcpCount;
    int      pendingBtMode; // written by AT+UBTMODE=, not yet stored
    int      softRestarts;
};
