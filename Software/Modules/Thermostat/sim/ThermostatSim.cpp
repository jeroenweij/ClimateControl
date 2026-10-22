/*************************************************************
 * Created by J. Weij
 *
 * Thermostat PC simulator.
 * Runs the real ThermostatHandler + Ssd1306 driver + NodeLib::Node on the
 * host, against the unit-test fakes plus SimI2c (an emulated SSD1306 panel
 * and CHT40 sensor), and draws the panel in the terminal -- for working on
 * the UI without hardware. The paired ControllerNode is played by the
 * simulator: it polls the link every 200 ms and pushes DamperActual and
 * AirMode.
 *
 *   make -C Software sim-thermostat        (or: build-test/ThermostatSim [-2])
 *************************************************************/

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "BoardPins.h"
#include "Logger.h"
#include "Node.h"
#include "RoomDemand.h"
#include "stm32g0xx_hal.h"

#include "Ssd1306.h"
#include "ThermostatHandler.h"

#include "BusHelpers.h"
#include "FakeBus.h"
#include "FakeClock.h"
#include "FakeConfigStore.h"
#include "SimI2c.h"

using NodeLib::Endpoint;
using NodeLib::Id;
using NodeLib::Message;
using NodeLib::Operation;

namespace
{
    const uint8_t nodeId = NodeLib::THERMOSTAT_NODE_ID;

    constexpr uint32_t PollIntervalMs = 200; // LinkMaster's poll period
    constexpr uint32_t DamperResendMs = 1000;
    constexpr uint32_t ButtonHoldMs   = 120; // a terminal has no key-up: a key is a press + release
    constexpr uint32_t FramePeriodMs  = 33;
    constexpr int      SnapshotScale  = 4;

    // --- simulated world ----------------------------------------------------
    int16_t  roomCentiDegC = 2150;
    uint16_t roomCentiRH   = 4500;
    bool     sensorPresent = true;
    bool     linkEnabled   = true;
    uint8_t  damperPercent = 40;
    uint8_t  airMode       = static_cast<uint8_t>(NodeLib::AirMode::Heating);
    bool     damperChanged = true;
    uint32_t upReleaseAt   = 0;
    uint32_t downReleaseAt = 0;

    // What the ControllerNode side last heard from the Thermostat.
    bool    seenSetpoint = false;
    int16_t lastSetpoint = 0;
    bool    seenTemp     = false;
    int16_t lastTemp     = 0;
    bool    seenRh       = false;
    int16_t lastRh       = 0;

    char lastLog[160] = "-";
    bool statusDirty  = true; // the text under the panel changed

    int scale = 1;

    // --- terminal -----------------------------------------------------------
    termios savedTermios;
    bool    termiosSaved = false;

    void RestoreTerminal()
    {
        if (termiosSaved)
        {
            tcsetattr(STDIN_FILENO, TCSANOW, &savedTermios);
            termiosSaved = false;
        }
        fputs("\x1b[0m\x1b[?25h\x1b[?1049l", stdout); // reset colours, show cursor, leave alt screen
        fflush(stdout);
    }

    void OnSignal(const int)
    {
        RestoreTerminal();
        _exit(0);
    }

    void SetupTerminal()
    {
        if (tcgetattr(STDIN_FILENO, &savedTermios) == 0)
        {
            termiosSaved = true;
            // VMIN/VTIME 0: read() returns at once with whatever is there.
            // (Not O_NONBLOCK -- stdin and stdout share the tty's file
            // description, and a non-blocking stdout drops frame bytes.)
            termios raw = savedTermios;
            raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
            raw.c_cc[VMIN]  = 0;
            raw.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        }
        atexit(RestoreTerminal);
        signal(SIGINT, OnSignal);
        signal(SIGTERM, OnSignal);
        fputs("\x1b[?1049h\x1b[?25l\x1b[2J", stdout); // alt screen, hide cursor, clear
    }

    uint32_t WallMs()
    {
        static timespec start{};
        timespec        now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (start.tv_sec == 0 && start.tv_nsec == 0)
        {
            start = now;
        }
        return static_cast<uint32_t>((now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000);
    }

    // --- GPIO: buttons are active-low on GPIOA (fake register block) --------
    void SetButton(const Hal::Pin& button, const bool pressed)
    {
        if (pressed)
        {
            button.port->IDR &= ~static_cast<uint32_t>(button.pin);
        }
        else
        {
            button.port->IDR |= button.pin;
        }
    }

    void Press(const Hal::Pin& button, uint32_t& releaseAt)
    {
        SetButton(button, true);
        releaseAt = FakeClock::Now() + ButtonHoldMs;
    }

    // The sensor reads the room plus the board's self-heating, which the
    // handler's RoomTempOffsetCentiDegC takes back out.
    void SetSensorRoom()
    {
        SimI2c::SetRoom(static_cast<int16_t>(roomCentiDegC - ThermostatHandler::RoomTempOffsetCentiDegC), roomCentiRH);
    }

    void ReleaseDueButtons()
    {
        if (upReleaseAt != 0 && FakeClock::Now() >= upReleaseAt)
        {
            SetButton(Board::UserButton, false);
            upReleaseAt = 0;
        }
        if (downReleaseAt != 0 && FakeClock::Now() >= downReleaseAt)
        {
            SetButton(Board::UserButton2, false);
            downReleaseAt = 0;
        }
    }

    // --- the ControllerNode end of the link ---------------------------------
    void CollectThermostatTx()
    {
        Message   tx[32];
        const int n = bus::DecodeTx(tx, 32);
        for (int i = 0; i < n; i++)
        {
            if (tx[i].id.operation != Operation::Report || tx[i].len < 2)
            {
                continue;
            }
            const int16_t v = static_cast<int16_t>(tx[i].data[0] | (tx[i].data[1] << 8));
            statusDirty     = true;
            switch (tx[i].id.endpoint)
            {
                case Endpoint::RoomSetpoint:
                    seenSetpoint = true;
                    lastSetpoint = v;
                    break;
                case Endpoint::RoomTemp:
                    seenTemp = true;
                    lastTemp = v;
                    break;
                case Endpoint::RoomHumidity:
                    seenRh = true;
                    lastRh = v;
                    break;
                default:
                    break;
            }
        }
        FakeBus::TruncateTx(0);
    }

    void ServiceLink(uint32_t& nextPoll, uint32_t& nextDamper)
    {
        const uint32_t now = FakeClock::Now();
        if (!linkEnabled || now < nextPoll)
        {
            return;
        }
        nextPoll = now + PollIntervalMs;

        // FakeBus's RX buffer only grows -- start it over once the node has
        // consumed everything (it always has, one Loop() after an inject).
        FakeBus::Reset();
        bus::InjectFrame(Message(nodeId, Operation::Poll));
        if (damperChanged || now >= nextDamper)
        {
            Message m(Id(nodeId, Endpoint::DamperActual, Operation::Set));
            m.data[0] = damperPercent;
            m.len     = 1;
            bus::InjectFrame(m);
            Message air(Id(nodeId, Endpoint::AirMode, Operation::Set));
            air.data[0] = airMode;
            air.len     = 1;
            bus::InjectFrame(air);
            damperChanged = false;
            nextDamper    = now + DamperResendMs;
        }
    }

    // --- rendering ----------------------------------------------------------
    // One whole terminal frame, written in a single fwrite() so it doesn't
    // flicker. (No std::string: Lib/Tools/SStream.h's std::stringstream
    // stand-in can't coexist with libstdc++'s <string>.)
    char   frame[256 * 1024];
    size_t frameLen = 0;

    void Out(const char* const fmt, ...) __attribute__((format(printf, 1, 2)));
    void Out(const char* const fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        const int n = vsnprintf(&frame[frameLen], sizeof(frame) - frameLen, fmt, args);
        va_end(args);
        if (n > 0)
        {
            frameLen += static_cast<size_t>(n);
            if (frameLen >= sizeof(frame))
            {
                frameLen = sizeof(frame) - 1;
            }
        }
    }

    void OutRepeat(const char c, const int count)
    {
        for (int i = 0; i < count && frameLen + 1 < sizeof(frame); i++)
        {
            frame[frameLen++] = c;
        }
    }

    // Formats into one of a few rotating static buffers, so several can sit
    // in the same printf argument list.
    const char* FormatCenti(const int v, const char* const unit)
    {
        static char buf[4][32];
        static int  next = 0;
        char* const out  = buf[next];
        next             = (next + 1) % 4;
        snprintf(out, sizeof(buf[0]), "%s%d.%02d%s", v < 0 ? "-" : "", abs(v) / 100, abs(v) % 100, unit);
        return out;
    }

    // Current terminal colours within a panel row: -1 unknown, else lit.
    int fgLit = -1;
    int bgLit = -1;

    void OutColour(const bool fg, const bool lit)
    {
        int& current = fg ? fgLit : bgLit;
        if (current == static_cast<int>(lit))
        {
            return;
        }
        current = lit;
        // Lit pixels white (the X096 panel is white-on-black); unlit a very
        // dark grey so the glass outline stays visible.
        const int c = lit ? 0xF0 : 0x14;
        Out("\x1b[%d;2;%d;%d;%dm", fg ? 38 : 48, c, c, lit ? 0xFF : 0x18);
    }

    void Render(const uint32_t now)
    {
        frameLen = 0;
        Out("\x1b[H\x1b[0m Thermostat simulator -- real ThermostatHandler/Ssd1306 code on a fake I2C bus\x1b[K\n\n");

        const int cols = Ssd1306::Width * scale;
        Out(" +");
        OutRepeat('-', cols);
        Out("+\x1b[K\n");

        // Each terminal cell is two stacked pixels ('upper half block', fg =
        // top pixel, bg = bottom pixel); at scale 2 each pixel is 2x2 of those.
        const int rows = Ssd1306::Height * scale / 2;
        for (int r = 0; r < rows; r++)
        {
            Out(" |");
            fgLit = -1;
            bgLit = -1;
            for (int c = 0; c < cols; c++)
            {
                const int x = c / scale;
                OutColour(true, SimI2c::Pixel(x, (r * 2) / scale));
                OutColour(false, SimI2c::Pixel(x, (r * 2 + 1) / scale));
                Out("\u2580");
            }
            Out("\x1b[0m|\x1b[K\n");
        }
        Out(" +");
        OutRepeat('-', cols);
        Out("+\x1b[K\n\n");

        Out(" panel %-6s   t=%6.1f s   room %s %s   sensor %-7s   link %-4s   damper %3u%%\x1b[K\n",
            SimI2c::PanelOn() ? "ON" : "asleep",
            now / 1000.0,
            FormatCenti(roomCentiDegC, "C"),
            FormatCenti(roomCentiRH, "%RH"),
            sensorPresent ? "ok" : "MISSING",
            linkEnabled ? "up" : "DOWN",
            damperPercent);
        static const char* const airNames[] = {"unknown", "heating", "cooling"};
        Out(" air mode (from ControllerNode) %s\x1b[K\n", airMode < 3 ? airNames[airMode] : "?");
        Out(" ControllerNode last heard: setpoint %s  temp %s  RH %s\x1b[K\n",
            seenSetpoint ? FormatCenti(lastSetpoint, "C") : "-",
            seenTemp ? FormatCenti(lastTemp, "C") : "-",
            seenRh ? FormatCenti(lastRh, "%") : "-");
        Out(" last log: %s\x1b[K\n\n", lastLog);

        Out(" Up/Down or +/-  thermostat buttons (setpoint +/-0.5, wakes panel)\x1b[K\n");
        Out(" t/T room temp -/+0.1   h/H humidity -/+1%%   d/D damper -/+10%%\x1b[K\n");
        Out(" a air mode: unknown -> heating -> cooling\x1b[K\n");
        Out(" s sensor present/missing   l link up/down   p snapshot (.pgm)   q quit\x1b[K\n\x1b[J");

        fwrite(frame, 1, frameLen, stdout);
        fflush(stdout);
    }

    void Snapshot()
    {
        char name[64];
        snprintf(name, sizeof(name), "thermostat-%u.pgm", FakeClock::Now());
        FILE* const f = fopen(name, "wb");
        if (f == nullptr)
        {
            snprintf(lastLog, sizeof(lastLog), "snapshot failed");
            return;
        }
        const int w = Ssd1306::Width * SnapshotScale;
        const int h = Ssd1306::Height * SnapshotScale;
        fprintf(f, "P5\n%d %d\n255\n", w, h);
        for (int y = 0; y < h; y++)
        {
            for (int x = 0; x < w; x++)
            {
                fputc(SimI2c::Pixel(x / SnapshotScale, y / SnapshotScale) ? 0xF0 : 0x10, f);
            }
        }
        fclose(f);
        snprintf(lastLog, sizeof(lastLog), "snapshot saved to %s", name);
    }

    // Returns false on quit.
    bool HandleKeys()
    {
        char          buf[16];
        const ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n > 0)
        {
            statusDirty = true;
        }
        for (ssize_t i = 0; i < n; i++)
        {
            char k = buf[i];
            if (k == '\x1b' && i + 2 < n && buf[i + 1] == '[')
            {
                k = buf[i + 2] == 'A' ? '+' : (buf[i + 2] == 'B' ? '-' : 0);
                i += 2;
            }
            switch (k)
            {
                case '+':
                case '=':
                    Press(Board::UserButton, upReleaseAt);
                    break;
                case '-':
                case '_':
                    Press(Board::UserButton2, downReleaseAt);
                    break;
                case 't':
                    roomCentiDegC = static_cast<int16_t>(roomCentiDegC - 10);
                    break;
                case 'T':
                    roomCentiDegC = static_cast<int16_t>(roomCentiDegC + 10);
                    break;
                case 'h':
                    roomCentiRH = static_cast<uint16_t>(roomCentiRH >= 100 ? roomCentiRH - 100 : 0);
                    break;
                case 'H':
                    roomCentiRH = static_cast<uint16_t>(roomCentiRH <= 9900 ? roomCentiRH + 100 : 10000);
                    break;
                case 'd':
                    damperPercent = static_cast<uint8_t>(damperPercent >= 10 ? damperPercent - 10 : 0);
                    damperChanged = true;
                    break;
                case 'D':
                    damperPercent = static_cast<uint8_t>(damperPercent <= 90 ? damperPercent + 10 : 100);
                    damperChanged = true;
                    break;
                case 'a':
                    airMode       = static_cast<uint8_t>((airMode + 1) % 3);
                    damperChanged = true;
                    break;
                case 's':
                    sensorPresent = !sensorPresent;
                    break;
                case 'l':
                    linkEnabled   = !linkEnabled;
                    damperChanged = true;
                    break;
                case 'p':
                    Snapshot();
                    break;
                case 'q':
                case 'Q':
                    return false;
                default:
                    break;
            }
        }
        SetSensorRoom();
        SimI2c::SetSensorPresent(sensorPresent);
        return true;
    }
} // namespace

// Strong override of Logger's weak sink: the handler's log lines go to the
// status area instead of scribbling over the panel.
void Tools::Logger::Write(const char level, const char* const msg)
{
    snprintf(lastLog, sizeof(lastLog), "%c: %s", level, msg);
    statusDirty = true;
}

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-2") == 0)
        {
            scale = 2;
        }
        else
        {
            fprintf(stderr, "usage: %s [-2]   (-2: draw the panel at double size)\n", argv[0]);
            return 1;
        }
    }

    FakeBus::Reset();
    FakeClock::Reset();
    FakeConfig::Reset();
    FakeConfig::SetValid(true);
    FakeConfig::SetNodeId(nodeId);
    FakeConfig::SetModule(NodeLib::ModuleType::Thermostat);
    SetButton(Board::UserButton2, false);
    SetButton(Board::UserButton, false);
    SetSensorRoom();

    // Wired exactly as Modules/Thermostat/main.cpp does.
    NodeLib::Node     node(Hal::Uart::Instance::Usart2, Board::LinkUart, 0);
    ThermostatHandler handler(node);
    handler.Init();
    node.RegisterHandler(&handler);
    node.Init();

    SetupTerminal();

    uint32_t nextPoll    = 0;
    uint32_t nextDamper  = 0;
    uint32_t nextFrame   = 0;
    uint32_t shownRev    = ~0u;
    uint32_t shownSecond = ~0u;
    while (HandleKeys())
    {
        // Firmware time follows the wall clock; Hal::Tick::DelayMs() inside
        // the code under test may push it ahead, and the wall catches up.
        const uint32_t wall = WallMs();
        if (wall > FakeClock::Now())
        {
            FakeClock::Set(wall);
        }

        ReleaseDueButtons();
        ServiceLink(nextPoll, nextDamper);

        handler.Loop();
        node.Loop();
        CollectThermostatTx();

        const uint32_t now = FakeClock::Now();
        if (now >= nextFrame && (statusDirty || SimI2c::Revision() != shownRev || now / 1000 != shownSecond))
        {
            Render(now);
            shownRev    = SimI2c::Revision();
            shownSecond = now / 1000;
            statusDirty = false;
            nextFrame   = now + FramePeriodMs;
        }

        usleep(1000);
    }
    return 0;
}
