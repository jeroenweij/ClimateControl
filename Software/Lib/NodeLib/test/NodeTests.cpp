/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include <string.h>

#include "EEndpoint.h"
#include "EFirmware.h"
#include "EOperation.h"
#include "LogRing.h"
#include "Node.h"

#include "BusHelpers.h"
#include "FakeBus.h"
#include "FakeClock.h"
#include "FakeConfigStore.h"
#include "Test.h"

using NodeLib::ConfigStore;
using NodeLib::Endpoint;
using NodeLib::FirmwareOp;
using NodeLib::Id;
using NodeLib::INodeHandler;
using NodeLib::Message;
using NodeLib::ModuleType;
using NodeLib::Node;
using NodeLib::Operation;
using NodeLib::SystemStatus;

// Node's own SLAVE-side dispatch: address/broadcast filtering, the
// NodeLib-owned System*/Firmware/Diagnostics* blocks, and Snoop() -- shared
// by every module (ControllerNode/TemperatureNode/Thermostat all inherit
// this exact dispatch), so a bug here is the highest-leverage kind. NOT
// covered here: SystemControl's actual reset-to-app/reset-to-bootloader
// trigger (data[0] = 1/2) and Firmware[EnterBootloader] -- both eventually
// call Hal::System::Reset(), which FakeSystem.cpp deliberately implements as
// an infinite loop (matching real hardware's "never returns"), so driving a
// test through that path would hang it. Only the queue-and-don't-flush-yet
// half of those is safe to exercise; the reset itself is a deliberately
// untestable boundary, same as ErrorHandler's halt-on-fault path.
namespace
{
    const uint8_t kNodeId = 5;

    void ResetWorld()
    {
        FakeBus::Reset();
        FakeClock::Reset();
        FakeConfig::Reset();
        FakeConfig::SetValid(true);
        FakeConfig::SetNodeId(kNodeId);
        FakeConfig::SetModule(ModuleType::ControllerNode);
    }

    // Same Poll/flush rationale as ControllerHandlerTests.cpp -- a slave
    // only transmits during its own polled window.
    void Flush(Node& node)
    {
        bus::InjectFrame(Message(kNodeId, Operation::Poll));
        node.Loop();
    }

    struct RecordingHandler : INodeHandler
    {
        int      received = 0;
        int      lost     = 0;
        int      snooped  = 0;
        Message  lastReceived{};
        Message  lastSnooped{};
        uint16_t statusErrorFlags = 0;

        void ReceivedMessage(const Message& m) override
        {
            received++;
            lastReceived = m;
        }
        void ConnectionLost() override
        {
            lost++;
        }
        void Snoop(const Message& m) override
        {
            snooped++;
            lastSnooped = m;
        }
        void FillStatus(SystemStatus& status) override
        {
            status.errorFlags = statusErrorFlags;
        }
    };

    uint32_t ReadU32(const uint8_t* const p)
    {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
            (static_cast<uint32_t>(p[3]) << 24);
    }

    bool FindMessage(Message* const tx, const int n, const Endpoint endpoint, const Operation op, int* const outIndex)
    {
        for (int i = 0; i < n; i++)
        {
            if (tx[i].id.endpoint == endpoint && tx[i].id.operation == op)
            {
                if (outIndex)
                {
                    *outIndex = i;
                }
                return true;
            }
        }
        return false;
    }
} // namespace

CC_TEST(Node, DropsAMessageAddressedToAnotherNode)
{
    ResetWorld();
    Node             node;
    RecordingHandler handler;
    node.RegisterHandler(&handler);
    node.Init();

    bus::InjectFrame(Message(Id(7, Endpoint::DamperTarget, Operation::Set), static_cast<uint8_t>(42)));
    node.Loop();

    CC_CHECK_EQ(handler.received, 0);
}

CC_TEST(Node, DeliversABroadcastSetToTheHandler)
{
    ResetWorld();
    Node             node;
    RecordingHandler handler;
    node.RegisterHandler(&handler);
    node.Init();

    bus::InjectFrame(Message(Id(NodeLib::BROADCAST_NODE, Endpoint::DamperTarget, Operation::Set), static_cast<uint8_t>(0)));
    node.Loop();

    CC_CHECK_EQ(handler.received, 1);
}

CC_TEST(Node, IgnoresABroadcastGet)
{
    ResetWorld();
    Node             node;
    RecordingHandler handler;
    node.RegisterHandler(&handler);
    node.Init();

    bus::InjectFrame(Message(Id(NodeLib::BROADCAST_NODE, Endpoint::DamperTarget, Operation::Get)));
    node.Loop();

    CC_CHECK_EQ(handler.received, 0);
}

CC_TEST(Node, SnoopSeesEveryFrameIncludingOnesNotAddressedToUs)
{
    ResetWorld();
    Node             node;
    RecordingHandler handler;
    node.RegisterHandler(&handler);
    node.Init();

    bus::InjectFrame(Message(Id(7, Endpoint::SupplyTemp, Operation::Report), static_cast<uint8_t>(0)));
    node.Loop();

    CC_CHECK_EQ(handler.snooped, 1);
    CC_CHECK(handler.lastSnooped.id.endpoint == Endpoint::SupplyTemp);
    CC_CHECK_EQ(handler.received, 0); // not addressed to us -- ReceivedMessage() still never fires
}

CC_TEST(Node, SnoopAlsoSeesAMessageThatIsAddressedToUs)
{
    ResetWorld();
    Node             node;
    RecordingHandler handler;
    node.RegisterHandler(&handler);
    node.Init();

    bus::InjectFrame(Message(Id(kNodeId, Endpoint::DamperTarget, Operation::Set), static_cast<uint8_t>(1)));
    node.Loop();

    CC_CHECK_EQ(handler.snooped, 1);
    CC_CHECK_EQ(handler.received, 1);
}

CC_TEST(Node, SystemInfoReportsModuleAndFirmwareVersion)
{
    ResetWorld();
    Node node;
    node.Init();

    bus::InjectFrame(Message(Id(kNodeId, Endpoint::SystemInfo, Operation::Get)));
    Flush(node);

    Message   tx[8];
    const int n = bus::DecodeTx(tx, 8);
    int       idx;
    CC_CHECK(FindMessage(tx, n, Endpoint::SystemInfo, Operation::Report, &idx));
    CC_CHECK_EQ(tx[idx].len, 6);
    CC_CHECK_EQ(tx[idx].data[0], static_cast<uint8_t>(ModuleType::ControllerNode));
    // FakeImageDescriptor.cpp bakes in fwVersionMajor=0, fwVersionMinor=1.
    CC_CHECK_EQ(tx[idx].data[2], 0);
    CC_CHECK_EQ(tx[idx].data[3], 0);
    CC_CHECK_EQ(tx[idx].data[4], 1);
    CC_CHECK_EQ(tx[idx].data[5], 0);
}

CC_TEST(Node, SystemStatusIncludesTheHandlersContributedErrorFlags)
{
    ResetWorld();
    Node             node;
    RecordingHandler handler;
    handler.statusErrorFlags = 0x0042;
    node.RegisterHandler(&handler);
    node.Init();

    bus::InjectFrame(Message(Id(kNodeId, Endpoint::SystemStatus, Operation::Get)));
    Flush(node);

    Message   tx[8];
    const int n = bus::DecodeTx(tx, 8);
    int       idx;
    CC_CHECK(FindMessage(tx, n, Endpoint::SystemStatus, Operation::Report, &idx));
    CC_CHECK_EQ(tx[idx].len, 8);
    const uint16_t errorFlags = static_cast<uint16_t>(tx[idx].data[5] | (tx[idx].data[6] << 8));
    CC_CHECK_EQ(errorFlags, 0x0042);
}

CC_TEST(Node, SystemControlIdentifyIsAckedWithoutTouchingReset)
{
    ResetWorld();
    Node node;
    node.Init();

    Message identify(Id(kNodeId, Endpoint::SystemControl, Operation::Set));
    identify.data[0] = 3; // identify
    identify.data[1] = 5; // seconds
    identify.len     = 2;
    bus::InjectFrame(identify);
    Flush(node); // safe -- identify never sets resetPending, so flushQueue() can't hang here

    Message   tx[8];
    const int n = bus::DecodeTx(tx, 8);
    CC_CHECK(FindMessage(tx, n, Endpoint::SystemControl, Operation::Ack, nullptr));
}

CC_TEST(Node, SystemControlNacksAnUnknownCommand)
{
    ResetWorld();
    Node node;
    node.Init();

    Message bad(Id(kNodeId, Endpoint::SystemControl, Operation::Set));
    bad.data[0] = 99;
    bad.len     = 1;
    bus::InjectFrame(bad);
    Flush(node);

    Message   tx[8];
    const int n = bus::DecodeTx(tx, 8);
    CC_CHECK(FindMessage(tx, n, Endpoint::SystemControl, Operation::Nack, nullptr));
}

CC_TEST(Node, FirmwareNacksAnythingOtherThanEnterBootloader)
{
    ResetWorld();
    Node node;
    node.Init();

    Message m(Id(kNodeId, Endpoint::Firmware, Operation::Set));
    m.data[0] = static_cast<uint8_t>(FirmwareOp::Begin); // only the bootloader serves the transfer -- a running app Nacks it
    m.len     = 1;
    bus::InjectFrame(m);
    Flush(node);

    Message   tx[8];
    const int n = bus::DecodeTx(tx, 8);
    CC_CHECK(FindMessage(tx, n, Endpoint::Firmware, Operation::Nack, nullptr));
}

CC_TEST(Node, DiagTxCountersIncrementsAndDiagResetZeroesItAgain)
{
    ResetWorld();
    Node node;
    node.Init();

    Flush(node); // a bare Poll -> Done still counts as one transmitted frame

    FakeBus::Reset();
    bus::InjectFrame(Message(Id(kNodeId, Endpoint::DiagTxCounters, Operation::Get)));
    Flush(node);

    Message tx[8];
    int     n = bus::DecodeTx(tx, 8);
    int     idx;
    CC_CHECK(FindMessage(tx, n, Endpoint::DiagTxCounters, Operation::Report, &idx));
    CC_CHECK_EQ(tx[idx].len, 8);
    // The Report's payload is built (and queued) BEFORE this flush writes
    // anything of its own -- so it reflects the earlier bare-Poll flush's
    // Done, not this flush's own not-yet-written Report/Done.
    const uint32_t txFramesBefore = ReadU32(tx[idx].data);
    CC_CHECK_EQ(txFramesBefore, 1);

    FakeBus::Reset();
    bus::InjectFrame(Message(Id(kNodeId, Endpoint::DiagReset, Operation::Set), static_cast<uint8_t>(0)));
    bus::InjectFrame(Message(Id(kNodeId, Endpoint::DiagTxCounters, Operation::Get)));
    Flush(node);

    n = bus::DecodeTx(tx, 8);
    CC_CHECK(FindMessage(tx, n, Endpoint::DiagReset, Operation::Ack, nullptr));
    CC_CHECK(FindMessage(tx, n, Endpoint::DiagTxCounters, Operation::Report, &idx));
    // DiagReset zeroed txFrames while handling that frame; the DiagTxCounters
    // Get right after it is processed (and its Report payload built) before
    // this flush's own Ack/Report/Done writes ever increment txFrames again
    // -- so the reported value is exactly 0, not just "lower than before".
    CC_CHECK_EQ(ReadU32(tx[idx].data), 0);
}

CC_TEST(Node, HeartbeatNeverFiresBeforeTheFirstPoll)
{
    ResetWorld();
    Node             node;
    RecordingHandler handler;
    node.RegisterHandler(&handler);
    node.Init();

    FakeClock::Advance(60000); // long past any reasonable timeout
    node.Loop();

    CC_CHECK_EQ(handler.lost, 0); // heartbeat only arms once a Poll has been seen
}

CC_TEST(Node, ConnectionLostFiresAfterAPolledNodeGoesQuiet)
{
    ResetWorld();
    Node             node;
    RecordingHandler handler;
    node.RegisterHandler(&handler);
    node.Init();

    bus::InjectFrame(Message(kNodeId, Operation::Poll)); // arms the heartbeat
    node.Loop();
    CC_CHECK_EQ(handler.lost, 0);

    FakeClock::Advance(1001); // just past Node's 1000ms heartbeat window
    node.Loop();

    CC_CHECK_EQ(handler.lost, 1);
}

CC_TEST(Node, DiagLogDrainsBufferedLogLinesOnePerGetThenReportsEmpty)
{
    ResetWorld();
    Node node;
    node.Init();
    Tools::LogRing::Clear(); // drop whatever Init() logged
    FakeClock::Set(7000);
    Tools::LogRing::Push("I", "first line");
    FakeClock::Set(9000);
    Tools::LogRing::Push("W", "second line");
    FakeClock::Set(21000);

    // Report = uptimeSec(3 LE) + text; text-less means drained, uptime = now.
    const char* const expected[]       = {"I: first line", "W: second line"};
    const uint8_t     expectedUptime[] = {7, 9, 21};
    for (int i = 0; i < 3; i++)
    {
        FakeBus::Reset();
        bus::InjectFrame(Message(Id(kNodeId, Endpoint::DiagLog, Operation::Get)));
        Flush(node);

        Message   tx[8];
        const int n = bus::DecodeTx(tx, 8);
        int       idx;
        CC_CHECK(FindMessage(tx, n, Endpoint::DiagLog, Operation::Report, &idx));
        CC_CHECK_EQ(tx[idx].data[0], expectedUptime[i]);
        CC_CHECK_EQ(tx[idx].data[1], 0);
        CC_CHECK_EQ(tx[idx].data[2], 0);
        if (i < 2)
        {
            CC_CHECK_EQ(tx[idx].len, 3 + strlen(expected[i]));
            CC_CHECK(memcmp(&tx[idx].data[3], expected[i], tx[idx].len - 3) == 0);
        }
        else
        {
            CC_CHECK_EQ(tx[idx].len, 3); // drained
        }
    }
}

CC_TEST(Node, DiagLogFitsOneBusMessageEvenForAnOverlongLogLine)
{
    ResetWorld();
    Node node;
    node.Init();
    Tools::LogRing::Clear();
    Tools::LogRing::Push("E", "0123456789012345678901234567890123456789012345678901234567890123456789");

    bus::InjectFrame(Message(Id(kNodeId, Endpoint::DiagLog, Operation::Get)));
    Flush(node);

    Message   tx[8];
    const int n = bus::DecodeTx(tx, 8);
    int       idx;
    CC_CHECK(FindMessage(tx, n, Endpoint::DiagLog, Operation::Report, &idx));
    CC_CHECK_EQ(tx[idx].len, 3 + Tools::LogRing::LineSize);
    CC_CHECK(tx[idx].len <= NodeLib::MAX_DATA);
}

CC_TEST(Node, DiagLogNacksAnythingButGet)
{
    ResetWorld();
    Node node;
    node.Init();

    bus::InjectFrame(Message(Id(kNodeId, Endpoint::DiagLog, Operation::Set), static_cast<uint8_t>(0)));
    Flush(node);

    Message   tx[8];
    const int n = bus::DecodeTx(tx, 8);
    CC_CHECK(FindMessage(tx, n, Endpoint::DiagLog, Operation::Nack, nullptr));
}

CC_TEST(Node, AnsweringDiscoveryLogsIntoTheDiagLogRingOncePerSweep)
{
    ResetWorld();
    Node node;
    node.Init();
    Tools::LogRing::Clear();

    for (int i = 0; i < 3; i++) // three discovery sweeps
    {
        bus::InjectFrame(Message(NodeLib::BROADCAST_NODE, Operation::Discover));
        node.Loop();
    }

    // The Announce went out ...
    Message   tx[8];
    const int n = bus::DecodeTx(tx, 8);
    int       idx;
    CC_CHECK(FindMessage(tx, n, Endpoint::Transport, Operation::Announce, &idx));

    // ... and each sweep left "Handle discover" then "Return Announce" in the
    // ring, oldest first, each short enough for one DiagLog message.
    CC_CHECK_EQ(Tools::LogRing::Buffered(), 6);
    const char* const expected[] = {"I: Handle discover", "I: Return Announce"};
    for (int i = 0; i < 6; i++)
    {
        uint8_t      line[Tools::LogRing::LineSize];
        uint32_t     at;
        const size_t len = Tools::LogRing::Pop(line, sizeof(line), at);
        CC_CHECK_EQ(len, strlen(expected[i % 2]));
        CC_CHECK(memcmp(line, expected[i % 2], len) == 0);
    }
}
