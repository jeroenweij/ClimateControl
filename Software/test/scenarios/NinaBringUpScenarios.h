/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

// Bring-up / link-supervision scenarios shared by the application's
// UplinkHandler and MainBootloader's (two forks of the same driver). The
// including test defines a 'Harness' with:
//
//   void Start();              fresh world, driver Init()'d
//   void Step(uint32_t ms);    advance time, run the driver once, pump the module
//   bool InDataMode() const;   the driver believes the uplink is up
//   ScriptedNina nina;         the module
//
// Each scenario is something a real module/network can do to the driver.

#include "EEndpoint.h"
#include "EOperation.h"
#include "Id.h"

#include "FakeClock.h"
#include "ScriptedNina.h"
#include "Test.h"
#include "UplinkFrames.h"

namespace scenario
{
    using NodeLib::Endpoint;
    using NodeLib::Id;
    using NodeLib::Message;
    using NodeLib::Operation;

    constexpr uint32_t StepMs = 10;

    // The very first low edge is the driver's own initial reset, not a failure.
    template <typename Harness>
    int FailureResets(const Harness& h)
    {
        return h.nina.Resets() - 1;
    }

    template <typename Harness>
    bool RunUntilDataMode(Harness& h, const uint32_t limitMs)
    {
        for (uint32_t t = 0; t < limitMs; t += StepMs)
        {
            h.Step(StepMs);
            if (h.InDataMode())
            {
                return true;
            }
        }
        return false;
    }

    // The server end of the socket: answers a keepalive Get with a Report, as
    // the real server does. Returns the frames the MCU sent.
    template <typename Harness>
    int ServerPump(Harness& h, Message* const seen, const int maxSeen, const bool answerKeepalive)
    {
        uint8_t      bytes[512];
        const size_t n = h.nina.TakeData(bytes, sizeof(bytes));
        if (n == 0)
        {
            return 0;
        }
        const int count = uplinkframes::Decode(bytes, n, seen, maxSeen);
        for (int i = 0; i < count; i++)
        {
            if (answerKeepalive && seen[i].id.endpoint == Endpoint::Keepalive && seen[i].id.operation == Operation::Get)
            {
                uint8_t      reply[64];
                const size_t len = uplinkframes::Encode(Message(Id(0, Endpoint::Keepalive, Operation::Report)), reply);
                h.nina.ServerSend(reply, len);
            }
        }
        return count;
    }

    template <typename Harness>
    void RunFor(Harness& h, const uint32_t ms, const bool answerKeepalive)
    {
        Message seen[16];
        for (uint32_t t = 0; t < ms; t += StepMs)
        {
            h.Step(StepMs);
            ServerPump(h, seen, 16, answerKeepalive);
        }
    }
} // namespace scenario

#define CC_NINA_SCENARIOS(Harness)                                                                                                                                                                                         \
    CC_TEST(NinaBringUp, HappyPathReachesDataModeAndSendsTheHelloFirst)                                                                                                                                                    \
    {                                                                                                                                                                                                                      \
        Harness h;                                                                                                                                                                                                         \
        h.Start();                                                                                                                                                                                                         \
        CC_CHECK(scenario::RunUntilDataMode(h, 10000));                                                                                                                                                                    \
        CC_CHECK_EQ(scenario::FailureResets(h), 0);                                                                                                                                                                        \
                                                                                                                                                                                                                           \
        scenario::Message seen[8];                                                                                                                                                                                         \
        int               n = 0;                                                                                                                                                                                           \
        for (int i = 0; i < 20 && n == 0; i++)                                                                                                                                                                             \
        {                                                                                                                                                                                                                  \
            h.Step(scenario::StepMs);                                                                                                                                                                                      \
            n = scenario::ServerPump(h, seen, 8, true);                                                                                                                                                                    \
        }                                                                                                                                                                                                                  \
        CC_CHECK(n >= 1);                                                                                                                                                                                                  \
        CC_CHECK(seen[0].id.endpoint == scenario::Endpoint::UplinkHello);                                                                                                                                                  \
        CC_CHECK(seen[0].id.operation == scenario::Operation::Report);                                                                                                                                                     \
        CC_CHECK_EQ(seen[0].len, 23);                                                                                                                                                                                      \
    }                                                                                                                                                                                                                      \
                                                                                                                                                                                                                           \
    CC_TEST(NinaBringUp, ASlowBootingModuleIsProbedUntilItAnswers)                                                                                                                                                         \
    {                                                                                                                                                                                                                      \
        Harness h;                                                                                                                                                                                                         \
        h.Start();                                                                                                                                                                                                         \
        h.nina.bootDelayMs = 4000; /* silent well past one probe timeout */                                                                                                                                                \
        CC_CHECK(scenario::RunUntilDataMode(h, 30000));                                                                                                                                                                    \
        CC_CHECK_EQ(scenario::FailureResets(h), 0); /* inside the probe budget: no reset */                                                                                                                                \
    }                                                                                                                                                                                                                      \
                                                                                                                                                                                                                           \
    CC_TEST(NinaBringUp, AModuleThatBootsButNeverAnswersAtIsResetAgain)                                                                                                                                                    \
    {                                                                                                                                                                                                                      \
        Harness h;                                                                                                                                                                                                         \
        h.Start();                                                                                                                                                                                                         \
        /* Seen on the bench: +STARTUP, then silence to every "AT". The first */                                                                                                                                           \
        /* 10 probes go unanswered (the whole budget), two more after the */                                                                                                                                               \
        /* reset, then it answers. */                                                                                                                                                                                      \
        h.nina.AddFirst({"AT", true, "", 0, "", 0, 12, false});                                                                                                                                                            \
        CC_CHECK(scenario::RunUntilDataMode(h, 60000));                                                                                                                                                                    \
        CC_CHECK_EQ(scenario::FailureResets(h), 1);                                                                                                                                                                        \
    }                                                                                                                                                                                                                      \
                                                                                                                                                                                                                           \
    CC_TEST(NinaBringUp, ASingleConfigErrorIsRetriedWithoutResettingTheModule)                                                                                                                                             \
    {                                                                                                                                                                                                                      \
        Harness h;                                                                                                                                                                                                         \
        h.Start();                                                                                                                                                                                                         \
        h.nina.AddFirst({"AT+UWSC=0,2", false, "ERROR\r\n", 10, "", 0, 1, false});                                                                                                                                         \
        CC_CHECK(scenario::RunUntilDataMode(h, 15000));                                                                                                                                                                    \
        CC_CHECK_EQ(scenario::FailureResets(h), 0);                                                                                                                                                                        \
    }                                                                                                                                                                                                                      \
                                                                                                                                                                                                                           \
    CC_TEST(NinaBringUp, RepeatedConfigErrorsResetTheModuleThenRecover)                                                                                                                                                    \
    {                                                                                                                                                                                                                      \
        Harness h;                                                                                                                                                                                                         \
        h.Start();                                                                                                                                                                                                         \
        h.nina.AddFirst({"AT+UWSC=0,2", false, "ERROR\r\n", 10, "", 0, 3, false}); /* the whole retry budget */                                                                                                            \
        CC_CHECK(scenario::RunUntilDataMode(h, 30000));                                                                                                                                                                    \
        CC_CHECK_EQ(scenario::FailureResets(h), 1);                                                                                                                                                                        \
    }                                                                                                                                                                                                                      \
                                                                                                                                                                                                                           \
    CC_TEST(NinaBringUp, NoNetworkWithinTheWaitResetsTheModuleThenRecovers)                                                                                                                                                \
    {                                                                                                                                                                                                                      \
        Harness h;                                                                                                                                                                                                         \
        h.Start();                                                                                                                                                                                                         \
        h.nina.AddFirst({"AT+UWSCA=", false, "OK\r\n", 20, "", 0, 1, false}); /* activates, never gets an IP */                                                                                                            \
        CC_CHECK(scenario::RunUntilDataMode(h, 90000));                                                                                                                                                                    \
        CC_CHECK_EQ(scenario::FailureResets(h), 1);                                                                                                                                                                        \
    }                                                                                                                                                                                                                      \
                                                                                                                                                                                                                           \
    CC_TEST(NinaBringUp, ANetworkFlapAfterTheIpIsUpDoesNotFailTheAttempt)                                                                                                                                                  \
    {                                                                                                                                                                                                                      \
        /* The module reports IP up, then down, then up again in a burst (the */                                                                                                                                           \
        /* settle glitch seen on the bench). The link is fine at the end. */                                                                                                                                               \
        Harness h;                                                                                                                                                                                                         \
        h.Start();                                                                                                                                                                                                         \
        h.nina.AddFirst({"AT+UWSCA=", false, "OK\r\n", 20, "+UUWLE:0,001122334455,6\r\n+UUNU:0,192.168.2.50,255.255.255.0,192.168.2.1,192.168.2.1\r\n"                                                                     \
                                                           "+UUND:0\r\n+UUNU:0,192.168.2.50,255.255.255.0,192.168.2.1,192.168.2.1\r\n",                                                                                    \
                         100,                                                                                                                                                                                              \
                         1,                                                                                                                                                                                                \
                         false});                                                                                                                                                                                          \
        CC_CHECK(scenario::RunUntilDataMode(h, 15000));                                                                                                                                                                    \
        CC_CHECK_EQ(scenario::FailureResets(h), 0);                                                                                                                                                                        \
    }                                                                                                                                                                                                                      \
                                                                                                                                                                                                                           \
    CC_TEST(NinaBringUp, APeerConnectErrorIsRetriedAndEventuallyResets)                                                                                                                                                    \
    {                                                                                                                                                                                                                      \
        Harness h;                                                                                                                                                                                                         \
        h.Start();                                                                                                                                                                                                         \
        h.nina.AddFirst({"AT+UDCP=", false, "ERROR\r\n", 20, "", 0, 3, false});                                                                                                                                            \
        CC_CHECK(scenario::RunUntilDataMode(h, 40000));                                                                                                                                                                    \
        CC_CHECK_EQ(scenario::FailureResets(h), 1);                                                                                                                                                                        \
    }                                                                                                                                                                                                                      \
                                                                                                                                                                                                                           \
    CC_TEST(NinaBringUp, ALateSecondNetworkUpDoesNotCostAModuleRestart)                                                                                                                                                    \
    {                                                                                                                                                                                                                      \
        /* Bench: AT+UDCP answers ERROR until a second +UUNU, up to ~0.9 s after the first. */                                                                                                                             \
        Harness h;                                                                                                                                                                                                         \
        h.Start();                                                                                                                                                                                                         \
        h.nina.secondNetworkUpMs = 900;                                                                                                                                                                                    \
        CC_CHECK(scenario::RunUntilDataMode(h, 20000));                                                                                                                                                                    \
        CC_CHECK_EQ(scenario::FailureResets(h), 0);                                                                                                                                                                        \
        CC_CHECK_EQ(h.nina.UdcpCount(), 1); /* not even one wasted attempt */                                                                                                                                              \
    }                                                                                                                                                                                                                      \
                                                                                                                                                                                                                           \
    CC_TEST(NinaBringUp, ANetworkUpFromBeforeTheModuleResetIsNotTakenForTheNewConnection)                                                                                                                                  \
    {                                                                                                                                                                                                                      \
        /* The module still gets a +UUNU out after the reset line is pulled. Read after the reset, it must not be mistaken for the new life's IP-up: */                                                                    \
        /* the retry has to wait for the real one, not spend attempts (and a further restart) on a network that is not there yet. */                                                                                       \
        Harness h;                                                                                                                                                                                                         \
        h.Start();                                                                                                                                                                                                         \
        h.nina.resetLagMs        = 200;                                                                                                                                                                                    \
        h.nina.secondNetworkUpMs = 1000;                                                                                                                                                                                   \
        h.nina.AddFirst({"AT+UWSCA=", false, "OK\r\n", 20, "+UUWLE:0,001122334455,6\r\n+UUNU:0,192.168.2.50,255.255.255.0,192.168.2.1,192.168.2.1\r\n", 3000, -1, false});                                                 \
        h.nina.AddFirst({"AT+UDCP=", false, "ERROR\r\n", 20, "+UUNU:0,192.168.2.50,255.255.255.0,192.168.2.1,192.168.2.1\r\n", 60, 3, false});                                                                             \
        CC_CHECK(scenario::RunUntilDataMode(h, 60000));                                                                                                                                                                    \
        CC_CHECK_EQ(scenario::FailureResets(h), 1);                                                                                                                                                                        \
        CC_CHECK_EQ(h.nina.UdcpCount(), 4); /* three that fail, one that connects */                                                                                                                                       \
    }                                                                                                                                                                                                                      \
                                                                                                                                                                                                                           \
    CC_TEST(NinaBringUp, AServerThatRefusesTheConnectionResetsTheModuleThenRecovers)                                                                                                                                       \
    {                                                                                                                                                                                                                      \
        Harness h;                                                                                                                                                                                                         \
        h.Start();                                                                                                                                                                                                         \
        h.nina.AddFirst({"AT+UDCP=", false, "+UDCP:0\r\nOK\r\n", 20, "+UUDPD:0\r\n", 80, 1, false});                                                                                                                       \
        CC_CHECK(scenario::RunUntilDataMode(h, 40000));                                                                                                                                                                    \
        CC_CHECK_EQ(scenario::FailureResets(h), 1);                                                                                                                                                                        \
    }                                                                                                                                                                                                                      \
                                                                                                                                                                                                                           \
    CC_TEST(NinaBringUp, ARespondingServerKeepsTheLinkUpIndefinitely)                                                                                                                                                      \
    {                                                                                                                                                                                                                      \
        Harness h;                                                                                                                                                                                                         \
        h.Start();                                                                                                                                                                                                         \
        CC_CHECK(scenario::RunUntilDataMode(h, 10000));                                                                                                                                                                    \
        scenario::RunFor(h, 5 * 60 * 1000, true); /* five minutes of keepalives */                                                                                                                                         \
        CC_CHECK(h.InDataMode());                                                                                                                                                                                          \
        CC_CHECK_EQ(scenario::FailureResets(h), 0);                                                                                                                                                                        \
    }                                                                                                                                                                                                                      \
                                                                                                                                                                                                                           \
    CC_TEST(NinaBringUp, ASilentServerTripsTheWatchdogAndTheLinkIsRebuilt)                                                                                                                                                 \
    {                                                                                                                                                                                                                      \
        Harness h;                                                                                                                                                                                                         \
        h.Start();                                                                                                                                                                                                         \
        CC_CHECK(scenario::RunUntilDataMode(h, 10000));                                                                                                                                                                    \
        scenario::RunFor(h, 2 * 60 * 1000, false); /* nothing ever comes back */                                                                                                                                           \
        CC_CHECK(scenario::FailureResets(h) >= 1);                                                                                                                                                                         \
        CC_CHECK(scenario::RunUntilDataMode(h, 60000)); /* and it comes back up */                                                                                                                                         \
    }                                                                                                                                                                                                                      \
                                                                                                                                                                                                                           \
    CC_TEST(NinaBringUp, APeerDropWithASilentModuleIsCaughtByTheWatchdog)                                                                                                                                                  \
    {                                                                                                                                                                                                                      \
        Harness h;                                                                                                                                                                                                         \
        h.Start();                                                                                                                                                                                                         \
        CC_CHECK(scenario::RunUntilDataMode(h, 10000));                                                                                                                                                                    \
        h.nina.DropPeer(0);                                                                                                                                                                                                \
        scenario::RunFor(h, 2 * 60 * 1000, true);                                                                                                                                                                          \
        CC_CHECK(scenario::FailureResets(h) >= 1);                                                                                                                                                                         \
    }                                                                                                                                                                                                                      \
                                                                                                                                                                                                                           \
    CC_TEST(NinaBringUp, APeerDropWhereTheModuleAnswersEveryStrayWriteWithErrorIsStillCaught)                                                                                                                              \
    { /* Worst case: after the peer is gone the module is back in command */ /* mode and replies ERROR to whatever the MCU keeps writing -- those */ /* bytes are not a live server and must not keep the link "alive". */ \
        Harness h;                                                                                                                                                                                                         \
        h.Start();                                                                                                                                                                                                         \
        CC_CHECK(scenario::RunUntilDataMode(h, 10000));                                                                                                                                                                    \
        h.nina.strayErrors = true;                                                                                                                                                                                         \
        h.nina.DropPeer(0);                                                                                                                                                                                                \
        scenario::RunFor(h, 3 * 60 * 1000, true);                                                                                                                                                                          \
        CC_CHECK(scenario::FailureResets(h) >= 1);                                                                                                                                                                         \
    }                                                                                                                                                                                                                      \
                                                                                                                                                                                                                           \
    CC_TEST(NinaBringUp, APeerDropIsDetectedWithinTheKeepaliveDeadline)                                                                                                                                                    \
    { /* keepalive interval (10 s) + reply deadline (10 s), with margin -- not the 60 s watchdog */                                                                                                                        \
        Harness h;                                                                                                                                                                                                         \
        h.Start();                                                                                                                                                                                                         \
        CC_CHECK(scenario::RunUntilDataMode(h, 10000));                                                                                                                                                                    \
        h.nina.DropPeer(0);                                                                                                                                                                                                \
        scenario::RunFor(h, 25000, true);                                                                                                                                                                                  \
        CC_CHECK(scenario::FailureResets(h) >= 1);                                                                                                                                                                         \
    }                                                                                                                                                                                                                      \
                                                                                                                                                                                                                           \
    CC_TEST(NinaBringUp, ADropWhereTheModuleAnswersStrayWritesWithErrorIsDetectedInTheSameTime)                                                                                                                            \
    {                                                                                                                                                                                                                      \
        Harness h;                                                                                                                                                                                                         \
        h.Start();                                                                                                                                                                                                         \
        CC_CHECK(scenario::RunUntilDataMode(h, 10000));                                                                                                                                                                    \
        h.nina.strayErrors = true;                                                                                                                                                                                         \
        h.nina.DropPeer(0);                                                                                                                                                                                                \
        scenario::RunFor(h, 25000, true);                                                                                                                                                                                  \
        CC_CHECK(scenario::FailureResets(h) >= 1);                                                                                                                                                                         \
    }                                                                                                                                                                                                                      \
                                                                                                                                                                                                                           \
    CC_TEST(NinaBringUp, AServerThatStopsAnsweringIsDetectedWithinTheKeepaliveDeadline)                                                                                                                                    \
    {                                                                                                                                                                                                                      \
        Harness h;                                                                                                                                                                                                         \
        h.Start();                                                                                                                                                                                                         \
        CC_CHECK(scenario::RunUntilDataMode(h, 10000));                                                                                                                                                                    \
        scenario::RunFor(h, 15000, true); /* healthy for a while ... */                                                                                                                                                    \
        CC_CHECK_EQ(scenario::FailureResets(h), 0);                                                                                                                                                                        \
        scenario::RunFor(h, 25000, false); /* ... then it never answers again */                                                                                                                                           \
        CC_CHECK(scenario::FailureResets(h) >= 1);                                                                                                                                                                         \
    }                                                                                                                                                                                                                      \
                                                                                                                                                                                                                           \
    CC_TEST(NinaBringUp, EverySessionOpensWithTheHelloEvenAfterAKeepaliveDeadlineReset)                                                                                                                                    \
    {                                                                                                                                                                                                                      \
        /* The server rejects a connection whose first frame is not the hello (bench: a stale keepalive left in the send queue by the dead session). */                                                                    \
        Harness h;                                                                                                                                                                                                         \
        h.Start();                                                                                                                                                                                                         \
        CC_CHECK(scenario::RunUntilDataMode(h, 10000));                                                                                                                                                                    \
        scenario::Message seen[8];                                                                                                                                                                                         \
        bool              gotFirst = false;                                                                                                                                                                                \
        scenario::Message first;                                                                                                                                                                                           \
        for (uint32_t t = 0; t < 120 * 1000 && !gotFirst; t += scenario::StepMs)                                                                                                                                           \
        {                                                                                                                                                                                                                  \
            h.Step(scenario::StepMs);                                                                                                                                                                                      \
            const int n = scenario::ServerPump(h, seen, 8, false); /* the server stops answering */                                                                                                                        \
            if (n > 0 && scenario::FailureResets(h) >= 1 && h.InDataMode())                                                                                                                                                \
            {                                                                                                                                                                                                              \
                first    = seen[0];                                                                                                                                                                                        \
                gotFirst = true;                                                                                                                                                                                           \
            }                                                                                                                                                                                                              \
        }                                                                                                                                                                                                                  \
        CC_CHECK(gotFirst);                                                                                                                                                                                                \
        CC_CHECK(first.id.endpoint == scenario::Endpoint::UplinkHello);                                                                                                                                                    \
    }                                                                                                                                                                                                                      \
                                                                                                                                                                                                                           \
    CC_TEST(NinaBringUp, AServerThatKeepsSendingButNeverAnswersKeepalivesIsDetected)                                                                                                                                       \
    {                                                                                                                                                                                                                      \
        /* One-way stall (bench): the module still delivers the server's frames but forwards nothing of ours, so the server never sees a keepalive. */                                                                     \
        /* Its other traffic must not pass for the answer, or the MC never resets the module and the link stays deaf for as long as the server talks. */                                                                   \
        Harness h;                                                                                                                                                                                                         \
        h.Start();                                                                                                                                                                                                         \
        CC_CHECK(scenario::RunUntilDataMode(h, 10000));                                                                                                                                                                    \
        scenario::Message seen[8];                                                                                                                                                                                         \
        uint8_t           filler[64];                                                                                                                                                                                      \
        const size_t      fillerLen = uplinkframes::Encode(scenario::Message(scenario::Id(0, static_cast<scenario::Endpoint>(0x6E), scenario::Operation::Report)), filler);                                                \
        for (uint32_t t = 0; t < 60 * 1000 && scenario::FailureResets(h) < 1; t += scenario::StepMs)                                                                                                                       \
        {                                                                                                                                                                                                                  \
            h.Step(scenario::StepMs);                                                                                                                                                                                      \
            scenario::ServerPump(h, seen, 8, false); /* keepalives go unanswered */                                                                                                                                        \
            if (t % 500 == 0)                                                                                                                                                                                              \
            {                                                                                                                                                                                                              \
                h.nina.ServerSend(filler, fillerLen);                                                                                                                                                                      \
            }                                                                                                                                                                                                              \
        }                                                                                                                                                                                                                  \
        CC_CHECK(scenario::FailureResets(h) >= 1);                                                                                                                                                                         \
    }                                                                                                                                                                                                                      \
                                                                                                                                                                                                                           \
    CC_TEST(NinaBringUp, AnAnsweredKeepaliveNeverTripsTheDeadline)                                                                                                                                                         \
    {                                                                                                                                                                                                                      \
        Harness h;                                                                                                                                                                                                         \
        h.Start();                                                                                                                                                                                                         \
        CC_CHECK(scenario::RunUntilDataMode(h, 10000));                                                                                                                                                                    \
        scenario::RunFor(h, 10 * 60 * 1000, true); /* ten minutes */                                                                                                                                                       \
        CC_CHECK_EQ(scenario::FailureResets(h), 0);                                                                                                                                                                        \
    }
