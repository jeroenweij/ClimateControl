# Node Message Model — Endpoints & Operations

**Status:** Draft — core decisions locked 2026-09-08 (§9); pending implementation in `NodeLib`. Replaces the IO-expander `ChannelId` model inherited from `~/git/node` with a flat, meaningfully-named **endpoint** model; header stays 3 bytes.
**Companion docs:** `RS485-Node-Protocol-Spec-STM32G030.md` (frame/CRC/framing — unchanged), `Node-Flash-Layout-and-Bootloader-Spec.md` (the `Firmware` endpoint, §6 there), `ControllerNode-Thermostat-Link-Spec.md` (the link reuses this model, §7 here), `Software-Architecture-Spec.md`

---

## 1. Why change

The current `Id` is `{ node, channel, operation }`, where `channel` is `DIGITAL_1..6 / ANALOG_1..3 / SERVO_1..2 / MOSFET_1..2 / INTERNAL_MSG`. That enum is a straight carry-over from the AVR `node` project, where a node genuinely *was* an I/O expander and each channel *was* a physical pin.

This project's nodes are not I/O expanders. A `ControllerNode` runs a damper loop and caches room state from a Thermostat link; a `TemperatureNode` reads two duct sensors; every node needs firmware updates and status/reset control. "Which pin" is the wrong question — the right one is **which named thing on the node is this message for**: its damper target, a duct temperature, its firmware, its status.

So `channel` becomes **`endpoint`** — a flat enum of the addressable things on a node, with real names. The enum is *organised in blocks* (high nibble = block) so it stays disciplined rather than becoming the next grab-bag, but it is still one flat field and one dispatch switch. `operation` is redesigned into a small verb set.

An earlier draft of this spec added a two-level `service` + `object` scheme; dropped 2026-09-08 — the grouping's only load-bearing justification was frame-level routing to the Thermostat link, and that turns out to be cache-based, not frame-forwarding (§7). A wire field that only carries documentation isn't worth the byte or the concept.

---

## 2. The model

```
        ┌── who ──┐  ┌──── which thing ────┐  ┌── what action ──┐  ┌─ value ─┐
Message:  node (1B)     endpoint (1B)          operation (1B)       data[0..LEN-1]
```

Header size and layout are **unchanged** from `RS485-Node-Protocol-Spec-STM32G030.md` §3 — only the middle byte's enum is redefined (`ChannelId` → `Endpoint`) and `operation`'s values change.

- **`node`** — physical bus address. `0` = master (reserved). `1 .. MAX_NODES-1` = slaves. **`0xFF` = broadcast** (decided 2026-09-08): valid **only with `operation = Set`** — a fire-and-forget command to every node, no reply (so no bus contention). Used for emergency safe-state (`Set DamperMode`) and time sync; `Discover` also carries `node = 0xFF`. A broadcast `Get` is invalid — nodes ignore it. Each node's dispatch accepts a frame when `node == ownId || node == 0xFF`.
- **`endpoint`** — the addressable thing (§3). Meaning of the application-block endpoints (`0x3_`) depends on the node's module type — a `ControllerNode` and a `TemperatureNode` use different endpoints in that block, no overlap.
- **`operation`** — a small **flat verb enum** (§4): `Get / Set / Report / Ack / Nack` for the RPC endpoints, plus `Discover / Announce / Poll / Done` for the `Transport` endpoint.
- **`data`** — value bytes, interpreted per `endpoint` (+ `operation`). `Get` usually carries no payload; `Set`/`Report` carry the value. The `Firmware` endpoint uses `data[0]` as its own sub-opcode (`FirmwareOp`).

---

## 3. Endpoint catalog (`Endpoint`, replaces `ChannelId`)

```cpp
enum class Endpoint : uint8_t
{
    Transport      = 0x00,  // operation field carries Discover / Announce / Poll / Done

    // 0x1_  system — every node; NodeLib supplies the handler
    SystemInfo     = 0x10,  // RO  module, hwRev, fwVersion, uid[12]
    SystemStatus   = 0x11,  // RO  state, uptimeSec, errorFlags, resetCause  (also periodic Report)
    SystemControl  = 0x12,  // WO  Set: 1=reset->app  2=reset->bootloader  3=identify(seconds)

    // 0x2_  firmware — every node; data[0] = FirmwareOp (Begin/Write/End/Activate/Abort/Status)
    Firmware           = 0x20,
    // ControllerNode only: relay an image to the paired Thermostat over the link
    // (ControllerNode-Thermostat-Link-Spec.md §5.4). Same FirmwareOp sub-opcodes;
    // app-delivered, not NodeLib-handled (§6.2).
    ThermostatFirmware = 0x22,

    // 0x3_  application, ControllerNode
    DamperTarget   = 0x30,  // RW  uint8 %
    DamperActual   = 0x31,  // RO  uint8 %
    DamperMode     = 0x32,  // RW  enum: 0 closed 1 open 2 auto 3 manual

    // 0x3_  application, TemperatureNode
    SupplyTemp     = 0x38,  // RO  int16 centi-degC
    ReturnTemp     = 0x39,  // RO  int16 centi-degC
    SensorStatus   = 0x3A,  // RO  bitfield: per-sensor present/valid

    // 0x4_  room — relayed from the paired Thermostat (ControllerNode only), served from cache
    RoomSetpoint   = 0x40,  // RO / RW*  int16 centi-degC   (*Set = master override, link spec §5 item 5)
    RoomTemp       = 0x41,  // RO  int16 centi-degC
    RoomHumidity   = 0x42,  // RO  uint16 centi-%RH
    RoomMode       = 0x43,  // RO  enum (same coding as DamperMode)
    RoomLink       = 0x44,  // RO  0 down / 1 up

    // 0x5_  diagnostics — every node; NodeLib-owned (§6)
    DiagRxCounters = 0x50,  // RO  rxFrames(4) crcErrors(4) resyncs(4) interByteTimeouts(4)   -- from Frame
    DiagTxCounters = 0x51,  // RO  txFrames(4) queueDrops(4)                                    -- from Node
    DiagLastError  = 0x52,  // RO  code(1) uptimeAtFault(4) context(2)                           -- from ErrorHandler
    DiagLog        = 0x53,  // RO  Get -> Report next buffered log line (string); empty when drained
    DiagReset      = 0x54,  // WO  Set -> clear the counters
};
```

| block | range | on | notes |
|---|---|---|---|
| Transport | `0x00` | all | consumed by `NodeLib` itself — never reaches application code. Today's `INTERNAL_MSG` + `DETECTNODES/HELLOWORLD/SENDQ/ENDOFQ`. |
| System | `0x10`–`0x1F` | all | identity, health, reset/bootloader entry, identify-blink. `NodeLib` can own the handler. |
| Firmware | `0x20`–`0x2F` | all | OTA workflow — `Node-Flash-Layout-and-Bootloader-Spec.md` §6.2. `Firmware` (`0x20`) is NodeLib-owned; `ThermostatFirmware` (`0x22`) is app-owned on the ControllerNode (`ControllerNode-Thermostat-Link-Spec.md` §5.4). |
| Application | `0x30`–`0x3F` | `ControllerNode`, `TemperatureNode` | the node's own function; per-module, non-overlapping within the block. |
| Room | `0x40`–`0x4F` | `ControllerNode` | last-known Thermostat state; `Thermostat` itself stays non-addressable (`ControllerNode-Thermostat-Link-Spec.md`). |
| Diagnostics | `0x50`–`0x5F` | all | bus/queue counters, last-error detail, log strings pulled over the bus. `NodeLib`-owned (§6); the app only feeds log lines. Confirmed in v1 2026-09-08. |
| Uplink | `0x60`–`0x6F` | `MainController` ↔ server only | roster / presence / time / OTA-relay on the MainController's server link — **never on the RS485 bus**. Reuses the shared `Endpoint` enum + frame format so one decoder covers both. `MainController-Server-Link-Spec.md` §5. Claimed 2026-09-09. |
| reserved | `0x70`–`0x7F` | | |
| vendor / experimental | `0x80`–`0xFF` | | |

`Endpoint` keeps the house pattern: `enum class Endpoint : uint8_t` with an `operator<<(std::stringstream&, Endpoint)` for logging.

---

## 4. Operation catalog (`Operation`, redesigned)

Flat enum, no context-dependent values — one `operator<<` table decodes every frame.

| value | `Operation` | Dir | Meaning / payload |
|---|---|---|---|
| `0x01` | `Get` | M→N | Request the endpoint's value. Usually no payload. |
| `0x02` | `Set` | M→N | Write the endpoint. `data` = value bytes. |
| `0x03` | `Report` | N→M | Value delivery — reply to `Get`, or unsolicited / periodic. `data` = value. |
| `0x04` | `Ack` | ↔ | Positive acknowledgement (`data` = optional context byte). |
| `0x05` | `Nack` | ↔ | Rejected / error. `data` = reason code. Replaces the old `ERROR`. |
| `0x10` | `Discover` | M→bcast | **Transport.** Enumerate nodes. (was `DETECTNODES`) |
| `0x11` | `Announce` | N→M | **Transport.** Node presence — `data` = module type + 96-bit UID, feeding the master's `UID→NodeId` roster (`Node-Flash-Layout-and-Bootloader-Spec.md` §6.3). (was `HELLOWORLD`) |
| `0x12` | `Poll` | M→N | **Transport.** Grant the node its transmit window. (was `SENDQ`) |
| `0x13` | `Done` | N→M | **Transport.** End of the node's queued messages. (was `ENDOFQ`) |

Retired: `SETPWM` → `Set DamperTarget`; `SETMODE` → `Set DamperMode` / `Set RoomMode`; `VALUE` → `Report`; `ERROR` → `Nack` (solicited) or `Report SystemStatus` (async fault).

The `Firmware` endpoint's richer workflow lives in `data[0]` as a `FirmwareOp` sub-enum, carried with `operation = Set` (M→N) / `Report` (N→M) — so the verb set never grows per-endpoint.

---

## 5. Value & encoding conventions

- **Endianness:** little-endian for all multi-byte scalars (matches the wire CRC convention, protocol spec §3).
- **Temperature:** `int16`, centi-°C (`2143` = 21.43 °C).
- **Percent:** `uint8`, 0–100 (not 0–255).
- **Humidity:** `uint16`, centi-%RH.
- **Enums:** one byte, values as listed per endpoint.
- **Strings** (Diagnostics): not null-terminated — bounded by frame `LEN`.
- **Uptime / timestamps:** `uint32` seconds.

---

## 6. Dispatch & reporting in `NodeLib`

### 6.1 Reporting model

**Decided 2026-09-08: on-change push + slow keepalive + `Get` on demand.**

- A node queues a `Report` for an endpoint when its value **changes** (past a per-endpoint deadband — e.g. damper ±1 %, temperature ±0.1 °C, any enum change). The queued `Report` goes out on the next `Poll`.
- **Keepalive:** every ~60 s (open item 4) each node re-`Report`s its key endpoints even if unchanged, so a dropped on-change `Report` self-heals and the master can bound staleness. Staggered across endpoints so one poll isn't oversized.
- The master may `Set`/`Get` any endpoint at any time — `Get` covers cold-start sync and forced refresh; the node answers with a `Report` in its next poll window.
- `NodeLib` provides the plumbing: a small per-endpoint "dirty" flag + deadband compare the app calls (`Node::PublishIfChanged(endpoint, value)`), the keepalive timer, and the queue. The app just calls `PublishIfChanged` whenever it has a new reading.

### 6.2 Dispatch

**Decided 2026-09-08: hybrid single handler.** One handler per node (today's `RegisterHandler` shape). `NodeLib` intercepts and *fully handles* three endpoint blocks itself — the module never sees them:

| block | handled by `NodeLib` using |
|---|---|
| `Transport` | the master/slave state machine (today's `HandleInternalMessage`/`HandleMasterMessage`, ops renamed) |
| `System*` | `ConfigStore` (module, uid), the app image descriptor (fwVersion), a `Node` uptime/error tally; `SystemControl` reset via the backup-register handoff in `Node-Flash-Layout-and-Bootloader-Spec.md` §5 |
| `Firmware` (`0x20` only) | the OTA path — app running: persist the enter-bootloader flag + reset; bootloader: the transfer (that spec §6). `ThermostatFirmware` (`0x22`) is **not** intercepted — it reaches the ControllerNode's handler, which relays it over the link (`ControllerNode-Thermostat-Link-Spec.md` §5.4). |
| `Diagnostics*` | counters kept in `Frame`/`Node`, `DiagLastError` from `ErrorHandler`, `DiagLog` drains a small in-RAM log ring that `Tools::Logger` now writes into (§8) |

The module's handler only ever receives its own application / `Room` endpoints (plus `ThermostatFirmware` on the ControllerNode):

```cpp
class INodeHandler
{
  public:
    virtual void ReceivedMessage(const Message& m) = 0;   // application + Room endpoints
    virtual void ConnectionLost()                  = 0;    // transport liveness (unchanged)

    // Optional hooks -- NodeLib calls these while handling the blocks above.
    virtual void PrepareForReset() {}                       // e.g. park the damper before an OTA reset
    virtual void FillStatus(SystemStatus&) {}               // app-specific state / errorFlags bits
};
```

`Frame` decodes to `Message` with `Id.channel` → `Id.endpoint` (`ChannelId` → `Endpoint`) — shape otherwise unchanged.

**Implemented 2026-09-08** (`Lib/NodeLib/`): the `Endpoint` / `Operation` enums, `Id`/`Message` rename, `INodeHandler` (+ `PrepareForReset`/`FillStatus` stubs), the transport-op renames (`Discover`/`Announce`/`Poll`/`Done`), `node = 0xFF` broadcast delivery, and the `Frame`/`Node` counters (`Node::Counters()` → `DiagCounters`). **Still to build:** the `System*` / `Firmware` / `Diagnostics*` interception + built-in handlers, `Node::PublishIfChanged` + the keepalive timer, and the `Announce` module/UID payload — these land with the bootloader and the first module.

---

## 7. Interaction with the Thermostat link

The `ControllerNode`↔`Thermostat` point-to-point link (`ControllerNode-Thermostat-Link-Spec.md`) reuses this same `Message` / `Endpoint` / `Operation` model over its own `Frame` framing — a **subset**, never `Transport` (no arbitration between two fixed endpoints, per that spec §2):

| endpoints on the link | direction | source of truth |
|---|---|---|
| `System*` | either | each end's own status / firmware / reset |
| `Room*` (`RoomSetpoint`, `RoomTemp`, `RoomHumidity`, `RoomMode`) | Thermostat → ControllerNode | **Thermostat** (it has the sensor + UI) |
| `DamperActual`, `DamperMode` | ControllerNode → Thermostat | ControllerNode (for the room display) |

The `ControllerNode` does **not** forward frames between the two buses. Its `Room*` handler answers a main-bus `Get` from the cache its link handler keeps fresh; a main-bus `Set RoomSetpoint` (if the override is adopted) is applied by pushing it down the link. This is what makes the `service`-as-routing-field argument moot (§1).

This resolves `ControllerNode-Thermostat-Link-Spec.md` §2's open "which `ChannelId`/`Operation` values apply": the named-endpoint ones (`System*`, `Room*`, `Damper*`), not the pin ones, not `Transport`.

---

## 8. Migration & impact

Code (`Software/Lib/NodeLib/`):
- `EChannelId.h` → `EEndpoint.h` — rename type, new values + `operator<<`.
- `EOperation.h` — new values + `operator<<`; retire `SETPWM/VALUE/ERROR/SETMODE/DETECTNODES/HELLOWORLD/SENDQ/ENDOFQ`.
- `Id.h` — `Id{ node, endpoint, operation }`; constructor defaults, `operator<<`.
- `Frame.cpp` — only the `static_cast<ChannelId>` / `static_cast<Operation>` lines; **no structural change**.
- `Node.cpp` / `NodeMaster.cpp` — rename transport ops (`HELLOWORLD`→`Announce`, `SENDQ`→`Poll`, `ENDOFQ`→`Done`, `DETECTNODES`→`Discover`); add the `System*` + `Firmware` interception and the built-in handlers (§6).
- `IVariableHandler.h` → `INodeHandler.h` — add `PrepareForReset()` / `FillStatus()` (both default-empty), keep `ReceivedMessage` / `ConnectionLost`.
- `Announce` payload carries module type + UID — align with `ConfigStore::GetModule()`.
- `Frame` / `Node` — expose the counters behind `DiagRxCounters` / `DiagTxCounters` (`rxFrames`, `crcErrors`, `resyncs`, `interByteTimeouts`, `txFrames`, `queueDrops`).
- `Tools/Logger` — add an in-RAM ring-buffer sink (a few short lines) that `DiagLog` drains; the existing weak/UART sink stays for bench use.

Cross-referenced into the other specs 2026-09-08:
- `RS485-Node-Protocol-Spec-STM32G030.md` §3 (`CHANNEL`→`ENDPOINT` field), §6 (`Operation` row + transport-op renames), §8 (migration notes).
- `Node-Flash-Layout-and-Bootloader-Spec.md` §6.1–6.2 — OTA re-expressed as `Endpoint::Firmware` + `FirmwareOp`.
- `ControllerNode-Thermostat-Link-Spec.md` §1–2 — the "which values apply" row resolved per §7 above.
- `Software-Architecture-Spec.md` §2 / §4 — `Endpoint` naming, `ConfigStore`, `Logger` ring sink.

---

## 9. Open items

**Resolved 2026-09-08:** flat endpoint model, no `service` field (§2); hybrid single handler, `NodeLib` owns `Transport`/`System*`/`Firmware`/`Diagnostics*` (§6.2); `Diagnostics` block in v1 (§3); on-change + keepalive + `Get` reporting model (§6.1); `node = 0xFF` `Set`-only broadcast (§2).

These are detail-level, not blockers for starting the code:

1. **Enum wire values** above are illustrative — fine to renumber on review (e.g. keep `Transport = 0x00` = today's `INTERNAL_MSG` value to shrink the diff).
2. **`DamperMode` / `RoomMode` shared coding** — confirm one mode enum serves both, or they diverge.
3. **`DiagLog` buffer size** — how many lines × how many chars of the 8 KB SRAM budget (`RS485-Node-Protocol-Spec-STM32G030.md` §7)? A tentative 8 × 48 B ≈ 384 B.
4. **Keepalive interval** — ~60 s assumed in §6.1; confirm, and the per-endpoint deadbands.
