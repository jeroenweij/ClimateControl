/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "Cht40.h"
#include "I2c.h"
#include "Ssd1306.h"

#include "SimI2c.h"

using Hal::I2c;
using Hal::I2cPins;

namespace
{
    // --- SSD1306 controller state -----------------------------------------
    uint8_t  gddram[Ssd1306::Pages][Ssd1306::Width];
    bool     displayOn   = false;
    bool     segRemap    = false; // 0xA1: column 0 drives SEG127
    bool     comRemap    = false; // 0xC8: scan COM63 -> COM0
    bool     inverse     = false; // 0xA7
    uint8_t  contrast    = 0x7F;
    int      page        = 0;
    int      column      = 0;
    int      pendingArgs = 0; // argument bytes still owed to the last command
    uint8_t  argCommand  = 0;
    uint32_t revision    = 0;

    // Commands that take N trailing argument bytes (SSD1306 datasheet §9).
    int ArgCount(const uint8_t cmd)
    {
        switch (cmd)
        {
            case 0x81: // contrast
            case 0x20: // addressing mode
            case 0xA8: // multiplex
            case 0xD3: // display offset
            case 0xD5: // clock
            case 0xD9: // pre-charge
            case 0xDA: // COM pins
            case 0xDB: // VCOMH
            case 0x8D: // charge pump
                return 1;
            case 0x21: // column address (horizontal/vertical mode)
            case 0x22: // page address
                return 2;
            default:
                return 0;
        }
    }

    void Command(const uint8_t b)
    {
        if (pendingArgs > 0)
        {
            if (argCommand == 0x81)
            {
                contrast = b;
                revision++;
            }
            pendingArgs--;
            return;
        }

        if (b >= 0xB0 && b <= 0xB7)
        {
            page = b - 0xB0;
        }
        else if (b <= 0x0F)
        {
            column = (column & 0xF0) | b;
        }
        else if (b >= 0x10 && b <= 0x1F)
        {
            column = (column & 0x0F) | ((b & 0x0F) << 4);
        }
        else if (b == 0xAE || b == 0xAF)
        {
            displayOn = b == 0xAF;
            revision++;
        }
        else if (b == 0xA0 || b == 0xA1)
        {
            segRemap = b == 0xA1;
            revision++;
        }
        else if (b == 0xC0 || b == 0xC8)
        {
            comRemap = b == 0xC8;
            revision++;
        }
        else if (b == 0xA6 || b == 0xA7)
        {
            inverse = b == 0xA7;
            revision++;
        }
        else
        {
            argCommand  = b;
            pendingArgs = ArgCount(b);
        }
    }

    void Data(const uint8_t b)
    {
        gddram[page][column & 0x7F] = b;
        column                      = (column + 1) & 0x7F; // page mode: wraps within the page
        revision++;
    }

    void OledWrite(const uint8_t* const data, const uint8_t len)
    {
        // Control byte: bit 6 D/C# (0 command, 1 data), bit 7 Co (1: only one
        // byte follows before the next control byte).
        uint8_t i = 0;
        while (i < len)
        {
            const uint8_t control = data[i++];
            const bool    isData  = (control & 0x40) != 0;
            const bool    single  = (control & 0x80) != 0;
            while (i < len)
            {
                isData ? Data(data[i]) : Command(data[i]);
                i++;
                if (single)
                {
                    break;
                }
            }
        }
    }

    // --- CHT40 ------------------------------------------------------------
    constexpr uint8_t MeasureHighRepeatability = 0xFD; // datasheet §5.4 (Cht40's own enum is private)

    int16_t  roomCentiDegC = 2150;
    uint16_t roomCentiRH   = 4500;
    bool     sensorPresent = true;
    uint8_t  sensorFrame[6];
    bool     measurementReady = false;

    void LatchMeasurement()
    {
        // Inverse of Cht40::Measure()'s conversion (datasheet §5.5).
        int32_t rawT  = ((static_cast<int32_t>(roomCentiDegC) + 4500) * 65535 + 17499) / 17500;
        int32_t rawRH = ((static_cast<int32_t>(roomCentiRH) + 600) * 65535 + 12499) / 12500;
        rawT          = rawT < 0 ? 0 : (rawT > 65535 ? 65535 : rawT);
        rawRH         = rawRH < 0 ? 0 : (rawRH > 65535 ? 65535 : rawRH);

        sensorFrame[0]   = static_cast<uint8_t>(rawT >> 8);
        sensorFrame[1]   = static_cast<uint8_t>(rawT);
        sensorFrame[2]   = Cht40::Crc8(&sensorFrame[0], 2);
        sensorFrame[3]   = static_cast<uint8_t>(rawRH >> 8);
        sensorFrame[4]   = static_cast<uint8_t>(rawRH);
        sensorFrame[5]   = Cht40::Crc8(&sensorFrame[3], 2);
        measurementReady = true;
    }
} // namespace

namespace SimI2c
{
    void SetRoom(const int16_t centiDegC, const uint16_t centiRH)
    {
        roomCentiDegC = centiDegC;
        roomCentiRH   = centiRH;
    }

    void SetSensorPresent(const bool present)
    {
        sensorPresent = present;
    }

    bool PanelOn()
    {
        return displayOn;
    }

    bool Pixel(const int x, const int y)
    {
        if (!displayOn || x < 0 || x >= Ssd1306::Width || y < 0 || y >= Ssd1306::Height)
        {
            return false;
        }
        // Ssd1306's init (0xA1 + 0xC8) is the "right way up" mount, so that
        // combination maps GDDRAM straight to the screen; any other setting
        // shows the panel mirrored the way the glass would.
        const int  col = segRemap ? x : Ssd1306::Width - 1 - x;
        const int  row = comRemap ? y : Ssd1306::Height - 1 - y;
        const bool lit = (gddram[row / 8][col] >> (row % 8)) & 1u;
        return lit != inverse;
    }

    uint8_t Contrast()
    {
        return contrast;
    }

    uint32_t Revision()
    {
        return revision;
    }
} // namespace SimI2c

I2c::I2c(const I2cPins& pins) :
    pins(pins)
{
}

void I2c::Init()
{
}

bool I2c::Write(const uint8_t address7, const uint8_t* const data, const uint8_t len)
{
    if (address7 == Ssd1306::DefaultAddress)
    {
        OledWrite(data, len);
        return true;
    }
    if (address7 == Cht40::DefaultAddress && sensorPresent)
    {
        if (len >= 1 && data[0] == MeasureHighRepeatability)
        {
            LatchMeasurement();
        }
        return true;
    }
    return false; // nobody home: NACK
}

bool I2c::Read(const uint8_t address7, uint8_t* const data, const uint8_t len)
{
    if (address7 != Cht40::DefaultAddress || !sensorPresent || !measurementReady)
    {
        return false;
    }
    for (uint8_t i = 0; i < len; i++)
    {
        data[i] = i < sizeof(sensorFrame) ? sensorFrame[i] : 0xFFu;
    }
    measurementReady = false;
    return true;
}
