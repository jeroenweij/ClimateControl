/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "NinaLineParser.h"
#include "Test.h"

namespace
{
    // Feeds a whole string (as received over the wire, with real \r\n line
    // endings) and returns the LineResult of the *last* completed line.
    NinaLineParser::LineResult FeedAll(NinaLineParser& parser, const char* const text)
    {
        NinaLineParser::LineResult result = NinaLineParser::LineResult::None;
        for (const char* p = text; *p != '\0'; p++)
        {
            const NinaLineParser::LineResult lineResult = parser.FeedByte(static_cast<uint8_t>(*p));
            if (lineResult != NinaLineParser::LineResult::None)
            {
                result = lineResult;
            }
        }
        return result;
    }
} // namespace

CC_TEST(NinaLineParser, PlainOkCompletesAsOk)
{
    NinaLineParser parser;
    const auto     result = FeedAll(parser, "AT\r\n\r\nOK\r\n");
    CC_CHECK(result == NinaLineParser::LineResult::Ok);
}

CC_TEST(NinaLineParser, ErrorLineCompletesAsError)
{
    NinaLineParser parser;
    const auto     result = FeedAll(parser, "AT+BOGUS\r\n\r\nERROR\r\n");
    CC_CHECK(result == NinaLineParser::LineResult::Error);
}

CC_TEST(NinaLineParser, UdcpLineCapturesPeerHandleThenOk)
{
    NinaLineParser parser;
    CC_CHECK(parser.LastPeerHandle() == -1);

    const auto result = FeedAll(parser, "AT+UDCP=\"tcp://10.18.9.7:9000/\"\r\n\r\n+UDCP:1\r\nOK\r\n");
    CC_CHECK(result == NinaLineParser::LineResult::Ok);
    CC_CHECK_EQ(parser.LastPeerHandle(), 1);
}

CC_TEST(NinaLineParser, MultiDigitPeerHandleParsedCorrectly)
{
    NinaLineParser parser;
    FeedAll(parser, "+UDCP:12\r\nOK\r\n");
    CC_CHECK_EQ(parser.LastPeerHandle(), 12);
}

CC_TEST(NinaLineParser, UnsolicitedEventsAreQueuedAndDrainedInOrder)
{
    NinaLineParser        parser;
    NinaLineParser::Event event;

    CC_CHECK(!parser.NextEvent(event));

    FeedAll(parser, "+UUWLE:0,3C46A168AF20,1\r\n");
    FeedAll(parser, "+UUNU:0\r\n");
    FeedAll(parser, "+UUDPC:1,2,0,10.18.8.7,58671,10.18.9.7,9000\r\n");

    CC_CHECK(parser.NextEvent(event));
    CC_CHECK(event == NinaLineParser::Event::LinkUp);
    CC_CHECK(parser.NextEvent(event));
    CC_CHECK(event == NinaLineParser::Event::NetworkUp);
    CC_CHECK(parser.NextEvent(event));
    CC_CHECK(event == NinaLineParser::Event::PeerConnected);
    CC_CHECK(!parser.NextEvent(event));
}

CC_TEST(NinaLineParser, DisconnectAndDownEventsClassified)
{
    NinaLineParser        parser;
    NinaLineParser::Event event;

    FeedAll(parser, "+UUDPD:1\r\n+UUWLD:0,1\r\n+UUND:0\r\n");

    CC_CHECK(parser.NextEvent(event));
    CC_CHECK(event == NinaLineParser::Event::PeerDisconnected);
    CC_CHECK(parser.NextEvent(event));
    CC_CHECK(event == NinaLineParser::Event::LinkDown);
    CC_CHECK(parser.NextEvent(event));
    CC_CHECK(event == NinaLineParser::Event::NetworkDown);
}

CC_TEST(NinaLineParser, CommandEchoAndQuotedValuesDoNotCompleteACommand)
{
    NinaLineParser parser;
    // AT+GMM's echo + quoted model name, with no OK yet.
    const auto result = FeedAll(parser, "AT+GMM\r\n\r\n\"NINA-W152\"\r\n");
    CC_CHECK(result == NinaLineParser::LineResult::None);
}

CC_TEST(NinaLineParser, ResetClearsBufferedLineAndEventQueue)
{
    NinaLineParser        parser;
    NinaLineParser::Event event;

    FeedAll(parser, "+UUWLE:0,3C46A168AF20,1\r\n");
    parser.Reset();

    CC_CHECK(!parser.NextEvent(event));

    // A line that was mid-accumulation before Reset() should not leak into
    // the next one fed after it.
    parser.Reset();
    for (const char* p = "AT\r\nOK\r\n"; *p != '\0'; p++)
    {
        parser.FeedByte(static_cast<uint8_t>(*p));
    }
}
