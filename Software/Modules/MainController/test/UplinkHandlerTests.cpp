/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include <string.h>

#include "BoardPins.h"
#include "EEndpoint.h"
#include "EOperation.h"
#include "NodeMaster.h"

#include "BusHelpers.h"
#include "FakeBus.h"
#include "FakeClock.h"
#include "FakeConfigStore.h"
#include "Test.h"

#include "LogRing.h"

#include "BudgetAllocator.h"
#include "UplinkHandler.h"

using NodeLib::ConfigStore;
using NodeLib::Endpoint;
using NodeLib::Id;
using NodeLib::Message;
using NodeLib::ModuleType;
using NodeLib::NodeMaster;
using NodeLib::Operation;

// UplinkHandler::Init()/Loop() drive a multi-state NINA AT bring-up (probe /
// Wi-Fi join / peer connect / data mode) that has no fake at the AT-protocol
// level, so that state machine itself is not exercised. Everything after it
// is: UplinkHandlerTestAccess (a friend) calls the private frame handling,
// SendRoster() and CheckNodePresence() directly and reads back the outbound
// queue -- the exact Messages that would be framed onto the socket -- against
// a real NodeMaster driven over the fake bus. Also covered: ReceivedMessage()
// must feed BudgetAllocator::Observe() unconditionally, even with the uplink
// down (UplinkHandler.cpp's class comment / Damper-Budget-Spec.md §5.4).

// Friend of UplinkHandler (declared there).
struct UplinkHandlerTestAccess
{
    static void HandleUplinkFrame(UplinkHandler& u, const Message& m)
    {
        u.HandleUplinkFrame(m);
    }
    static void SendRoster(UplinkHandler& u)
    {
        u.SendRoster();
    }
    static void CheckNodePresence(UplinkHandler& u)
    {
        u.CheckNodePresence();
    }
    static void PushLog(UplinkHandler& u)
    {
        u.PushLog();
    }
    static void ShowUplinkState(UplinkHandler& u, const bool up)
    {
        u.ShowUplinkState(up);
    }
    static bool Send(UplinkHandler& u, const Message& m)
    {
        return u.link.Send(m);
    }
    static uint8_t Queued(const UplinkHandler& u)
    {
        return u.link.Queued();
    }
    static const Message& Queue(const UplinkHandler& u, const uint8_t i)
    {
        return u.link.QueuedAt(i);
    }
    static void ClearQueue(UplinkHandler& u)
    {
        u.link.ClearQueue();
    }
    static bool ResetPending(const UplinkHandler& u)
    {
        return u.resetPending;
    }
    static bool ResetToBootloader(const UplinkHandler& u)
    {
        return u.resetToBootloader;
    }
};

using Access = UplinkHandlerTestAccess;

namespace
{
    const uint32_t discoveryWindowMs = 250;

    void ResetWorld()
    {
        FakeBus::Reset();
        FakeClock::Reset();
        FakeConfig::Reset();
    }

    void Announce(const uint8_t nodeId, const ModuleType module)
    {
        Message m(nodeId, Operation::Announce);
        m.data[0] = static_cast<uint8_t>(module);
        m.len     = 1;
        bus::InjectFrame(m);
    }

    void PackI16(uint8_t* const out, const int16_t v)
    {
        out[0] = static_cast<uint8_t>(v);
        out[1] = static_cast<uint8_t>(static_cast<uint16_t>(v) >> 8);
    }

    Message ReportI16(const uint8_t nodeId, const Endpoint endpoint, const int16_t value)
    {
        Message m(Id(nodeId, endpoint, Operation::Report));
        PackI16(m.data, value);
        m.len = 2;
        return m;
    }

    // Same order requirement as BudgetAllocatorTests.cpp: Init() broadcasts
    // Discover, so clear the fake bus (which also clears RX) before injecting
    // replies.
    void InitAndClearDiscover(NodeMaster& master)
    {
        master.Init();
        FakeBus::Reset();
    }

    void StartPolling(NodeMaster& master)
    {
        FakeClock::Advance(discoveryWindowMs);
        master.Loop();
    }

    void FlushQueuedBudgets(NodeMaster& master, const uint8_t pendingPollNode)
    {
        FakeBus::Reset();
        bus::InjectFrame(Message(pendingPollNode, Operation::Done));
        master.Loop();
    }

    bool FindBudget(Message* const tx, const int n, const uint8_t nodeId, uint8_t& percent)
    {
        for (int i = 0; i < n; i++)
        {
            if (tx[i].id.node == nodeId && tx[i].id.endpoint == Endpoint::DamperBudget &&
                tx[i].id.operation == Operation::Set && tx[i].len >= 1)
            {
                percent = tx[i].data[0];
                return true;
            }
        }
        return false;
    }
} // namespace

CC_TEST(UplinkHandler, ObservesBusTrafficThroughReceivedMessageEvenWithTheUplinkDown)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);
    UplinkHandler   uplink(master, allocator);
    // uplink.Init()/.Loop() never called -- nina stays out of data mode,
    // i.e. exactly "uplink down" (NinaAt::InDataMode() defaults to false).

    InitAndClearDiscover(master);
    Announce(2, ModuleType::ControllerNode);
    Announce(3, ModuleType::ControllerNode);
    StartPolling(master); // polls node 2 first (lowest id)

    // Two nodes with deliberately different demand: if Observe() were a
    // no-op (e.g. gated behind the uplink's InDataMode() check), weightSum
    // would stay 0 and BOTH nodes would land on the even-split default (25
    // each, pool = 50*2 = 100) -- indistinguishable from "no data yet". The
    // asymmetric 100/0 split below is only reachable if every
    // ReceivedMessage() call actually reached BudgetAllocator::Observe().
    uplink.ReceivedMessage(ReportI16(2, Endpoint::RoomTemp, 2400)); // wants cooling, saturates demand
    uplink.ReceivedMessage(ReportI16(2, Endpoint::RoomSetpoint, 1800));
    uplink.ReceivedMessage(ReportI16(3, Endpoint::RoomTemp, 2000)); // already at setpoint -- zero demand
    uplink.ReceivedMessage(ReportI16(3, Endpoint::RoomSetpoint, 2000));
    uplink.ReceivedMessage(ReportI16(9, Endpoint::SupplyTemp, 1500)); // colder than either room -- helps cooling

    allocator.Loop();
    FlushQueuedBudgets(master, 2);

    Message   tx[8];
    const int n  = bus::DecodeTx(tx, 8);
    uint8_t   p2 = 0;
    uint8_t   p3 = 0;
    CC_CHECK(FindBudget(tx, n, 2, p2));
    CC_CHECK(FindBudget(tx, n, 3, p3));
    CC_CHECK_EQ(p2, 100);
    CC_CHECK_EQ(p3, 0);
}

// --- roster / presence / uplink frame handling -----------------------------

namespace
{
    // Node 2 = ControllerNode running its app, node 5 = TemperatureNode
    // resident in its bootloader; the master is left polling node 2.
    void TwoNodesUp(NodeMaster& master)
    {
        InitAndClearDiscover(master);
        Announce(2, ModuleType::ControllerNode);
        Message b(5, Operation::Announce);
        b.data[0] = static_cast<uint8_t>(ModuleType::TemperatureNode);
        b.data[1] = 1; // bl-idle
        b.len     = 2;
        bus::InjectFrame(b);
        StartPolling(master);
        // The master's own Discover/Poll frames are not under test.
        FakeBus::Reset();
    }

    uint32_t ReadU32(const uint8_t* const p)
    {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
            (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    }

    bool IsRosterEntry(
        const Message& m,
        const uint8_t  nodeId,
        const uint8_t  module,
        const uint8_t  bootloader,
        const uint32_t lastMs)
    {
        return m.id.node == 0 && m.id.endpoint == Endpoint::Roster && m.id.operation == Operation::Report &&
            m.len == 7 && m.data[0] == nodeId && m.data[1] == module && m.data[2] == bootloader &&
            ReadU32(&m.data[3]) == lastMs;
    }

    bool IsRosterEnd(const Message& m)
    {
        return m.id.endpoint == Endpoint::Roster && m.id.operation == Operation::Report && m.len == 1 &&
            m.data[0] == 0xFF;
    }

    bool IsPresence(
        const Message& m,
        const uint8_t  nodeId,
        const uint8_t  module,
        const uint8_t  up,
        const uint8_t  bootloader)
    {
        return m.id.node == 0 && m.id.endpoint == Endpoint::NodePresence && m.id.operation == Operation::Report &&
            m.len == 4 && m.data[0] == nodeId && m.data[1] == module && m.data[2] == up && m.data[3] == bootloader;
    }

    // Pending-poll node's Done flushes whatever the uplink queued for the bus;
    // returns how many frames of 'endpoint' went out.
    int BusFramesFor(NodeMaster& master, const Endpoint endpoint)
    {
        FlushQueuedBudgets(master, 2);
        Message   tx[16];
        const int n     = bus::DecodeTx(tx, 16);
        int       count = 0;
        for (int i = 0; i < n; i++)
        {
            if (tx[i].id.endpoint == endpoint)
            {
                count++;
            }
        }
        return count;
    }
} // namespace

CC_TEST(UplinkHandler, SendRosterListsActiveNodesThenTerminates)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);
    UplinkHandler   uplink(master, allocator);
    TwoNodesUp(master);

    Access::SendRoster(uplink);

    CC_CHECK_EQ(Access::Queued(uplink), 3);
    CC_CHECK(IsRosterEntry(Access::Queue(uplink, 0), 2, 1, 0, master.NodeLastContactMs(2)));
    CC_CHECK(IsRosterEntry(Access::Queue(uplink, 1), 5, 2, 1, master.NodeLastContactMs(5))); // bootloader bit set
    CC_CHECK(IsRosterEnd(Access::Queue(uplink, 2)));
}

CC_TEST(UplinkHandler, SendRosterWithNoNodesIsJustTheTerminator)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);
    UplinkHandler   uplink(master, allocator);
    InitAndClearDiscover(master);

    Access::SendRoster(uplink);

    CC_CHECK_EQ(Access::Queued(uplink), 1);
    CC_CHECK(IsRosterEnd(Access::Queue(uplink, 0)));
}

CC_TEST(UplinkHandler, SendRosterSkipsNodesThatDroppedOut)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);
    UplinkHandler   uplink(master, allocator);
    TwoNodesUp(master);

    master.Loop(); // Flush -> sends the first Poll (to node 2)
    FakeClock::Advance(200); // pollTimeoutMs -- polled node 2 never answered
    master.Loop();
    CC_CHECK(!master.NodeActive(2));

    Access::SendRoster(uplink);

    CC_CHECK_EQ(Access::Queued(uplink), 2); // node 5 + terminator
    CC_CHECK_EQ(Access::Queue(uplink, 0).data[0], 5);
    CC_CHECK(IsRosterEnd(Access::Queue(uplink, 1)));
}

CC_TEST(UplinkHandler, PresenceIsQuietRightAfterARosterDump)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);
    UplinkHandler   uplink(master, allocator);
    TwoNodesUp(master);

    Access::SendRoster(uplink); // seeds the snapshot with what it just reported
    Access::ClearQueue(uplink);
    Access::CheckNodePresence(uplink);

    CC_CHECK_EQ(Access::Queued(uplink), 0);
}

CC_TEST(UplinkHandler, PresenceReportsANodeDroppingOutOnce)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);
    UplinkHandler   uplink(master, allocator);
    TwoNodesUp(master);
    Access::SendRoster(uplink);
    Access::ClearQueue(uplink);

    master.Loop(); // Flush -> sends the first Poll (to node 2)
    FakeClock::Advance(200);
    master.Loop(); // node 2 misses its poll
    Access::CheckNodePresence(uplink);

    CC_CHECK_EQ(Access::Queued(uplink), 1);
    CC_CHECK(IsPresence(Access::Queue(uplink, 0), 2, 1, 0, 0)); // up = 0

    Access::CheckNodePresence(uplink); // nothing further changed
    CC_CHECK_EQ(Access::Queued(uplink), 1);
}

CC_TEST(UplinkHandler, PresenceReportsANodeJoiningAndABootloaderTransition)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);
    UplinkHandler   uplink(master, allocator);
    TwoNodesUp(master);
    Access::SendRoster(uplink);
    Access::ClearQueue(uplink);

    // Node 5 comes back up in its app, and a brand-new node 7 appears.
    Announce(5, ModuleType::TemperatureNode);
    Announce(7, ModuleType::ControllerNode);
    master.Loop();
    Access::CheckNodePresence(uplink);

    CC_CHECK_EQ(Access::Queued(uplink), 2);
    CC_CHECK(IsPresence(Access::Queue(uplink, 0), 5, 2, 1, 0)); // still up, bootloader bit cleared
    CC_CHECK(IsPresence(Access::Queue(uplink, 1), 7, 1, 1, 0)); // joined
}

CC_TEST(UplinkHandler, RosterGetSendsTheDumpAgainAndReseedsPresence)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);
    UplinkHandler   uplink(master, allocator);
    TwoNodesUp(master);

    Access::HandleUplinkFrame(uplink, Message(Id(0, Endpoint::Roster, Operation::Get)));

    CC_CHECK_EQ(Access::Queued(uplink), 3);
    CC_CHECK(IsRosterEntry(Access::Queue(uplink, 0), 2, 1, 0, master.NodeLastContactMs(2)));
    CC_CHECK(IsRosterEntry(Access::Queue(uplink, 1), 5, 2, 1, master.NodeLastContactMs(5)));
    CC_CHECK(IsRosterEnd(Access::Queue(uplink, 2)));

    Access::ClearQueue(uplink);
    Access::CheckNodePresence(uplink);
    CC_CHECK_EQ(Access::Queued(uplink), 0); // the dump was the current truth
}

CC_TEST(UplinkHandler, KeepaliveGetIsAnswered)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);
    UplinkHandler   uplink(master, allocator);
    InitAndClearDiscover(master);

    Access::HandleUplinkFrame(uplink, Message(Id(0, Endpoint::Keepalive, Operation::Get)));

    CC_CHECK_EQ(Access::Queued(uplink), 1);
    CC_CHECK(Access::Queue(uplink, 0).id.endpoint == Endpoint::Keepalive);
    CC_CHECK(Access::Queue(uplink, 0).id.operation == Operation::Report);
}

CC_TEST(UplinkHandler, RelayedEndpointsForABusNodeGoOntoTheBus)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);
    UplinkHandler   uplink(master, allocator);
    TwoNodesUp(master);

    Message set(Id(2, Endpoint::DamperTarget, Operation::Set));
    set.data[0] = 50;
    set.len     = 1;
    Access::HandleUplinkFrame(uplink, set);

    CC_CHECK_EQ(Access::Queued(uplink), 0); // nothing answered on the uplink
    CC_CHECK_EQ(BusFramesFor(master, Endpoint::DamperTarget), 1);
}

CC_TEST(UplinkHandler, SystemControlForTheMainControllerIsHandledHereNotRelayed)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);
    UplinkHandler   uplink(master, allocator);
    TwoNodesUp(master);

    Message toBootloader(Id(0, Endpoint::SystemControl, Operation::Set));
    toBootloader.data[0] = 2;
    toBootloader.len     = 1;
    Access::HandleUplinkFrame(uplink, toBootloader);

    CC_CHECK_EQ(Access::Queued(uplink), 1);
    CC_CHECK(Access::Queue(uplink, 0).id.endpoint == Endpoint::SystemControl);
    CC_CHECK(Access::Queue(uplink, 0).id.operation == Operation::Ack);
    CC_CHECK(Access::ResetPending(uplink));
    CC_CHECK(Access::ResetToBootloader(uplink));
    CC_CHECK_EQ(BusFramesFor(master, Endpoint::SystemControl), 0); // never reached the bus
}

CC_TEST(UplinkHandler, SystemControlResetToAppDoesNotSetTheBootloaderMagic)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);
    UplinkHandler   uplink(master, allocator);
    InitAndClearDiscover(master);

    Message toApp(Id(0, Endpoint::SystemControl, Operation::Set));
    toApp.data[0] = 1;
    toApp.len     = 1;
    Access::HandleUplinkFrame(uplink, toApp);

    CC_CHECK(Access::Queue(uplink, 0).id.operation == Operation::Ack);
    CC_CHECK(Access::ResetPending(uplink));
    CC_CHECK(!Access::ResetToBootloader(uplink));
}

CC_TEST(UplinkHandler, SystemControlWithAnUnknownValueOrVerbIsNackedWithoutResetting)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);
    UplinkHandler   uplink(master, allocator);
    InitAndClearDiscover(master);

    Message identify(Id(0, Endpoint::SystemControl, Operation::Set));
    identify.data[0] = 3; // a bus-node command the MainController does not implement
    identify.len     = 1;
    Access::HandleUplinkFrame(uplink, identify);
    Access::HandleUplinkFrame(uplink, Message(Id(0, Endpoint::SystemControl, Operation::Get)));

    CC_CHECK_EQ(Access::Queued(uplink), 2);
    CC_CHECK(Access::Queue(uplink, 0).id.operation == Operation::Nack);
    CC_CHECK(Access::Queue(uplink, 1).id.operation == Operation::Nack);
    CC_CHECK(!Access::ResetPending(uplink));
}

CC_TEST(UplinkHandler, SystemControlForABusNodeIsStillRelayed)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);
    UplinkHandler   uplink(master, allocator);
    TwoNodesUp(master);

    Message reset(Id(2, Endpoint::SystemControl, Operation::Set));
    reset.data[0] = 2;
    reset.len     = 1;
    Access::HandleUplinkFrame(uplink, reset);

    CC_CHECK(!Access::ResetPending(uplink)); // the MainController itself is untouched
    CC_CHECK_EQ(Access::Queued(uplink), 0);
    CC_CHECK_EQ(BusFramesFor(master, Endpoint::SystemControl), 1);
}

CC_TEST(UplinkHandler, OtaFramesAreIgnoredByTheRunningApp)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);
    UplinkHandler   uplink(master, allocator);
    InitAndClearDiscover(master);

    Message begin(Id(0, Endpoint::OtaControl, Operation::Set));
    begin.data[0] = 1;
    begin.len     = 1;
    Access::HandleUplinkFrame(uplink, begin);
    Access::HandleUplinkFrame(uplink, Message(Id(0, Endpoint::OtaData, Operation::Set)));

    CC_CHECK_EQ(Access::Queued(uplink), 0);
}

namespace
{
    bool IsMainLog(const Message& m, const uint32_t uptimeSec, const char* const text)
    {
        const size_t textLen = strlen(text);
        if (m.id.node != 0 || m.id.endpoint != Endpoint::MainLog || m.id.operation != Operation::Report ||
            m.len != 3 + textLen)
        {
            return false;
        }
        const uint32_t at = m.data[0] | (m.data[1] << 8) | (static_cast<uint32_t>(m.data[2]) << 16);
        return at == uptimeSec && memcmp(&m.data[3], text, textLen) == 0;
    }
} // namespace

CC_TEST(UplinkHandler, PushLogSendsAtMostTwoRingLinesPerPassOldestFirst)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);
    UplinkHandler   uplink(master, allocator);
    InitAndClearDiscover(master);
    Tools::LogRing::Clear();

    FakeClock::Set(5000);
    Tools::LogRing::Push('I', "one");
    Tools::LogRing::Push('W', "two");
    FakeClock::Set(7000);
    Tools::LogRing::Push('E', "three");

    Access::PushLog(uplink);
    CC_CHECK_EQ(Access::Queued(uplink), 2);
    CC_CHECK(IsMainLog(Access::Queue(uplink, 0), 5, "I: one"));
    CC_CHECK(IsMainLog(Access::Queue(uplink, 1), 5, "W: two"));

    Access::PushLog(uplink);
    CC_CHECK_EQ(Access::Queued(uplink), 3);
    CC_CHECK(IsMainLog(Access::Queue(uplink, 2), 7, "E: three"));

    Access::PushLog(uplink); // drained -- nothing more
    CC_CHECK_EQ(Access::Queued(uplink), 3);
}

CC_TEST(UplinkHandler, PushLogWaitsWhileTheOutboundQueueIsHalfFull)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);
    UplinkHandler   uplink(master, allocator);
    InitAndClearDiscover(master);
    Tools::LogRing::Clear();
    FakeClock::Set(9000);
    Tools::LogRing::Push('I', "held back");

    for (uint8_t i = 0; i < 16; i++) // outboundQueueSize / 2
    {
        Access::Send(uplink, Message(Id(3, Endpoint::RoomTemp, Operation::Report)));
    }
    Access::PushLog(uplink);
    CC_CHECK_EQ(Access::Queued(uplink), 16);
    CC_CHECK_EQ(Tools::LogRing::Buffered(), 1); // still in the ring

    Access::ClearQueue(uplink);
    Access::PushLog(uplink);
    CC_CHECK_EQ(Access::Queued(uplink), 1);
    CC_CHECK(IsMainLog(Access::Queue(uplink, 0), 9, "I: held back"));
}

CC_TEST(UplinkHandler, PushLogReportsLinesLostToAnOverflowFirst)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);
    UplinkHandler   uplink(master, allocator);
    InitAndClearDiscover(master);
    Tools::LogRing::Clear();
    FakeClock::Set(9000);
    for (uint8_t i = 0; i < Tools::LogRing::Lines + 3; i++)
    {
        Tools::LogRing::Push('I', "x");
    }

    Access::PushLog(uplink);
    CC_CHECK_EQ(Access::Queued(uplink), 2);
    CC_CHECK(IsMainLog(Access::Queue(uplink, 0), 9, "~ 3 lost"));
    CC_CHECK(IsMainLog(Access::Queue(uplink, 1), 9, "I: x"));
}

namespace
{
    bool ErrorLedOn()
    {
        return (Board::ErrorLed.port->ODR & Board::ErrorLed.pin) != 0u;
    }
} // namespace

CC_TEST(UplinkHandler, ErrorLedIsOnWhileTheUplinkIsDown)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);
    UplinkHandler   uplink(master, allocator);
    uplink.Init();

    uplink.Loop(); // no NINA session in the host build -- never reaches data mode

    CC_CHECK(ErrorLedOn());
}

CC_TEST(UplinkHandler, ErrorLedGoesOffWhenTheUplinkComesUpAndBackOnWhenItIsLost)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);
    UplinkHandler   uplink(master, allocator);

    UplinkHandlerTestAccess::ShowUplinkState(uplink, false);
    CC_CHECK(ErrorLedOn());
    UplinkHandlerTestAccess::ShowUplinkState(uplink, true); // connected
    CC_CHECK(!ErrorLedOn());
    UplinkHandlerTestAccess::ShowUplinkState(uplink, false); // lost
    CC_CHECK(ErrorLedOn());
}

CC_TEST(UplinkHandler, TheBusMasterLeavesTheErrorLedToTheUplink)
{
    ResetWorld();
    NodeMaster      master;
    BudgetAllocator allocator(master);
    UplinkHandler   uplink(master, allocator);
    master.RegisterHandler(&uplink);
    master.Init();

    UplinkHandlerTestAccess::ShowUplinkState(uplink, true); // uplink up -> LED off
    for (int i = 0; i < 50; i++)
    {
        FakeClock::Advance(100);
        master.Loop(); // a slave's bus-loss LED logic must not run on the master
    }
    CC_CHECK(!ErrorLedOn());
}
