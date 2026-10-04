# Damper Budget & Room Control Loop — Design Spec

**Companion docs:** `ControllerNode-Thermostat-Link-Spec.md` (resolves that spec's §6 open item 1 — the room loop lives on `ControllerNode`), `Node-Message-Model-Spec.md` (the `DamperBudget` endpoint, §3 here), `MainController-Spec.md` (§2 there — this is the "supervisor" role, not a room-level closed loop), `TemperatureNode-Spec.md` (`SupplyTemp`/`ReturnTemp` source)

---

## 1. Split of responsibility

- **`ControllerNode` runs the room's control loop.** This resolves `ControllerNode-Thermostat-Link-Spec.md` §6 open item 1: yes, `ControllerNode` compares its own Thermostat's setpoint/room-temp against the shared duct supply-air temperature and drives its own damper — `MainController` is never in that loop.
- **`MainController` runs a fleet-wide fairness arbitration on top**, not a room loop — consistent with `MainController-Spec.md` §2 ("supervisor + bridge/logger, not a closed-loop controller"). It watches every node's `Room*`/`SupplyTemp` traffic (it already sees all of it, being bus master) and periodically narrows what each `ControllerNode`'s loop is *permitted* to do, by sending it a `DamperBudget` **range** — a max and a min, never a target. The max is the room's fair share of the airflow pool (§5.2). The min is normally 0, and is raised when the dampers together would close off too much of the duct: the HVAC unit's fan always runs and its air has to go somewhere (§5.5).
- Each `ControllerNode` still decides locally where inside its current range to sit; it does not always run at the max.

---

## 2. Shared per-room demand logic (`Lib/NodeLib/RoomDemand.h`)

Both firmware images need to agree on "does this room need this supply air right now" — computed once, in a host-testable, allocation-free pair of pure functions, not duplicated:

```cpp
namespace NodeLib
{
    // True if supply air at supplyCentiC would move a room at roomTempCentiC
    // toward setpointCentiC -- evaluated per room, not against a house-wide
    // heating/cooling label. A room too warm needs supply colder than itself;
    // a room too cold needs supply warmer than itself. False if supply sits on
    // the wrong side to help, or the room is already within deadbandCentiC of
    // setpoint.
    //
    // Deliberately NOT supply-vs-return: return air reflects the whole house's
    // average, and a room can want the opposite of that average (e.g. return
    // 20.0C, supply 19.0C -- house trending cooling overall -- but a room at
    // 18.0C: supply is still warmer than that room, so opening its damper
    // heats it, correctly, even though the house-wide trend is "cooling").
    bool SupplyHelpsRoom(int16_t supplyCentiC, int16_t roomTempCentiC, int16_t setpointCentiC,
                         int16_t deadbandCentiC = 30);

    // 0..100: magnitude of this room's unmet demand, scaled linearly from 0 at
    // deadbandCentiC to 100 at fullAuthorityCentiC past deadband. 0 whenever
    // !SupplyHelpsRoom().
    uint8_t RoomDemandPercent(int16_t supplyCentiC, int16_t roomTempCentiC, int16_t setpointCentiC,
                               int16_t deadbandCentiC = 30, int16_t fullAuthorityCentiC = 300);
}
```

This one function is used twice, for two different purposes, from the same inputs:

- `ControllerNode`'s `RoomControlLoop` uses it as the **desired damper position**, kept inside the node's current budget range.
- `MainController`'s `BudgetAllocator` uses it as the **weight** for splitting the budget pool across nodes.

Both consumers compute in centi-°C (0.01 °C resolution, the same unit `RoomTemp`/`RoomSetpoint`/`SupplyTemp` already use on the wire) all the way through; only the final `uint8_t` handed to `Damper::SetTarget()` or the `DamperBudget` endpoint rounds down to a percent. Percent is not a precision loss here — see §6.

---

## 3. Wire changes (`Node-Message-Model-Spec.md`)

### 3.1 New endpoint

```cpp
DamperBudget = 0x33,  // RW  max(1) [min(1)] %  -- range DamperTarget stays in while DamperMode == Auto
DumpRoom     = 0x34,  // RW  uint8 0/1  -- this room takes the surplus air (§5.5); persisted on the node
```

`DamperBudget` carries the max first and the min as an optional second byte. A 1-byte `Set` is a plain ceiling with min 0, and a node reading only `data[0]` still gets its ceiling, so the two-byte form is compatible in both directions. If a min arrives above the max, the min wins: the node raises its max to match.

Free slot in the `0x30`–`0x3F` ControllerNode application block (`DamperTarget`/`DamperActual`/`DamperMode` occupy `0x30`–`0x32`). Same reporting model as every other endpoint (`Node-Message-Model-Spec.md` §6.1): `ControllerNode` `Report`s it on change + keepalive so the server/operator can see it; `MainController` `Set`s it, addressed to the specific node id (not broadcast — each node's budget differs).

### 3.2 New `INodeHandler` hook: `Snoop()`

`ControllerNode` needs to see `TemperatureNode`'s `SupplyTemp` `Report`, but that frame is not addressed to it — `Node::HandleMessage` (`Node.cpp`) drops any frame where `m.id.node != nodeId` and it isn't a broadcast `Set` (`Node-Message-Model-Spec.md` §2's address rule). On a shared half-duplex RS485 bus every node's transceiver physically receives every frame anyway, so this is a software drop, not a hardware limitation. Add one more optional hook, same default-no-op pattern as `PrepareForReset`/`FillStatus`:

```cpp
// INodeHandler.h
virtual void Snoop(const Message&) {}  // every frame this node's UART sees,
                                        // regardless of address match -- for
                                        // modules that need to observe another
                                        // node's traffic, not just their own
```

Called from `Node::HandleMessage()`, unconditionally, before the address filter:

```cpp
// Node.cpp, top of HandleMessage()
if (handler) { handler->Snoop(m); }
```

Master-side (`NodeMaster`/`MainController`) needs no such hook — it already receives every frame off every node's poll window through the ordinary `ReceivedMessage()` path, since it is what's driving the round-robin.

---

## 4. `ControllerNode`: `SupplyTemp` cache + `RoomControlLoop`

### 4.1 `SupplyTemp` — the snoop cache

`Lib/NodeLib/SupplyTemp.h`, shared with `MainController`'s `BudgetAllocator` (§5.1) so both sides drop a stale reading on the same timeout.

```cpp
// Learns the shared duct SupplyTemp by passively observing whichever
// TemperatureNode reports it as it passes on the bus (Snoop(), §3.2) -- not
// tied to a specific TemperatureNode id, matching TemperatureNode-Spec.md §1's
// single shared duct sensor.
class NodeLib::SupplyTemp
{
  public:
    SupplyTemp();

    void Snoop(const Message& m);            // no-op unless Report SupplyTemp
    void Loop();                             // ages out after staleTimeoutMs

    bool    Valid() const;
    int16_t CentiDegC() const;

  private:
    static const uint32_t staleTimeoutMs = 5 * 60 * 1000;

    int16_t           value;
    bool               valid;
    Tools::DelayTimer staleTimer;
};
```

### 4.2 `RoomControlLoop` — the actual room control loop

```cpp
// The room's control loop (ControllerNode-Thermostat-Link-Spec.md §6 item 1,
// resolved: yes, it runs here). Drives Damper::SetTarget() from
// ThermostatLink's Room* cache (setpoint/roomTemp) and SupplyTemp, kept inside
// the [min, max] range of whatever DamperBudget MainController last set. Only
// active while Damper::GetMode() == Auto -- Closed/Open/Manual are explicit
// overrides this loop never touches, budget included.
class RoomControlLoop
{
  public:
    RoomControlLoop(ThermostatLink& thermostatLink, const NodeLib::SupplyTemp& supplyTemp, Damper& damper);

    void Loop();

    void    SetBudget(uint8_t max, uint8_t min = 0);  // from Set DamperBudget; also proves the bus link is alive (cancels the fallback)
    uint8_t BudgetMax() const;
    uint8_t BudgetMin() const;

    void SetDumpRoom(bool dumpRoom);  // persisted DumpRoom flag, loaded by ControllerHandler::Init()
    bool DumpRoom() const;

    void ConnectionLost();  // max ramps back to defaultBudget, min drops to 0 -- or 100 on the dump room (§4.3)

  private:
    void StepBudgetRamp();
    void ClampIntoRange();  // moves the damper into [min, max] at once if it sits outside

    static const uint8_t  defaultBudget    = 50;
    static const uint32_t rampIntervalMs   = 36000;  // 1 point / 36s -> 30 min for the worst-case 50-point gap (§4.3)
    static const uint8_t  rampStepPercent  = 1;

    ThermostatLink&            thermostatLink;
    const NodeLib::SupplyTemp& supplyTemp;
    Damper&                    damper;

    uint8_t           budgetMax;
    uint8_t           budgetMin;
    bool              dumpRoom;
    bool              connectionLost;
    Tools::DelayTimer rampTimer;
};
```

```cpp
void RoomControlLoop::Loop()
{
    if (connectionLost)
    {
        StepBudgetRamp();
    }
    if (damper.GetMode() != Damper::Mode::Auto)
    {
        return;
    }

    uint8_t desired = Damper::NeutralPercent;  // no supply reading: the fail-safe position (§4.4)
    if (supplyTemp.Valid())
    {
        if (!thermostatLink.Room().valid)
        {
            ClampIntoRange();  // no room data yet: hold where it is, but still inside the range
            return;
        }
        desired = NodeLib::RoomDemandPercent(supplyTemp.CentiDegC(), thermostatLink.Room().temp, thermostatLink.Room().setpoint);
    }

    uint8_t target = desired < budgetMax ? desired : budgetMax;
    target         = target > budgetMin ? target : budgetMin;
    if (target != damper.Target())  // SetTarget() (re)starts a move -- repeating an unchanged target every
    {                               // pass would keep the settle timer from running out and a stall from confirming
        damper.SetTarget(target);
    }
}

void RoomControlLoop::SetBudget(const uint8_t max, const uint8_t min)
{
    budgetMin      = min > 100 ? 100 : min;
    budgetMax      = max > 100 ? 100 : max;
    budgetMax      = budgetMax > budgetMin ? budgetMax : budgetMin;  // a min above the max wins
    connectionLost = false;
    rampTimer.Stop();
    ClampIntoRange();  // at once, not just on the next Loop() tick -- gated on Auto inside, so a
                       // technician's Manual override is never silently moved
}
```

Wiring into `ControllerHandler`:
- New members: `SupplyTemp supplyTemp; RoomControlLoop roomControlLoop;`.
- `Snoop()` override forwards to `supplyTemp.Snoop(m)`.
- `HandleDamper()`'s endpoint switch gains a `DamperBudget` case: `Get` reports `{BudgetMax(), BudgetMin()}`, `Set` calls `roomControlLoop.SetBudget(m.data[0], m.len >= 2 ? m.data[1] : 0)`. The published value carries both bytes, so a change to either is reported.
- A `DumpRoom` case: `Set` (0 or 1, else `Nack`) persists the flag in the node's settings page (`NodeLib::SettingsPage`, `Node-Flash-Layout-and-Bootloader-Spec.md` §3) and answers with a `Report` of it; `Get` reports it. `Init()` loads it once at start-up. The flag is published, which is how `MainController` learns which room is the dump room.
- `ConnectionLost()` also calls `roomControlLoop.ConnectionLost()`.
- `Loop()` calls `supplyTemp.Loop()` and `roomControlLoop.Loop()` before `damper.Loop()` (decide the target, then let `Damper` step the actual move).

### 4.3 Disconnect ramp

While the main-bus connection is lost, a node's budget is not stuck wherever `MainController` last left it — it drifts back toward the `defaultBudget` (50%) over **30 minutes**, so a node cut off from `MainController` (bus fault, `MainController` reset, uplink-independent — this is main-bus liveness, not the uplink) eventually returns to sane, un-arbitrated behavior instead of staying pinned at whatever the last allocation was, possibly 0. Budget can only ever be 30–50 points from default in either direction (`[0,100]` range around a 50 default), so a fixed 1-point/36s step bounds the worst case at exactly 30 minutes; a smaller gap closes sooner. The ramp is cancelled — not reset, just stopped where it is — the moment a fresh `Set DamperBudget` arrives (`SetBudget()`, §4.2), which is what makes a live `MainController` connection self-evident without a separate liveness endpoint. This requires `MainController` to periodically re-`Set` `DamperBudget` for every online node even when unchanged (§5.3), on the same order as the existing keepalive cadence (`Node-Message-Model-Spec.md` §6.1) — otherwise a node whose fair allocation never changes would never learn its connection is still up.

The budget **min** does not ramp. On losing the bus it drops to 0 at once, except on the dump room (§5.5), where it goes to 100 and stays there — with `MainController` gone nobody is watching the total opening any more, so the dump room makes sure the unit's air still has somewhere to go. The dump room's max is held up by that min and does not ramp down. The next `DamperBudget` Set from `MainController` replaces both.

### 4.4 No supply reading — fail safe to 50%

Without a valid `SupplyTemp` — the TemperatureNode is off the bus, or its supply probe failed (`DuctChannel` then stops reporting it and raises `SupplyTempSensorFault`), for `staleTimeoutMs` — no room's demand can be judged. The fail-safe is the building running as it would with no control at all: every damper at 50%. It is reached gradually and never above the budget:

- **`ControllerNode`:** in `Auto`, `desired` becomes `Damper::NeutralPercent` (50%), whatever the room or thermostat state, and the damper goes to `desired` clamped into `[min, max]` as always.
- **`MainController`:** `Recompute()` stops allocating and instead moves each online node's budget one point toward 50% per recompute (`budgetStepPercent`, every 30 s — back at 50% within 25 minutes), starting from the node's own `DamperBudget` report, else what it last sent, else 50%. A node below 50% opens up as its budget rises; one above 50% stays at 50%.
- **`MainController` gone too:** the disconnect ramp (§4.3) moves the budget to 50% the same way.

A single failed probe therefore never leaves the building with every damper at its minimum stop, and never jumps a closed-down room wide open. When the supply reading returns, the next recompute allocates by demand again.

---

## 5. `MainController`: `BudgetAllocator`

### 5.1 Shape

```cpp
// Divides a shared airflow-budget pool across every online ControllerNode.
// Never a room-level loop (MainController-Spec.md §2) -- this only
// narrows what each ControllerNode's own RoomControlLoop is permitted to do.
// MainController is already bus master, so Observe() needs no extra bus
// traffic -- it just reads what UplinkHandler already sees relayed to it.
class BudgetAllocator
{
  public:
    explicit BudgetAllocator(NodeLib::NodeMaster& master);

    void Loop();                               // recomputes every recomputeIntervalMs
    void Observe(const NodeLib::Message& m);   // fed from UplinkHandler::ReceivedMessage()

  private:
    struct SRoom
    {
        bool    known;      // temp + setpoint both reported
        int16_t temp;
        int16_t setpoint;
        uint8_t budget;     // the node's DamperBudget max -- its own Report, else what was last sent (§4.4)
        uint8_t mode;       // its DamperMode Report -- Auto until one arrives
        uint8_t target;     // its DamperTarget Report
        bool    dumpRoom;   // its DumpRoom Report
    };

    void    Recompute();
    void    StepBudgetsTowardDefault(const uint8_t* onlineIds, uint8_t onlineCount, uint8_t* maxes);  // §4.4
    void    AssignMinimumsAndSend(const uint8_t* onlineIds, uint8_t onlineCount, const uint8_t* maxes);  // §5.5
    uint8_t Unfloored(uint8_t nodeId, uint8_t max) const;  // §5.5
    int32_t SurplusRank(uint8_t nodeId) const;              // §5.5
    void    SendBudget(uint8_t nodeId, uint8_t max, uint8_t min);

    static const uint8_t  defaultBudgetPerNode = 50;   // pool = defaultBudgetPerNode * onlineCount
    static const uint16_t minTotalOpenPercent  = 200;  // §5.5
    static const uint32_t recomputeIntervalMs  = 30000;
    static const uint8_t  budgetStepPercent    = 1;    // per recompute, without a supply reading (§4.4)

    NodeLib::NodeMaster& master;
    NodeLib::SupplyTemp   supplyTemp;                // §4.1 -- same staleTimeoutMs as every ControllerNode
    SRoom                 room[NodeLib::MAX_NODES];  // indexed by nodeId - 1 (nodes 1..MAX_NODES)
    Tools::DelayTimer     recomputeTimer;
};
```

`Observe()` caches `SupplyTemp`, `RoomTemp`, `RoomSetpoint` and `DamperBudget` `Report`s by source node id — no new bus traffic, just watching what already flows through `UplinkHandler`. A node that doesn't take part in an allocation (§5.2 step 1) has its cached room data cleared on that pass, so a node that drops off the bus and rejoins starts from fresh reports rather than its pre-loss demand; until they arrive (a node re-reports every value once the master polls it again, `Node-Message-Model-Spec.md` §6) its room is unknown and its weight 0.

### 5.2 Allocation — `Recompute()`

1. `pool = 50 * onlineCount` (`onlineCount` = nodes with `NodeMaster::NodeModule(id) == ControllerNode` that are `NodeActive()` and not `NodeInBootloader()`). `NodeMaster` keeps a lost node's module type, so the active check is what drops it; a node in its bootloader is excluded because it can't act on a budget. A node leaves the allocation at the next recompute (§5.3) after it is lost.
   Without a valid supply reading the steps below are skipped: each online node's budget is stepped toward 50% instead (§4.4).
2. `weight[i] = RoomDemandPercent(supplyTemp, room[i].temp, room[i].setpoint)` per online node (§2) — 0 for a room the current supply air can't help, exactly matching *"if the supply air is warm but setpoint is asking for cooling, budget can be low."*
3. `weightSum == 0` (nobody has any demand) → split the pool evenly: `pool / onlineCount == 50` each — lands exactly back on the default, no special-casing needed.
4. Otherwise `raw[i] = pool * weight[i] / weightSum`.
5. **Water-fill** any node whose `raw[i]` would exceed 100: clamp it at 100, redistribute the excess proportionally by weight among the still-open nodes. Bounded to at most `onlineCount` passes (each pass clamps at least one more node, so this always terminates in a provable, small number of iterations — not an unbounded `while`, deliberately, given this runs on an 8 KB-RAM MCU).
6. Size each node's min (§5.5), then `SendBudget(nodeId, max[i], min[i])` for every online node, via `master.QueueMessage(Id(nodeId, Endpoint::DamperBudget, Operation::Set), {max, min})` — the same mechanism `UplinkHandler` already uses for uplink→bus relay. Without a supply reading (step 1) the stepped maxes go through the same min sizing.

### 5.3 Recompute cadence doubles as the ramp-cancelling keepalive

`recomputeIntervalMs` (30s, tentative — §7) re-sends every online node's budget on every tick, even when unchanged, which is what lets `RoomControlLoop::SetBudget()` (§4.3) use "I got a fresh budget" as its bus-liveness signal without a separate heartbeat-forwarding path.

### 5.4 Wiring

- `UplinkHandler::ReceivedMessage()` gains one line: `budgetAllocator.Observe(message)`, alongside its existing relay-queue staging.
- `main.cpp` ticks `budgetAllocator.Loop()` next to `master.Loop()` / `uplink.Loop()`.

---

### 5.5 Minimum total opening — the budget min and the dump room

The HVAC unit's fan cannot be controlled: it always runs, and its air has to go somewhere. Left to themselves, the room loops would close every damper whenever every room is satisfied, which is an ordinary situation. The actuator's 0 % end point is only a couple of degrees short of closed (`Damper-Mechanics-Spec.md` §3.1), so that would leave the unit blowing against a nearly closed duct system: high static pressure, noise at the dampers, and too little air across the unit's coil. The house has one uncontrolled branch that is always open, which carries part of the air; the dampers have to keep enough open on top of it.

So after the maxes are allocated, `AssignMinimumsAndSend()` checks the total:

1. **Where each damper would sit without a min** (`Unfloored()`): for a node in `Auto`, what its room loop would choose, `min(RoomDemandPercent, max)` (or `min(50, max)` without a supply reading, §4.4), and 0 while its room data is unknown. A node in `Closed`/`Open`/`Manual` counts its commanded `DamperTarget` and takes no min, since those modes are explicit overrides. A stalled node counts as 0.
2. **Total** = the sum over all online ControllerNodes. If it is at least `minTotalOpenPercent`, every min is 0.
3. **Otherwise the shortfall is handed out as mins**, filling one room to 100 % before the next takes any, in `SurplusRank()` order:
   - the **dump room** first — the ControllerNode whose `DumpRoom` flag is set, chosen at installation as the room that minds extra air least (a hallway, say);
   - then the other `Auto` rooms, starting with the one the extra air harms least: the one that is least far past its setpoint in the direction the supply air pushes it (a room that still wants the air comes first, one already overshooting comes last). Rooms with nothing to judge by go last, in id order.
4. Each node is sent its `[min, max]`. A min above the room's fair-share max is sent as it is; the node lets the min win.

`minTotalOpenPercent` is a sum of damper positions, in percent: 100 means the equivalent of one damper fully open, on top of the uncontrolled branch. It is set to **200**, two dampers' worth. It is a compile-time constant, to be confirmed to the unit's minimum airflow (its installation manual gives a minimum air volume or a maximum external static pressure). Damper airflow is not linear in position — a butterfly blade at 20 % passes noticeably more than 20 % of its full flow — so the sum is a conservative measure.

The minimum is enforced from `MainController`, so the node-side fallback (§4.3) covers the time it is gone: the dump room opens fully on its own.

---

## 6. Why percent, not degrees or raw PWM

Considered and rejected: exposing the servo's native `0–180°` range (or raw PWM pulse width) through this design instead of percent.

- **The control math already runs at full resolution internally.** `RoomDemandPercent()` computes in centi-°C (0.01 °C, the same resolution `RoomTemp`/`SupplyTemp` carry on the wire) through the whole deadband/scaling calculation; only the final value handed to `Damper::SetTarget()` or `DamperBudget` rounds to `0..100`. Quantizing the *output* doesn't lose anything upstream of that rounding.
- **Percent is already the locked wire unit for every other Damper endpoint** (`DamperTarget`/`DamperActual`/`DamperMode`, `Node-Message-Model-Spec.md` §5's "Percent: uint8, 0–100" convention, enforced today by `Damper::Clamp100()`). `DamperBudget` reusing it keeps every value `RoomControlLoop` compares (`desired < budget`, `damper.Target() > budget`) in the same unit.
- **100 steps over 180° ≈ 1.8°/step is already finer than what matters mechanically** — a geared servo's own backlash, and a damper's nonlinear (butterfly-valve-like) airflow-vs-angle curve, both dwarf 1.8° of positioning error. Room thermal time constants are minutes; nothing here is fighting for a fraction of a degree.
- If finer actuator resolution is ever wanted, it belongs entirely inside `Damper`'s percent→PWM-pulse-width mapping (`Damper::PulseFor()`, linear over 500–2500 µs) — the wire protocol, `RoomControlLoop`, and `BudgetAllocator` would never need to change.

---

## 7. Open items

1. **Tunable constants are defaults, not bench-validated:** `roomDeadbandCentiC = 30` (0.3 °C), `fullAuthorityCentiC = 300` (3 °C), `SupplyTemp::staleTimeoutMs = 5 min`, `BudgetAllocator::recomputeIntervalMs = 30 s`. Revisit once real thermostats/dampers are on a bench.
2. **`BudgetAllocator` keeps no state across a `MainController` reset** — it recomputes fresh from whatever `Report`s arrive after reboot; a node's own 30-minute disconnect ramp (§4.3) covers the gap while `MainController` is down, so this is believed fine, not re-litigated here.
3. **`minTotalOpenPercent = 200` is not yet checked against the unit** (§5.5) — confirm it against the HVAC unit's minimum airflow and the measured flow of the uncontrolled branch once the installation is known.

`RoomDemandPercent`'s linear scale (§2) and `BudgetAllocator`'s water-fill (§5.2) both round to nearest rather than floor — a plain `/` would understate every value by up to a point, compounding across the proportional split and the redistribution pass.
