/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "Cht40.h"

#include "FakeI2c.h"
#include "Test.h"

namespace
{
    // CRC(0xBEEF) == 0x92 -- the datasheet's own worked example (§5.3).
    CC_TEST(Cht40, Crc8MatchesTheDatasheetsWorkedExample)
    {
        const uint8_t data[2] = {0xBE, 0xEF};
        CC_CHECK_EQ(Cht40::Crc8(data, 2), 0x92);
    }

    CC_TEST(Cht40, MeasureDecodesAKnownRawReading)
    {
        FakeI2c::ResetAll();
        Hal::I2c bus({});
        Cht40    sensor(bus);

        // rawT = 30000 (0x7530) -> -45 + 175*30000/65535 = 35.10 C -> 3510 centi-degC.
        // rawRH = 20000 (0x4E20) -> -6 + 125*20000/65535 = 32.14 %RH -> 3214 centi-%RH.
        const uint8_t rawT[2]  = {0x75, 0x30};
        const uint8_t rawRH[2] = {0x4E, 0x20};
        const uint8_t frame[6] = {
            rawT[0], rawT[1], Cht40::Crc8(rawT, 2), rawRH[0], rawRH[1], Cht40::Crc8(rawRH, 2)};
        FakeI2c::QueueRead(Cht40::DefaultAddress, frame, sizeof(frame));

        int16_t  centiDegC;
        uint16_t centiRH;
        CC_CHECK(sensor.Measure(centiDegC, centiRH));
        CC_CHECK_EQ(centiDegC, 3510);
        CC_CHECK_EQ(centiRH, 3214);

        CC_CHECK_EQ(FakeI2c::WrittenLen(Cht40::DefaultAddress), 1);
        CC_CHECK_EQ(FakeI2c::Written(Cht40::DefaultAddress)[0], 0xFD);
    }

    CC_TEST(Cht40, MeasureFailsOnABadCrc)
    {
        FakeI2c::ResetAll();
        Hal::I2c bus({});
        Cht40    sensor(bus);

        const uint8_t frame[6] = {0x62, 0x37, 0x00, 0x50, 0xC1, 0x00}; // wrong CRC bytes
        FakeI2c::QueueRead(Cht40::DefaultAddress, frame, sizeof(frame));

        int16_t  centiDegC;
        uint16_t centiRH;
        CC_CHECK(!sensor.Measure(centiDegC, centiRH));
    }

    CC_TEST(Cht40, MeasureFailsOnNack)
    {
        FakeI2c::ResetAll();
        Hal::I2c bus({});
        Cht40    sensor(bus);
        FakeI2c::SetNack(Cht40::DefaultAddress, true); // e.g. conversion not done yet (datasheet §5.1)

        int16_t  centiDegC;
        uint16_t centiRH;
        CC_CHECK(!sensor.Measure(centiDegC, centiRH));
    }
} // namespace
