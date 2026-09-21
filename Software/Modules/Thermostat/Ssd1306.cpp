/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include <string.h>

#include "Ssd1306.h"

namespace
{
    constexpr uint8_t ControlCommand = 0x00;
    constexpr uint8_t ControlData    = 0x40;

    // Standard SSD1306 128x64 bring-up sequence (internal charge pump, per
    // this board's I2C reference circuit -- ControllerNode-Thermostat-Link-
    // Spec.md §4.1). Segment remap (0xA1) and COM scan direction (0xC8) pick
    // the usual "right way up" mount; flip both (0xA0/0xC0) if the panel ends
    // up rotated 180 on the real board.
    const uint8_t initCommands[] = {
        0xAE, // display off
        0xD5,
        0x80, // clock divide ratio / oscillator frequency
        0xA8,
        0x3F, // multiplex ratio = 64
        0xD3,
        0x00, // display offset = 0
        0x40, // display start line = 0
        0x8D,
        0x14, // charge pump: enable, internal VCC
        0x20,
        0x02, // memory addressing mode = page
        0xA1, // segment remap
        0xC8, // COM output scan direction, remapped
        0xDA,
        0x12, // COM pins hardware configuration
        0x81,
        0x7F, // contrast
        0xD9,
        0xF1, // pre-charge period
        0xDB,
        0x40, // VCOMH deselect level
        0xA4, // resume to RAM content display
        0xA6, // normal (not inverted) display
    };
} // namespace

Ssd1306::Ssd1306(Hal::I2c& bus, const uint8_t address7) :
    bus(bus),
    address(address7),
    framebuffer{}
{
}

bool Ssd1306::WriteCommand(const uint8_t cmd)
{
    const uint8_t buf[2] = {ControlCommand, cmd};
    return bus.Write(address, buf, sizeof(buf));
}

void Ssd1306::Init()
{
    for (size_t i = 0; i < sizeof(initCommands); i++)
    {
        WriteCommand(initCommands[i]);
    }
    Clear();
    Flush();
    On();
}

void Ssd1306::On()
{
    WriteCommand(0xAF);
}

void Ssd1306::Off()
{
    WriteCommand(0xAE);
}

void Ssd1306::Clear()
{
    memset(framebuffer, 0, sizeof(framebuffer));
}

void Ssd1306::SetPixel(const int x, const int y, const bool on)
{
    if (x < 0 || x >= Width || y < 0 || y >= Height)
    {
        return;
    }
    uint8_t&      b    = framebuffer[(y / 8) * Width + x];
    const uint8_t mask = static_cast<uint8_t>(1u << (y % 8));
    b                  = on ? static_cast<uint8_t>(b | mask) : static_cast<uint8_t>(b & ~mask);
}

void Ssd1306::FillRect(const int x, const int y, const int w, const int h, const bool on)
{
    for (int yy = y; yy < y + h; yy++)
    {
        for (int xx = x; xx < x + w; xx++)
        {
            SetPixel(xx, yy, on);
        }
    }
}

void Ssd1306::DrawRect(const int x, const int y, const int w, const int h, const bool on)
{
    FillRect(x, y, w, 1, on);
    FillRect(x, y + h - 1, w, 1, on);
    FillRect(x, y, 1, h, on);
    FillRect(x + w - 1, y, 1, h, on);
}

void Ssd1306::FillCircle(const int cx, const int cy, const int r, const bool on)
{
    for (int y = -r; y <= r; y++)
    {
        for (int x = -r; x <= r; x++)
        {
            if (x * x + y * y <= r * r)
            {
                SetPixel(cx + x, cy + y, on);
            }
        }
    }
}

void Ssd1306::DrawCircle(const int cx, const int cy, const int r, const bool on)
{
    // Midpoint circle algorithm, 8-way symmetry.
    int x   = r;
    int y   = 0;
    int err = 0;
    while (x >= y)
    {
        SetPixel(cx + x, cy + y, on);
        SetPixel(cx + y, cy + x, on);
        SetPixel(cx - y, cy + x, on);
        SetPixel(cx - x, cy + y, on);
        SetPixel(cx - x, cy - y, on);
        SetPixel(cx - y, cy - x, on);
        SetPixel(cx + y, cy - x, on);
        SetPixel(cx + x, cy - y, on);
        y++;
        if (err <= 0)
        {
            err += 2 * y + 1;
        }
        if (err > 0)
        {
            x--;
            err -= 2 * x + 1;
        }
    }
}

void Ssd1306::DrawDigit(const int x, const int y, const int w, const int h, const int t, const uint8_t digit)
{
    // Segment truth table: {a, b, c, d, e, f, g} (classic 7-segment layout,
    //   _a_
    //  f   b
    //   _g_
    //  e   c
    //   _d_
    static const bool segments[10][7] = {
        {true, true, true, true, true, true, false}, // 0
        {false, true, true, false, false, false, false}, // 1
        {true, true, false, true, true, false, true}, // 2
        {true, true, true, true, false, false, true}, // 3
        {false, true, true, false, false, true, true}, // 4
        {true, false, true, true, false, true, true}, // 5
        {true, false, true, true, true, true, true}, // 6
        {true, true, true, false, false, false, false}, // 7
        {true, true, true, true, true, true, true}, // 8
        {true, true, true, true, false, true, true}, // 9
    };
    if (digit > 9)
    {
        return;
    }

    const bool* const seg    = segments[digit];
    const int         halfH  = h / 2;
    const int         lowerH = h - halfH;
    if (seg[0])
    {
        FillRect(x, y, w, t, true); // a: top
    }
    if (seg[3])
    {
        FillRect(x, y + h - t, w, t, true); // d: bottom
    }
    if (seg[6])
    {
        FillRect(x, y + halfH - t / 2, w, t, true); // g: middle
    }
    if (seg[5])
    {
        FillRect(x, y, t, halfH, true); // f: top-left
    }
    if (seg[1])
    {
        FillRect(x + w - t, y, t, halfH, true); // b: top-right
    }
    if (seg[4])
    {
        FillRect(x, y + halfH, t, lowerH, true); // e: bottom-left
    }
    if (seg[2])
    {
        FillRect(x + w - t, y + halfH, t, lowerH, true); // c: bottom-right
    }
}

void Ssd1306::DrawNumber(
    const int x,
    const int y,
    const int w,
    const int h,
    const int t,
    int       value,
    const int decimals)
{
    const bool negative = value < 0;
    if (negative)
    {
        value = -value;
    }

    uint8_t digits[6];
    int     n = 0;
    if (value == 0)
    {
        digits[n++] = 0;
    }
    while (value > 0 && n < static_cast<int>(sizeof(digits)))
    {
        digits[n++] = static_cast<uint8_t>(value % 10);
        value /= 10;
    }
    while (n <= decimals && n < static_cast<int>(sizeof(digits)))
    {
        digits[n++] = 0; // e.g. "0.5", not ".5"
    }

    const int gap    = w / 4 + 1;
    int       cursor = x;
    if (negative)
    {
        FillRect(cursor, y + h / 2 - t / 2, w / 2, t, true);
        cursor += w / 2 + gap;
    }
    for (int i = n - 1; i >= 0; i--)
    {
        DrawDigit(cursor, y, w, h, t, digits[i]);
        cursor += w + gap;
        if (decimals > 0 && i == decimals)
        {
            FillRect(cursor, y + h - t, t, t, true); // decimal point
            cursor += t + gap;
        }
    }
}

void Ssd1306::Flush()
{
    for (int page = 0; page < Pages; page++)
    {
        const uint8_t setPage[4] = {ControlCommand, static_cast<uint8_t>(0xB0 + page), 0x00, 0x10};
        bus.Write(address, setPage, sizeof(setPage));

        uint8_t buf[1 + Width];
        buf[0] = ControlData;
        memcpy(&buf[1], &framebuffer[page * Width], Width);
        bus.Write(address, buf, sizeof(buf));
    }
}
