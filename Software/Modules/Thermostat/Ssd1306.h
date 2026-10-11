/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

#include "I2c.h"

// SSD1306/SSD1315 128x64 mono OLED (ControllerNode-Thermostat-Link-Spec.md
// §4.1, Wisevision X096-2864KSWPG01-H30), page-addressing mode, internal
// charge pump (the datasheet's I2C reference circuit).
//
// No bitmap font: numbers are 7-segment digits built from filled rectangles,
// so they scale to any size. The few icons are drawn with DrawGlyph() from
// '#'/'.' string art kept next to the code that uses them -- readable and
// editable as-is, and checkable in the PC simulator (Modules/Thermostat/sim).
class Ssd1306
{
  public:
    // SA0 tied low -- the common default for this class of module. The other
    // strap (0x3D) is a one-constant swap if the board ties it high instead
    // (§4.1: "check both datasheets before laying out" -- not yet confirmed
    // against CHT40MEMS's own 0x44/0x45, though those ranges can't collide
    // with this one regardless of either part's address strap).
    static constexpr uint8_t DefaultAddress = 0x3C;

    static constexpr int Width  = 128;
    static constexpr int Height = 64;
    static constexpr int Pages  = Height / 8;

    explicit Ssd1306(Hal::I2c& bus, const uint8_t address7 = DefaultAddress);

    void Init(); // full init sequence; panel left on (call Off() to sleep it)
    void On();
    void Off();

    void Clear();

    void SetPixel(const int x, const int y, const bool on);
    void FillRect(const int x, const int y, const int w, const int h, const bool on);
    void DrawRect(const int x, const int y, const int w, const int h, const bool on);
    void FillCircle(const int cx, const int cy, const int r, const bool on);
    void DrawCircle(const int cx, const int cy, const int r, const bool on);

    // One 7-segment digit (0-9; anything else is left blank), top-left at
    // (x,y), box w x h, segment thickness t.
    void DrawDigit(const int x, const int y, const int w, const int h, const int t, const uint8_t digit);

    // 'value' is the number already scaled by 10^decimals (e.g. 21.5 with
    // decimals=1 is passed as 215) -- avoids a generic power-of-ten split for
    // the only two shapes this UI needs (one decimal place, or none).
    void DrawNumber(
        const int x,
        const int y,
        const int w,
        const int h,
        const int t,
        int       value,
        const int decimals);

    // The width in pixels DrawNumber() takes for the same arguments -- for
    // centring or right-aligning a number.
    static int NumberWidth(const int w, const int t, int value, const int decimals);

    // String art: 'rows' are rowCount strings of equal length, '#' = pixel on,
    // anything else left untouched; top-left at (x,y).
    void       DrawGlyph(const int x, const int y, const char* const* const rows, const int rowCount);
    static int GlyphWidth(const char* const* const rows);

    // Pushes the whole framebuffer to the panel (one page-addressed write per
    // page). Nothing appears on the physical panel before this is called.
    void Flush();

  private:
    bool WriteCommand(const uint8_t cmd);

    Hal::I2c& bus;
    uint8_t   address;
    uint8_t   framebuffer[Width * Pages];
};
