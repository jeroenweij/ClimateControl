# Node Message Model — Endpoints & Operations

**Companion docs:** `RS485-Node-Protocol-Spec-STM32G030.md` (frame/CRC/framing), `Node-Flash-Layout-and-Bootloader-Spec.md` (the `Firmware` endpoint), `ControllerNode-Thermostat-Link-Spec.md` (the link reuses this model, §7 here), `Software-Architecture-Spec.md`

---

## 1. Why an endpoint model

A node is not an I/O expander — a `ControllerNode` runs a damper loop and caches room state from a Thermostat link, a `TemperatureNode` reads two duct sensors, and every node needs firmware updates and status/reset control. The addressing question is **which named thing on the node is this message for**: its damper target, a duct temperature, its firmware, its status — not "which pin."

So the message header's middle field is **`endpoint`**: a flat enum of the addressable things on a node, with real names, organised in blocks (high nibble = block) so it stays disciplined. `operation` is a small verb set.

---

## 2. The model

```
        ┌── who ──┐  ┌──── which thing ────┐  ┌── what action ──┐  ┌─ value ─┐
Message:  node (1B)     endpoint (1B)          operation (1B)       data[0..LEN-1]
```

Header size and layout match `RS485-Node-Protocol-Spec-STM32G030.md` §3.

- **`node`** — physical bus address. `0` = master (reserved). `1 .. MAX_NODES-1` = slaves. `0xFF` = broadcast, valid only with `operation = Set` — a fire-and-forget command to every node, no reply (so no bus contention). Used for emergency safe-state (`Set DamperMode`) and time sync; `Discover` also carries `node = 0xFF`. A broadcast `Get` is invalid — nodes ignore it. Each node's dispatch accepts a frame when `node == ownId || node == 0xFF`.
- **`endpoint`** — the addressable thing (§3). Meaning of the application-block endpoints (`0x3_`) depends on the node's module type — a `ControllerNode` and a `TemperatureNode` use different endpoints in that block, no overlap.
- **`operation`** — a small flat verb enum (§4): `Get / Set / Report / Ack / Nack` for the RPC endpoints, plus `Discover / Announce / Poll / Done` for the `Transport` endpoint.
- **`data`** — value bytes, interpreted per `endpoint` (+ `operation`). `Get` usually carries no payload; `Set`/`Report` carry the value. The `Firmware` endpoint uses `data[0]` as its own sub-opcode (`FirmwareOp`).

---

## 3. Endpoint catalog (`Endpoint`)

```cpp
enum class Endpoint : uint8_t
{
    Transport      = 0x00,  // operation field carries Discover / Announce / Poll / Done

    // 0x1_  system — every node; NodeLib supplies the handler
    SystemInfo     = 0x10,  // RO  module, hwRev, fwVersion, uid[12]
    SystemStatus   = 0x11,  // RO  state, uptimeSec, errorFlags, resetCause  (also pushed whenever state/errorFlags change)
    SystemControl  = 0x12,  // WO  Set: 1=reset->app  2=reset->bootloader  3=identify(seconds)

    // 0x2_  firmware — every node; data[0] = FirmwareOp (Begin/Write/End/Activate/Abort/Status)
    Firmware           = 0x20,
    // ControllerNode only: relay an image to the paired Thermostat over the link
    // (ControllerNode-Thermostat-Link-Spec.md §5.4). Same FirmwareOp sub-opcodes;
    // app-delivered, not NodeLib-handled (§6.2).
    ThermostatFirmware = 0x22,

    // 0x3_  application, ControllerNode
    DamperTarget   = 0x30,  // RW  uint8 %  -- a Set also switches DamperMode to Manual (an explicit position is a manual override in any mode)
    DamperActual   = 0x31,  // RO  uint8 %
    DamperMode     = 0x32,  // RW  enum: 0 closed 1 open 2 auto 3 manual; 4 stalled (RO fault code, Set never accepts it)
    DamperBudget   = 0x33,  // RW  uint8 %  -- ceiling on DamperTarget while DamperMode == Auto, set by MainController (Damper-Budget-Spec.md)

    // 0x3_  application, TemperatureNode
    SupplyTemp     = 0x38,  // RO  int16 centi-degC
    ReturnTemp     = 0x39,  // RO  int16 centi-degC
    SensorStatus   = 0x3A,  // RO  bitfield: per-sensor present/valid

    // 0x4_  room — relayed from the paired Thermostat (ControllerNode only), served from cache
    RoomSetpoint   = 0x40,  // RO / RW*  int16 centi-degC   (*Set = master override, link spec §5.3)
    RoomTemp       = 0x41,  // RO  int16 centi-degC
    RoomHumidity   = 0x42,  // RO  uint16 centi-%RH
    RoomMode       = 0x43,  // RO  enum (same coding as DamperMode)
    RoomLink       = 0x44,  // RO  0 down / 1 up

    // 0x5_  diagnostics — every node; NodeLib-owned (§6)
    DiagRxCounters = 0x50,  // RO  rxFrames(4) crcErrors(4) resyncs(4) interByteTimeouts(4)   -- from Frame
    DiagTxCounters = 0x51,  // RO  txFrames(4) queueDrops(4)                                    -- from Node
    DiagLastError  = 0x52,  // RO  code(1) uptimeAtFault(4) context(2)                           -- from ErrorHandler
    DiagLog        = 0x53,  // RO  Get -> Report the oldest buffered log line: uptimeSec(3 LE) + "<level>: <message>" (≤ 32 chars, so 35 B = MAX_DATA); no text = drained, uptime is "now"; a leading "~ <n> lost" line means the ring overflowed
    DiagReset      = 0x54,  // WO  Set -> clear the counters
};
```

| block | range | on | notes |
|---|---|---|---|
| Transport | `0x00` | all | consumed by `NodeLib` itself — never reaches application code. |
| System | `0x10`–`0x1F` | all | identity, health, reset/bootloader entry, identify-blink. `NodeLib` owns the handler. |
| Firmware | `0x20`–`0x2F` | all | OTA workflow — `Node-Flash-Layout-and-Bootloader-Spec.md` §6.2. `Firmware` (`0x20`) is NodeLib-owned; `ThermostatFirmware` (`0x22`) is app-owned on the ControllerNode (`ControllerNode-Thermostat-Link-Spec.md` §5.4). |
| Application | `0x30`–`0x3F` | `ControllerNode`, `TemperatureNode` | the node's own function; per-module, non-overlapping within the block. |
| Room | `0x40`–`0x4F` | `ControllerNode` | last-known Thermostat state; `Thermostat` itself stays non-addressable (`ControllerNode-Thermostat-Link-Spec.md`). |
| Diagnostics | `0x50`–`0x5F` | all | bus/queue counters, last-error detail, log strings pulled over the bus. `NodeLib`-owned; the app only feeds log lines. |
| Uplink | `0x60`–`0x6F` | `MainController` ↔ server only | roster / presence / time / OTA-relay on the MainController's server link — never on the RS485 bus. Reuses the shared `Endpoint` enum + frame format so one decoder covers both. `MainController-Server-Link-Spec.md` §5. |
| reserved | `0x70`–`0x7F` | | |
| vendor / experimental | `0x80`–`0xFF` | | |

`Endpoint` keeps the house pattern: `enum class Endpoint : uint8_t` with an `operator<<(std::stringstream&, Endpoint)` for logging.

---

## 4. Operation catalog (`Operation`)

Flat enum, no context-dependent values — one `operator<<` table decodes every frame.

| value | `Operation` | Dir | Meaning / payload |
|---|---|---|---|
| `0x01` | `Get` | M→N | Request the endpoint's value. Usually no payload. |
| `0x02` | `Set` | M→N | Write the endpoint. `data` = value bytes. |
| `0x03` | `Report` | N→M | Value delivery — reply to `Get`, or unsolicited / periodic. `data` = value. |
| `0x04` | `Ack` | ↔ | Positive acknowledgement (`data` = optional context byte). |
| `0x05` | `Nack` | ↔ | Rejected / error. `data` = reason code. |
| `0x10` | `Discover` | M→bcast | **Transport.** Enumerate nodes. |
| `0x11` | `Announce` | N→M | **Transport.** Node presence — `data` = module type + 96-bit UID, feeding the master's `UID→NodeId` roster (`Node-Flash-Layout-and-Bootloader-Spec.md` §6.3). |
| `0x12` | `Poll` | M→N | **Transport.** Grant the node its transmit window. |
| `0x13` | `Done` | N→M | **Transport.** End of the node's queued messages. |

The `Firmware` endpoint's richer workflow lives in `data[0]` as a `FirmwareOp` sub-enum, carried with `operation = Set` (M→N) / `Report` (N→M) — the `Operation` verb set never grows per-endpoint.

---

## 5. Value & encoding conventions

- **Endianness:** little-endian for all multi-byte scalars.
- **Temperature:** `int16`, centi-°C (`2143` = 21.43 °C).
- **Percent:** `uint8`, 0–100 (not 0–255).
- **Humidity:** `uint16`, centi-%RH.
- **Enums:** one byte, values as listed per endpoint.
- **Strings** (Diagnostics): not null-terminated — bounded by frame `LEN`.
- **Uptime / timestamps:** `uint32` seconds.

---

## 6. Dispatch & reporting in `NodeLib`

### 6.1 Reporting model

On-change push + slow keepalive + `Get` on demand:

- A node queues a `Report` for an endpoint when its value **changes** (past a per-endpoint deadband — e.g. damper ±1%, temperature ±0.1°C, any enum change). The queued `Report` goes out on the next `Poll`.
- **Keepalive:** each node re-`Report`s its key endpoints on a periodic cadence (~60 s) even if unchanged, so a dropped on-change `Report` self-heals and the master can bound staleness. Staggered across endpoints so one poll isn't oversized.
- The master may `Set`/`Get` any endpoint at any time — `Get` covers cold-start sync and forced refresh; the node answers with a `Report` in its next poll window.
- **Faults:** `NodeLib` pushes `SystemStatus` itself whenever the app's `FillStatus()` `state` / `errorFlags` change (and once at start and after a lost connection), so a fault is visible upstream when it happens, not on the next `Get`. The `errorFlags` bit meanings are per module (`ControllerHandler.h`: bit 0 Thermostat link down, bit 1 damper stalled; `TemperatureHandler.h`: bit 0 return sensor, bit 1 supply sensor), mirrored by the server's `nodelib.FaultNames`.
- `NodeLib` provides the plumbing: a small per-endpoint "dirty" flag + deadband compare the app calls (`Node::PublishIfChanged(endpoint, value)`), the keepalive timer, and the queue.

### 6.2 Dispatch

One handler per node. `NodeLib` intercepts and fully handles three endpoint blocks itself — the module never sees them:

| block | handled by `NodeLib` using |
|---|---|
| `Transport` | the master/slave state machine |
| `System*` | `ConfigStore` (module, uid), the app image descriptor (fwVersion), a `Node` uptime/error tally; `SystemControl` reset via the backup-register handoff in `Node-Flash-Layout-and-Bootloader-Spec.md` §5 |
| `Firmware` (`0x20` only) | the OTA path — app running: persist the enter-bootloader flag + reset; bootloader: the transfer. `ThermostatFirmware` (`0x22`) is **not** intercepted — it reaches the ControllerNode's handler, which relays it over the link (`ControllerNode-Thermostat-Link-Spec.md` §5.4). |
| `Diagnostics*` | counters kept in `Frame`/`Node`, `DiagLastError` from `ErrorHandler`, `DiagLog` drains `Tools::LogRing` — 10 lines × 32 characters (320 B) that every `LOG_*` macro feeds, oldest overwritten first, one line per `Get`, each stamped with the node uptime it was logged at |

The module's handler only ever receives its own application / `Room` endpoints (plus `ThermostatFirmware` on the ControllerNode):

```cpp
class INodeHandler
{
  public:
    virtual void ReceivedMessage(const Message& m) = 0;   // application + Room endpoints
    virtual void ConnectionLost()                  = 0;    // transport liveness

    // Optional hooks -- NodeLib calls these while handling the blocks above.
    virtual void PrepareForReset() {}                       // e.g. park the damper before an OTA reset
    virtual void FillStatus(SystemStatus&) {}               // app-specific state / errorFlags bits
    virtual void Snoop(const Message&) {}                    // every frame this node's UART sees, regardless of address match (Damper-Budget-Spec.md §3.2 -- ControllerNode uses this to learn SupplyTemp from a node's traffic it's not addressed by)
};
```

`Frame` decodes to `Message` with `Id.endpoint` — shape otherwise unchanged.

---

## 7. Interaction with the Thermostat link

The `ControllerNode`↔`Thermostat` point-to-point link (`ControllerNode-Thermostat-Link-Spec.md`) reuses this same `Message` / `Endpoint` / `Operation` model over its own `Frame` framing — a subset, never `Transport` (no arbitration between two fixed endpoints, per that spec §2):

| endpoints on the link | direction | source of truth |
|---|---|---|
| `System*` | either | each end's own status / firmware / reset |
| `Room*` (`RoomSetpoint`, `RoomTemp`, `RoomHumidity`, `RoomMode`) | Thermostat → ControllerNode | **Thermostat** (it has the sensor + UI) |
| `DamperActual`, `DamperMode` | ControllerNode → Thermostat | ControllerNode (for the room display) |

The `ControllerNode` does **not** forward frames between the two buses. Its `Room*` handler answers a main-bus `Get` from the cache its link handler keeps fresh; a main-bus `Set RoomSetpoint` (the master override) is applied by pushing it down the link.

This resolves which endpoint values apply on the link: the named-endpoint ones (`System*`, `Room*`, `Damper*`), not `Transport`.

---

## 8. Open items

1. **Keepalive interval and per-endpoint deadbands** — ~60 s assumed in §6.1; the exact value and the per-endpoint deadbands need confirming once real sensors are on a bench. Not yet built (`Node::PublishIfChanged` and the keepalive timer are still to come).
