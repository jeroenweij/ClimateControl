/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include <string.h>

#include "FakeClock.h"
#include "LogRing.h"
#include "Logger.h"
#include "Test.h"

using Tools::LogRing::Lines;
using Tools::LogRing::LineSize;

namespace
{
    // One Pop() into a terminated string; false when the ring gave nothing.
    bool NextLine(char* const out)
    {
        uint8_t      buf[LineSize + 8];
        uint32_t     at;
        const size_t n = Tools::LogRing::Pop(buf, sizeof(buf), at);
        memcpy(out, buf, n);
        out[n] = '\0';
        return n != 0;
    }
} // namespace

CC_TEST(LogRing, ReadsBackWhatWasPushedOldestFirst)
{
    Tools::LogRing::Clear();
    Tools::LogRing::Push('I', "first");
    Tools::LogRing::Push('W', "second");
    CC_CHECK_EQ(Tools::LogRing::Buffered(), 2);

    char line[LineSize + 1];
    CC_CHECK(NextLine(line));
    CC_CHECK(strcmp(line, "I: first") == 0);
    CC_CHECK(NextLine(line));
    CC_CHECK(strcmp(line, "W: second") == 0);
    CC_CHECK(!NextLine(line)); // drained
    CC_CHECK_EQ(Tools::LogRing::Buffered(), 0);
}

CC_TEST(LogRing, EmptyRingReturnsNothing)
{
    Tools::LogRing::Clear();
    uint8_t  buf[LineSize];
    uint32_t at;
    CC_CHECK_EQ(Tools::LogRing::Pop(buf, sizeof(buf), at), 0);
}

CC_TEST(LogRing, LinesAreTruncatedToOneBusMessage)
{
    Tools::LogRing::Clear();
    Tools::LogRing::Push('E', "0123456789012345678901234567890123456789012345678901234567890123456789");

    char line[LineSize + 8];
    CC_CHECK(NextLine(line));
    CC_CHECK_EQ(strlen(line), LineSize);
    CC_CHECK_EQ(LineSize, 32);
    CC_CHECK(strncmp(line, "E: 0123456789", 13) == 0);
    CC_CHECK(!NextLine(line)); // one message per line, no continuation
}

CC_TEST(LogRing, PopRespectsTheCallersBuffer)
{
    Tools::LogRing::Clear();
    Tools::LogRing::Push('I', "hello world");
    uint8_t      buf[4];
    uint32_t     at;
    const size_t n = Tools::LogRing::Pop(buf, sizeof(buf), at);
    CC_CHECK_EQ(n, 4);
    CC_CHECK(memcmp(buf, "I: h", 4) == 0);
}

CC_TEST(LogRing, OverflowOverwritesTheOldestAndReportsHowManyWereLost)
{
    Tools::LogRing::Clear();
    char msg[8];
    for (int i = 0; i < Lines + 2; i++)
    {
        msg[0] = static_cast<char>('a' + i);
        msg[1] = '\0';
        Tools::LogRing::Push('I', msg);
    }
    CC_CHECK_EQ(Tools::LogRing::Buffered(), Lines);

    char line[LineSize + 8];
    CC_CHECK(NextLine(line));
    CC_CHECK(strcmp(line, "~ 2 lost") == 0);
    CC_CHECK(NextLine(line));
    CC_CHECK(strcmp(line, "I: c") == 0); // 'a' and 'b' were overwritten
    for (int i = 1; i < Lines; i++)
    {
        CC_CHECK(NextLine(line));
    }
    const char last[] = {'I', ':', ' ', static_cast<char>('a' + Lines + 1), '\0'};
    CC_CHECK(strcmp(line, last) == 0); // the newest line survived
    CC_CHECK(!NextLine(line));

    // The marker is only reported once.
    Tools::LogRing::Push('I', "x");
    CC_CHECK(NextLine(line));
    CC_CHECK(strcmp(line, "I: x") == 0);
}

CC_TEST(LogRing, LostMarkerIsNotCountedAsBuffered)
{
    Tools::LogRing::Clear();
    for (int i = 0; i < Lines + 1; i++)
    {
        Tools::LogRing::Push('I', "z");
    }
    CC_CHECK_EQ(Tools::LogRing::Buffered(), Lines);
}

CC_TEST(LogRing, LogMacrosFeedTheRing)
{
    Tools::LogRing::Clear();
    LOG_INFO("hello " << 42);
    LOG_WARN("careful");
    LOG_ERROR("bad");

    char line[LineSize + 8];
    CC_CHECK(NextLine(line));
    CC_CHECK(strcmp(line, "I: hello 42") == 0);
    CC_CHECK(NextLine(line));
    CC_CHECK(strcmp(line, "W: careful") == 0);
    CC_CHECK(NextLine(line));
    CC_CHECK(strcmp(line, "E: bad") == 0);
}

CC_TEST(LogRing, LinesCarryTheUptimeTheyWereLoggedAtAndAnEmptyPopReportsNow)
{
    FakeClock::Reset();
    Tools::LogRing::Clear();

    FakeClock::Advance(3000);
    Tools::LogRing::Push('I', "early"); // 3 s
    FakeClock::Advance(12000);
    Tools::LogRing::Push('I', "later"); // 15 s
    FakeClock::Advance(20500); // now 35.5 s

    uint8_t  buf[LineSize];
    uint32_t at = 99;
    CC_CHECK_EQ(Tools::LogRing::Pop(buf, sizeof(buf), at), 8); // "I: early"
    CC_CHECK_EQ(at, 3);
    CC_CHECK_EQ(Tools::LogRing::Pop(buf, sizeof(buf), at), 8); // "I: later"
    CC_CHECK_EQ(at, 15);
    CC_CHECK_EQ(Tools::LogRing::Pop(buf, sizeof(buf), at), 0); // drained: 'at' is now
    CC_CHECK_EQ(at, 35);
}

CC_TEST(LogRing, LogDebugReachesTheRingOnlyInDebugBuilds)
{
    Tools::LogRing::Clear();
    LOG_DEBUG("dbg " << 1);
#ifdef DEBUG
    char line[LineSize + 8];
    CC_CHECK(NextLine(line));
    CC_CHECK(strcmp(line, "D: dbg 1") == 0);
#else
    CC_CHECK_EQ(Tools::LogRing::Buffered(), 0); // compiled out entirely
#endif
}
