/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include "Gpio.h"
#include "INodeHandler.h"
#include "NodeMaster.h"

#include "NinaLink.h"

#include "BudgetAllocator.h"
#include "HalNinaPort.h"
#include "ThermostatMonitor.h"

// MainController's bridge to the server over the on-board NINA-W152
// (MainController-Server-Link-Spec.md). The connection itself -- Wi-Fi join,
// the TCP peer, keepalive and watchdog -- is a NinaLink (Lib/Nina); this class
// is the application's side of it, relaying NodeLib frames verbatim in both
// directions once the uplink is up:
//   - bus -> uplink: registered as the NodeMaster's INodeHandler, so every
//     frame NodeMaster sees reaches ReceivedMessage() (NodeMaster.cpp already
//     forwards there for anything past its own Transport bookkeeping).
//     ReceivedMessage() also feeds BudgetAllocator::Observe() -- unconditionally,
//     even with the uplink down, since that's bus-side supervision, not
//     something the uplink should gate (Damper-Budget-Spec.md §5.4).
//     ReceivedMessage() only enqueues -- it must never block, since NodeMaster
//     calls it synchronously from inside its own byte-at-a-time bus receive
//     loop (Node::PumpRx()); a blocking NINA write there, multiplied by a
//     burst of several queued node messages arriving back-to-back, previously
//     stalled bus servicing long enough to miss bytes (and the terminating
//     Done) on a real bus with real traffic. The actual NINA writes happen
//     from Loop()/DrainDataMode() instead, decoupled from bus reception.
//   - uplink -> bus: relayed-range frames decoded from the NINA UART are
//     pushed onto the bus via NodeMaster::QueueMessage().
//
// Entirely non-blocking -- Loop() does at most one AT command's worth of
// progress per call, ticked from the same super-loop as NodeMaster::Loop()
// (MainController-Server-Link-Spec.md §3).
class UplinkHandler : public NodeLib::INodeHandler, public NinaLinkHandler
{
  public:
    UplinkHandler(NodeLib::NodeMaster& master, BudgetAllocator& budgetAllocator);

    void Init();
    void Loop();

    // NodeLib::INodeHandler
    void ReceivedMessage(const NodeLib::Message& message) override;
    void ConnectionLost() override;

    // NinaLinkHandler
    void BuildHello(NodeLib::Message& hello) override;
    void OnFrame(const NodeLib::Message& message) override;
    void OnConnected() override;
    void BeforeFrames() override;
    void AfterFrames() override;

  private:
    // Host tests reach the roster/presence/frame-handling logic and the
    // outbound queue directly (test/UplinkHandlerTests.cpp).
    friend struct UplinkHandlerTestAccess;

    void HandleUplinkFrame(const NodeLib::Message& message);
    // SystemControl addressed to the MainController itself (NODE = 0, never
    // relayed onto the bus): 1 = reset -> app, 2 = reset -> bootloader.
    // Acks, then arms resetPending -- the reset itself runs from
    // AfterFrames() once the Ack has been written out.
    void              HandleSelfControl(const NodeLib::Message& message);
    [[noreturn]] void PerformPendingReset();
    void              SendRoster();
    // Diffs every node's current active/bootloader state against the
    // snapshot SendRoster() last took and emits a NodePresence Report for
    // anything that changed -- a live update in between Roster's periodic
    // full-snapshot dumps. Idempotent to call when nothing changed.
    void CheckNodePresence();
    // Moves up to maxLogLinesPerPass lines from Tools::LogRing into the
    // outbound queue as MainLog Reports, only while the queue is under half
    // full so log traffic never displaces relayed bus frames
    // (MainController-Server-Link-Spec.md §5.1). Never logs itself -- that
    // would feed the ring it is draining.
    void PushLog();
    void EnqueueUplink(const NodeLib::Message& message);
    // Error LED on while the uplink to the server is down (not yet up after
    // boot, or lost), off while it is up. Only touches the pin on a change.
    void ShowUplinkState(const bool up);
    // Moves due 0x63 ThermostatStatus Reports (ThermostatMonitor) into the
    // outbound queue -- same half-full rule and per-pass cap as PushLog().
    void PushThermostatStatus();

    NodeLib::NodeMaster& master;
    BudgetAllocator&     budgetAllocator;
    ThermostatMonitor    thermostats;

    // Frames for the server, staged here by ReceivedMessage() (called
    // synchronously from the bus receive path -- it must only enqueue, never
    // block) and written out by NinaLink from Loop(). Sized well above what
    // one flushQueue() burst from a single node realistically queues
    // (NodeLib::Node::queueSize is 25); a still-full queue just drops the
    // newest message (MainController-Server-Link-Spec.md §7.2).
    static const uint8_t outboundQueueSize = 32;
    NodeLib::Message     outboundQueue[outboundQueueSize];

    static const uint8_t maxLogLinesPerPass         = 2;
    static const uint8_t maxThermostatStatusPerPass = 2;

    // Last active/bootloader state CheckNodePresence() has told the server
    // about, indexed nodeId-1. Seeded by SendRoster() itself (so the roster
    // dump and the snapshot never disagree, and the very next CheckNodePresence()
    // after a (re)connect is a no-op -- Roster already reported the
    // post-reconnect truth, nothing "changed" on top of that).
    bool nodePresenceActive[NodeLib::MAX_NODES];
    bool nodePresenceBootloader[NodeLib::MAX_NODES];

    HalNinaPort    port;
    NinaLinkConfig config;
    NinaLink       link;

    bool resetPending;
    bool resetToBootloader;

    Hal::Gpio errorLed;
    bool      uplinkShown; // ShowUplinkState() has driven the LED at least once
    bool      uplinkUp;
};
