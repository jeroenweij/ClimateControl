/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include <string.h>

#include "Crc.h"

#include "Test.h"

using Hal::Crc;

namespace
{
    const uint8_t check[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
} // namespace

CC_TEST(Crc, Ccitt16KnownAnswer)
{
    Crc crc(Crc::Poly::Ccitt16);
    // CRC-16/CCITT-FALSE("123456789") == 0x29B1.
    CC_CHECK_EQ(crc.Compute(check, sizeof(check)), 0x29B1);
}

CC_TEST(Crc, Ccitt16EmptyIsInitValue)
{
    Crc crc(Crc::Poly::Ccitt16);
    CC_CHECK_EQ(crc.Compute(check, 0), 0xFFFF);
}

CC_TEST(Crc, Ccitt16IsResetBetweenCalls)
{
    Crc crc(Crc::Poly::Ccitt16);
    crc.Compute(check, sizeof(check));
    CC_CHECK_EQ(crc.Compute(check, sizeof(check)), 0x29B1);
}

CC_TEST(Crc, Crc32Mpeg2KnownAnswer)
{
    Crc crc(Crc::Poly::Ieee32);
    // CRC-32/MPEG-2("123456789") == 0x0376E6E7.
    CC_CHECK_EQ(crc.Compute32(check, sizeof(check)), 0x0376E6E7);
}

CC_TEST(Crc, Crc32EmptyIsInitValue)
{
    Crc crc(Crc::Poly::Ieee32);
    CC_CHECK_EQ(crc.Compute32(check, 0), 0xFFFFFFFF);
}
