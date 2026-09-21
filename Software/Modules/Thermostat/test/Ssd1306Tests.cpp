/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "Ssd1306.h"

#include "FakeI2c.h"
#include "Test.h"

namespace
{
    constexpr int pageStride = 4 + 1 + Ssd1306::Width; // set-page cmd(4) + data control(1) + 128 data bytes

    // Decodes one framebuffer byte (page, column) out of a captured Flush()
    // write stream -- see Ssd1306::Flush()'s wire layout.
    uint8_t FlushedByte(const int page, const int col)
    {
        return FakeI2c::Written(Ssd1306::DefaultAddress)[page * pageStride + 4 + 1 + col];
    }

    int PopCount(const uint8_t b)
    {
        int     n = 0;
        uint8_t v = b;
        for (int i = 0; i < 8; i++)
        {
            n += (v & 0x1);
            v >>= 1;
        }
        return n;
    }

    CC_TEST(Ssd1306, InitSendsTheStandardBringUpSequenceAndTurnsOn)
    {
        FakeI2c::ResetAll();
        Hal::I2c bus({});
        Ssd1306  display(bus);

        display.Init();

        const uint8_t* const w   = FakeI2c::Written(Ssd1306::DefaultAddress);
        const size_t         len = FakeI2c::WrittenLen(Ssd1306::DefaultAddress);
        CC_CHECK(len > 2);
        CC_CHECK_EQ(w[0], 0x00); // command control byte
        CC_CHECK_EQ(w[1], 0xAE); // display off, first
        CC_CHECK_EQ(w[len - 2], 0x00);
        CC_CHECK_EQ(w[len - 1], 0xAF); // On() writes this last
    }

    CC_TEST(Ssd1306, ClearLeavesTheFramebufferBlank)
    {
        FakeI2c::ResetAll();
        Hal::I2c bus({});
        Ssd1306  display(bus);

        display.Clear();
        display.Flush();

        for (int page = 0; page < Ssd1306::Pages; page++)
        {
            for (int col = 0; col < Ssd1306::Width; col++)
            {
                CC_CHECK_EQ(FlushedByte(page, col), 0);
            }
        }
    }

    CC_TEST(Ssd1306, SetPixelTogglesExactlyOneBit)
    {
        FakeI2c::ResetAll();
        Hal::I2c bus({});
        Ssd1306  display(bus);

        display.Clear();
        display.SetPixel(5, 3, true); // page 0, bit 3
        display.Flush();

        CC_CHECK_EQ(FlushedByte(0, 5), 1u << 3);
        CC_CHECK_EQ(FlushedByte(0, 4), 0);
        CC_CHECK_EQ(FlushedByte(1, 5), 0);
    }

    CC_TEST(Ssd1306, SetPixelOnAPageBoundaryLandsInTheRightPage)
    {
        FakeI2c::ResetAll();
        Hal::I2c bus({});
        Ssd1306  display(bus);

        display.Clear();
        display.SetPixel(0, 8, true); // first row of page 1
        display.Flush();

        CC_CHECK_EQ(FlushedByte(0, 0), 0);
        CC_CHECK_EQ(FlushedByte(1, 0), 1u);
    }

    CC_TEST(Ssd1306, FillRectSetsTheWholeArea)
    {
        FakeI2c::ResetAll();
        Hal::I2c bus({});
        Ssd1306  display(bus);

        display.Clear();
        display.FillRect(0, 0, 3, 3, true);
        display.Flush();

        CC_CHECK_EQ(FlushedByte(0, 0), 0x07);
        CC_CHECK_EQ(FlushedByte(0, 1), 0x07);
        CC_CHECK_EQ(FlushedByte(0, 2), 0x07);
        CC_CHECK_EQ(FlushedByte(0, 3), 0x00); // just outside the rect
    }

    CC_TEST(Ssd1306, DrawRectLeavesTheInteriorClear)
    {
        FakeI2c::ResetAll();
        Hal::I2c bus({});
        Ssd1306  display(bus);

        display.Clear();
        display.DrawRect(0, 0, 5, 5, true);
        display.Flush();

        CC_CHECK_EQ(FlushedByte(0, 2) & (1u << 2), 0); // centre pixel, not on the border
        CC_CHECK(FlushedByte(0, 0) != 0); // left edge is on the border
        CC_CHECK(FlushedByte(0, 2) & 0x01); // top edge
        CC_CHECK(FlushedByte(0, 2) & (1u << 4)); // bottom edge
    }

    CC_TEST(Ssd1306, FillCircleSetsTheCentreDrawCircleDoesNot)
    {
        FakeI2c::ResetAll();
        Hal::I2c bus({});
        Ssd1306  display(bus);

        display.Clear();
        display.FillCircle(10, 10, 4, true);
        display.Flush();
        CC_CHECK(FlushedByte(1, 10) & (1u << 2)); // (10,10) -- page 1, bit (10%8)=2

        FakeI2c::ClearWritten(Ssd1306::DefaultAddress);
        display.Clear();
        display.DrawCircle(10, 10, 4, true);
        display.Flush();
        CC_CHECK_EQ(FlushedByte(1, 10) & (1u << 2), 0); // outline only -- centre stays clear
        CC_CHECK(FlushedByte(1, 6) != 0); // left-most point of the circle (cx - r)
    }

    CC_TEST(Ssd1306, DigitZeroLitsMoreSegmentsThanDigitOne)
    {
        FakeI2c::ResetAll();
        Hal::I2c bus({});
        Ssd1306  display(bus);

        display.Clear();
        display.DrawDigit(0, 0, 6, 10, 1, 0);
        display.Flush();
        int zeroBits = 0;
        for (int col = 0; col < 6; col++)
        {
            zeroBits += PopCount(FlushedByte(0, col)) + PopCount(FlushedByte(1, col));
        }

        FakeI2c::ClearWritten(Ssd1306::DefaultAddress);
        display.Clear();
        display.DrawDigit(0, 0, 6, 10, 1, 1);
        display.Flush();
        int oneBits = 0;
        for (int col = 0; col < 6; col++)
        {
            oneBits += PopCount(FlushedByte(0, col)) + PopCount(FlushedByte(1, col));
        }

        CC_CHECK(zeroBits > oneBits); // '0' lights 6 segments, '1' only 2
    }

    CC_TEST(Ssd1306, DrawNumberAddsPixelsForTheDecimalPointAndMinusSign)
    {
        FakeI2c::ResetAll();
        Hal::I2c bus({});
        Ssd1306  display(bus);

        auto countBits = [&]()
        {
            int total = 0;
            for (int page = 0; page < Ssd1306::Pages; page++)
            {
                for (int col = 0; col < Ssd1306::Width; col++)
                {
                    total += PopCount(FlushedByte(page, col));
                }
            }
            return total;
        };

        display.Clear();
        display.DrawNumber(0, 0, 6, 10, 1, 215, 0); // "215", no decimal point
        display.Flush();
        const int withoutDecimal = countBits();

        FakeI2c::ClearWritten(Ssd1306::DefaultAddress);
        display.Clear();
        display.DrawNumber(0, 0, 6, 10, 1, 215, 1); // "21.5"
        display.Flush();
        const int withDecimal = countBits();

        CC_CHECK(withDecimal > withoutDecimal); // the added decimal point lit at least one more pixel

        FakeI2c::ClearWritten(Ssd1306::DefaultAddress);
        display.Clear();
        display.DrawNumber(0, 0, 6, 10, 1, -215, 1); // "-21.5"
        display.Flush();
        const int withMinus = countBits();

        CC_CHECK(withMinus > withDecimal); // the added minus sign lit at least one more pixel
    }
} // namespace
