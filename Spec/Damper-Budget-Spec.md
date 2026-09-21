# Damper Budget & Room Control Loop — Design Spec

**Status:** Draft — design locked 2026-09-21, not yet implemented (see §7)
**Companion docs:** `ControllerNode-Thermostat-Link-Spec.md` (resolves that spec's §6 open item 1 — the room loop lives on `ControllerNode`), `Node-Message-Model-Spec.md` (the `DamperBudget` endpoint, §3 here), `MainController-Spec.md` (§2 there — this is the "supervisor" role, not a room-level closed loop), `TemperatureNode-Spec.md` (`SupplyTemp`/`ReturnTemp` source)

---

## 1. Split of responsibility

- **`ControllerNode` runs the room's control loop.** This resolves `ControllerNode-Thermostat-Link-Spec.md` §6 open item 1: yes, `ControllerNode` compares its own Thermostat's setpoint/room-temp against the shared duct supply-air temperature and drives its own damper — `MainController` is never in that loop.
- **`MainController` runs a fleet-wide fairness arbitration on top**, not a room loop — consistent with `MainController-Spec.md` §4 item 2 ("supervisor + bridge/logger, not a closed-loop controller"). It watches every node's `Room*`/`SupplyTemp` traffic (it already sees all of it, being bus master) and periodically narrows what each `ControllerNode`'s loop is *permitted* to do, by sending it a `DamperBudget` percent — a ceiling, never a target.
- Each `ControllerNode` still decides locally where inside its current budget to sit; it does not always run at the budget ceiling.

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

- `ControllerNode`'s `RoomControlLoop` uses it as the **desired damper position**, capped by the node's current budget.
- `MainController`'s `BudgetAllocator` uses it as the **weight** for splitting the budget pool across nodes.

Both consumers compute in centi-°C (0.01 °C resolution, the same unit `RoomTemp`/`RoomSetpoint`/`SupplyTemp` already use on the wire) all the way through; only the final `uint8_t` handed to `Damper::SetTarget()` or the `DamperBudget` endpoint rounds down to a percent. Percent is not a precision loss here — see §6.

---

## 3. Wire changes (`Node-Message-Model-Spec.md`)

### 3.1 New endpoint

```cpp
DamperBudget = 0x33,  // RW  uint8 %  -- ceiling on DamperTarget while DamperMode == Auto
```

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

```cpp
// Learns the shared duct SupplyTemp by passively observing whichever
// TemperatureNode reports it as it passes on the bus (Snoop(), §3.2) -- not
// tied to a specific TemperatureNode id, matching TemperatureNode-Spec.md §1's
// single shared duct sensor.
class SupplyTemp
{
  public:
    SupplyTemp();

    void Snoop(const NodeLib::Message& m);  // no-op unless Report SupplyTemp
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
// ThermostatLink's Room* cache (setpoint/roomTemp) and SupplyTemp, capped by
// whatever DamperBudget MainController last set. Only active while
// Damper::GetMode() == Auto -- Closed/Open/Manual are explicit overrides this
// loop never touches, budget included.
class RoomControlLoop
{
  public:
    RoomControlLoop(ThermostatLink& thermostatLink, const SupplyTemp& supplyTemp, Damper& damper);

    void Loop();

    void    SetBudget(uint8_t percent);  // from Set DamperBudget; also proves the bus link is alive (cancels the ramp)
    uint8_t Budget() const;

    void ConnectionLost();  // starts the ramp back to defaultBudget (§4.3)

  private:
    void StepBudgetRamp();

    static const uint8_t  defaultBudget    = 50;
    static const uint32_t rampIntervalMs   = 36000;  // 1 point / 36s -> 30 min for the worst-case 50-point gap (§4.3)
    static const uint8_t  rampStepPercent  = 1;

    ThermostatLink&   thermostatLink;
    const SupplyTemp& supplyTemp;
    Damper&           damper;

    uint8_t           budget;
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
    if (damper.GetMode() != Damper::Mode::Auto || !thermostatLink.Room().valid)
    {
        return;
    }

    const uint8_t desired = supplyTemp.Valid()
        ? NodeLib::RoomDemandPercent(supplyTemp.CentiDegC(), thermostatLink.Room().temp, thermostatLink.Room().setpoint)
        : 0;  // no supply reading -- don't guess, stay closed
    damper.SetTarget(desired < budget ? desired : budget);
}

void RoomControlLoop::SetBudget(const uint8_t percent)
{
    budget         = percent > 100 ? 100 : percent;
    connectionLost = false;
    rampTimer.Stop();
    if (damper.Target() > budget)
    {
        damper.SetTarget(budget);  // re-clamp immediately, not just on the next Loop() tick
    }
}
```

Wiring into `ControllerHandler`:
- New members: `SupplyTemp supplyTemp; RoomControlLoop roomControlLoop;`.
- `Snoop()` override forwards to `supplyTemp.Snoop(m)`.
- `HandleDamper()`'s endpoint switch gains a `DamperBudget` case: `Get` reports `roomControlLoop.Budget()`, `Set` calls `roomControlLoop.SetBudget(m.data[0])`.
- `ConnectionLost()` also calls `roomControlLoop.ConnectionLost()`.
- `Loop()` calls `supplyTemp.Loop()` and `roomControlLoop.Loop()` before `damper.Loop()` (decide the target, then let `Damper` step the actual move).

### 4.3 Disconnect ramp

While the main-bus connection is lost, a node's budget is not stuck wherever `MainController` last left it — it drifts back toward the `defaultBudget` (50%) over **30 minutes**, so a node cut off from `MainController` (bus fault, `MainController` reset, uplink-independent — this is main-bus liveness, not the uplink) eventually returns to sane, un-arbitrated behavior instead of staying pinned at whatever the last allocation was, possibly 0. Budget can only ever be 30–50 points from default in either direction (`[0,100]` range around a 50 default), so a fixed 1-point/36s step bounds the worst case at exactly 30 minutes; a smaller gap closes sooner. The ramp is cancelled — not reset, just stopped where it is — the moment a fresh `Set DamperBudget` arrives (`SetBudget()`, §4.2), which is what makes a live `MainController` connection self-evident without a separate liveness endpoint. This requires `MainController` to periodically re-`Set` `DamperBudget` for every online node even when unchanged (§5.3), on the same order as the existing keepalive cadence (`Node-Message-Model-Spec.md` §6.1) — otherwise a node whose fair allocation never changes would never learn its connection is still up.

---

## 5. `MainController`: `BudgetAllocator`

### 5.1 Shape

```cpp
// Divides a shared airflow-budget pool across every online ControllerNode.
// Never a room-level loop (MainController-Spec.md §4 item 2) -- this only
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
        bool    known;
        int16_t temp;
        int16_t setpoint;
    };

    void Recompute();
    void SendBudget(uint8_t nodeId, uint8_t percent);

    static const uint8_t  defaultBudgetPerNode = 50;   // pool = defaultBudgetPerNode * onlineCount
    static const uint32_t recomputeIntervalMs  = 30000;

    NodeLib::NodeMaster& master;
    int16_t               supplyTemp;
    bool                  supplyValid;
    SRoom                 room[NodeLib::MAX_NODES];  // indexed by nodeId
    Tools::DelayTimer     recomputeTimer;
};
```

`Observe()` caches `SupplyTemp`, `RoomTemp`, `RoomSetpoint` `Report`s by source node id — no new bus traffic, just watching what already flows through `UplinkHandler`.

### 5.2 Allocation — `Recompute()`

1. `pool = 50 * onlineCount` (`onlineCount` = active nodes with `NodeMaster::NodeModule(id) == ControllerNode`).
2. `weight[i] = RoomDemandPercent(supplyTemp, room[i].temp, room[i].setpoint)` per online node (§2) — 0 for a room the current supply air can't help, exactly matching *"if the supply air is warm but setpoint is asking for cooling, budget can be low."*
3. `weightSum == 0` (nobody has any demand) → split the pool evenly: `pool / onlineCount == 50` each — lands exactly back on the default, no special-casing needed.
4. Otherwise `raw[i] = pool * weight[i] / weightSum`.
5. **Water-fill** any node whose `raw[i]` would exceed 100: clamp it at 100, redistribute the excess proportionally by weight among the still-open nodes. Bounded to at most `onlineCount` passes (each pass clamps at least one more node, so this always terminates in a provable, small number of iterations — not an unbounded `while`, deliberately, given this runs on an 8 KB-RAM MCU).
6. `SendBudget(nodeId, budget[i])` for every online node, via `master.QueueMessage(Id(nodeId, Endpoint::DamperBudget, Operation::Set), budget)` — the same mechanism `UplinkHandler` already uses for uplink→bus relay.

There is deliberately **no software floor** — a floor was considered (originally 20%) and dropped: the servo has a hardware mechanical stop that guarantees some minimum ventilation regardless of the commanded position, so a software floor would just be redundant with what the actuator already guarantees.

### 5.3 Recompute cadence doubles as the ramp-cancelling keepalive

`recomputeIntervalMs` (30s, tentative — §7) re-sends every online node's budget on every tick, even when unchanged, which is what lets `RoomControlLoop::SetBudget()` (§4.3) use "I got a fresh budget" as its bus-liveness signal without a separate heartbeat-forwarding path.

### 5.4 Wiring

- `UplinkHandler::ReceivedMessage()` gains one line: `budgetAllocator.Observe(message)`, alongside its existing relay-queue staging.
- `main.cpp` ticks `budgetAllocator.Loop()` next to `master.Loop()` / `uplink.Loop()`.

---

## 6. Why percent, not degrees or raw PWM

Considered and rejected: exposing the servo's native `0–180°` range (or raw PWM pulse width) through this design instead of percent.

- **The control math already runs at full resolution internally.** `RoomDemandPercent()` computes in centi-°C (0.01 °C, the same resolution `RoomTemp`/`SupplyTemp` carry on the wire) through the whole deadband/scaling calculation; only the final value handed to `Damper::SetTarget()` or `DamperBudget` rounds to `0..100`. Quantizing the *output* doesn't lose anything upstream of that rounding.
- **Percent is already the locked wire unit for every other Damper endpoint** (`DamperTarget`/`DamperActual`/`DamperMode`, `Node-Message-Model-Spec.md` §5's "Percent: uint8, 0–100" convention, enforced today by `Damper::Clamp100()`). `DamperBudget` reusing it keeps every value `RoomControlLoop` compares (`desired < budget`, `damper.Target() > budget`) in the same unit.
- **100 steps over 180° ≈ 1.8°/step is already finer than what matters mechanically** — a geared servo's own backlash, and a damper's nonlinear (butterfly-valve-like) airflow-vs-angle curve, both dwarf 1.8° of positioning error. Room thermal time constants are minutes; nothing here is fighting for a fraction of a degree.
- If finer actuator resolution is ever wanted, it belongs entirely inside `Damper::SetTarget()`'s (currently still-TODO) percent→PWM-pulse-width mapping — the wire protocol, `RoomControlLoop`, and `BudgetAllocator` would never need to change.

---

## 7. Open items

1. **Tunable constants are defaults, not bench-validated:** `roomDeadbandCentiC = 30` (0.3 °C), `fullAuthorityCentiC = 300` (3 °C), `SupplyTemp::staleTimeoutMs = 5 min`, `BudgetAllocator::recomputeIntervalMs = 30 s`. Revisit once real thermostats/dampers are on a bench.
2. **`DamperBudget` only constrains `Damper::Mode::Auto`.** A technician's `Manual`/`Open`/`Closed` override is never silently reclamped by a budget update. Confirm this is the intended scope before it ships.
3. **`BudgetAllocator` keeps no state across a `MainController` reset** — it recomputes fresh from whatever `Report`s arrive after reboot; a node's own 30-minute disconnect ramp (§4.3) covers the gap while `MainController` is down, so this is believed fine, not re-litigated here.
4. **Rounding in the water-fill (§5.2) and in `RoomDemandPercent`'s linear scale** — integer division rounds down throughout; whether that should round-to-nearest instead is a small correctness detail, flagged but not decided.
