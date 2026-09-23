# MainController ↔ Server Link — Design Spec

`MainController` relays `NodeLib` v2 frames verbatim between the RS485 node bus and a single LAN server; the server decodes, stores to SQLite, and serves the operator web application.

**Companion docs:** `MainController-Spec.md` §4 (NINA-W152 hardware), `RS485-Node-Protocol-Spec-STM32G030.md` §3 (frame format, reused on this link), `Node-Message-Model-Spec.md` §3–§5 (endpoints, operations, encodings — the server decodes these), `Node-Flash-Layout-and-Bootloader-Spec.md` §6 (the bus OTA sequence this link feeds).

---

## 1. Role & topology

`MainController` is bus master, supervisor, and a frame bridge to one server:

- **Bus master** — runs the `NodeMaster` discovery + round-robin poll loop.
- **Supervisor** — its own fault detection / safe-state / aggregation logic (`MainController-Spec.md` §2), running independently of the uplink.
- **Bridge** — holds one outbound TCP connection to the server and moves `NodeLib` frames across it in both directions. It does not decode endpoint payloads, hold a schema, or serialise anything; the server owns all persistent state and the UI.

```
   RS485 bus (115200, half-duplex, HW-DE)              LAN (Wi-Fi)
 ┌───────────┐  ┌───────────┐  ┌───────────┐   ┌──────────────┐        ┌──────────────┐
 │Temperature│  │Controller │  │Controller │   │ MainController│  TCP   │    Server    │
 │  Node  3  │──│  Node  5  │──│  Node  7  │───│  NodeMaster   │═══════▶│  decode + DB │
 └───────────┘  └───────────┘  └───────────┘   │  + AT bridge  │        │  + web app   │
                      │  Thermostat link        │  ── NINA ──   │       └──────┬───────┘
                      └── (local, off-bus) ─    └──────────────┘         WebSocket + HTTP
                                                                        ┌──────┴───────┐
                                                                        │   browsers   │
                                                                        └──────────────┘
```

- The socket is **outbound from `MainController`** — no inbound port or firewall exception at the building.
- **Plaintext TCP on the LAN.** Authentication is a 16-byte shared token in the first frame (`UplinkHello`, §5); the server drops any connection whose token does not match.
- The bus runs normally while the uplink is down: nodes are autonomous, each `ControllerNode`↔`Thermostat` loop is local, and the `MainController`'s supervisory logic keeps running. The uplink carries logging, monitoring, and operator control only — never a closed loop.

---

## 2. Properties of frame pass-through

| | |
|---|---|
| STM32 flash | Reuses `Frame` / `Message` / `Id` / `Crc`, already linked. No JSON writer, no schema, no per-endpoint marshalling on the MCU. |
| STM32 SRAM | Working set is one frame in + one frame out (~40 B each) plus the AT line buffer. The MCU holds no model of the building. |
| Evolution | A new endpoint or value type is a server-only change; the bridge carries it with no firmware update. |
| Single decoder | The `Node-Message-Model-Spec` decode lives in exactly one place — the server — and serves both stored history and the live UI. |

The server implements the `NodeLib` wire format (§6); it is a small, fixed, already-specified codec.

---

## 3. Transport — NINA socket

- NINA-W152 on USART2, u-connectXpress AT firmware, 115200 8N1, 4-wire hardware flow control (RTS/CTS per `MainController-Spec.md` §4) — the STM32 must hold `NinaRts` (`PA1`) low or the module never transmits.
- **Wi-Fi join:** `AT+UWSC=0,2,"<ssid>"` / `AT+UWSC=0,5,2` (WPA/WPA2-PSK) / `AT+UWSC=0,8,"<passphrase>"` configure station profile 0; `AT+UWSCA=0,3` activates it. `+UUWLE` (link up) / `+UUNU` (IP up) report readiness; `+UUWLD` / `+UUND` report loss and drive the same reconnect logic as the socket below.
- **Socket:** u-connectXpress models the server as a **peer**, not a BSD-style socket — there is no `+USOCR`/`+USOCO` family on this firmware. `AT+UDCP="tcp://<host>:<port>/"` opens it: `+UDCP:<peer_handle>` on write, then an unsolicited `+UUDPC:<peer_handle>,<type>,<flags>,<local_ip>,<local_port>,<remote_ip>,<remote_port>` once the TCP handshake completes. `ATO` then switches the UART into **data mode** — a transparent byte pipe to the connected peer — for the life of the connection. `+UUDPD:<peer_handle>` reports a close (drops the module back to command mode); `AT+UDCPC=<peer_handle>` closes it explicitly.
- The **AT engine is a non-blocking state machine** ticked from the same super-loop as `master.Loop()`: it issues one command and returns, collecting the response over later iterations. A stalled uplink never delays a bus `Poll` / `Done`.
- **Reconnect:** exponential backoff, 1 s → 30 s cap, re-triggered by `+UUWLD` / `+UUND` / `+UUDPD`. The MCU keeps no uplink send queue — a `Report` produced during an outage is dropped; the value re-reports on its next change or periodic refresh, leaving only a gap in stored history.

---

## 4. Wire format over the socket

Byte-for-byte the RS485 v2 frame (`RS485-Node-Protocol-Spec-STM32G030.md` §3):

```
SYNC(0xEE 0x42) · LEN · NODE · ENDPOINT · OPERATION · DATA[LEN] · CRC16
```

CRC-16/CCITT-FALSE over `NODE..DATA`, little-endian. `LEN` ≤ 35 (`MAX_DATA`, `RS485-Node-Protocol-Spec-STM32G030.md` §7/§9). TCP is a stream, so the server runs the same `Frame` deframer (SYNC scan → LEN → CRC) as a node; a sync loss just resynchronises. No length prefix or envelope is added.

Two frame classes share the link:

| Class | `ENDPOINT` | Meaning |
|---|---|---|
| **Relayed node frame** | `0x10`–`0x5F` | Verbatim copy of a frame on / for the bus. `NODE` is the real node id. Server `Get` / `Set` go onto the bus unchanged; node `Report` / `Ack` / `Nack` go up unchanged. |
| **Uplink frame** | `0x60`–`0x6F` | `MainController` (`NODE = 0`) ↔ server only, never on the RS485 bus. Roster, presence, keepalive, health, OTA control/data (§5). |

Transport verbs (`Discover` / `Announce` / `Poll` / `Done`) are bus-internal and not relayed. Node arrivals and departures reach the server as `NodePresence` (§5).

---

## 5. Uplink endpoint block (`0x60`–`0x6F`)

`NODE = 0`, little-endian per `Node-Message-Model-Spec.md` §5.

| Endpoint | Dir | Op | Payload | Meaning |
|---|---|---|---|---|
| `0x60` `UplinkHello` | MC→S | `Report` | `fwVersion(2) · uptimeSec(4) · nodeCount(1) · authToken(16)` | First frame after every (re)connect. Connection is dropped on token mismatch. `fwVersion = 0` identifies MainController's bootloader (it carries no version of its own; every application image has a non-zero one): the server then shows the MainController as `bootloader` with an unknown version, and treats the bus as unmanaged — every bus node reads offline, exactly as with no uplink at all — while still accepting a firmware push. |
| `0x61` `Roster` | S→MC / MC→S | `Get` / `Report` | Report: `{nodeId(1) · module(1) · state(1) · lastSeenMs(4)}`, one node per frame, `nodeId = 0xFF` terminates | Full active-node table. Streamed unsolicited after `UplinkHello` and on request. `lastSeenMs` is `MainController` uptime-ms at last contact (relative — the MCU has no wall clock). |
| `0x62` `NodePresence` | MC→S | `Report` | `nodeId(1) · module(1) · up(1) · bootloader(1)` | A node joined discovery (`up = 1`) or missed its heartbeat (`up = 0`); `bootloader` mirrors `Roster`'s per-node bootloader bit at the moment of the transition, for a live update between `Roster` snapshots. |
| `0x63` `ThermostatStatus` | MC→S | `Report` | `controllerNodeId(1) · linkUp(1) · blState(1) · fwMajor(1) · fwMinor(1) · uid[12]` (17 B) | The paired Thermostat behind one ControllerNode — link state + running firmware + identity. On change + slow keepalive; the MC fills it from `Get RoomLink` + `Get ThermostatFirmware`. See `ControllerNode-Thermostat-Link-Spec.md` §5.6. |
| `0x64` `Keepalive` | ↔ | `Get` / `Report` | none | Idle liveness, ~30 s interval. A missed round trip triggers reconnect. |
| `0x65` `OtaControl` | S→MC | `Set` / `Get` | `Set`: `op(1)`, `op=1` (`Begin`) followed by `imageSize(4) · imageCrc32(4) · fwVersion(2)`; `op=2` (`Abort`), `op=3` (`End`), `op=4` (`Activate`) take no further payload. `Get`: none. | MC-self-update control channel — **only MainController's own bootloader speaks this** (`Software/Modules/MainBootloader/Firmware.cpp`), never the running app, never a bus node. See §8 step 7. |
| — | MC→S | `Ack` / `Nack` | `lastError(1)`, `0` on `Ack` | Reply to a `Set` above. |
| — | MC→S | `Report` | `state(1) · expectedOffset(4) · lastError(1) · fwVersion(2)` | Reply to `Get` — lets the server resume after a dropped uplink without resending from offset 0. |
| `0x66` `OtaData` | S→MC | `Set` | `byteOffset(2 LE) · data(≤32)` | One flash-write chunk, sent only while the bootloader is `Receiving` (after `OtaControl[Begin]`, before `[End]`). No RAM page buffer — each chunk is programmed to flash immediately, mirroring the bus `Firmware[Write]` design (`Node-Flash-Layout-and-Bootloader-Spec.md` §6.2.1). |
| — | MC→S | `Ack` / `Nack` | `byteOffset(2 LE) · chunkCrc16(2 LE) · programFailed(1)` | Reply to a chunk — echoes the offset + a flash-read-back CRC16, same correlation the bus protocol uses; a `Nack` names where the bootloader actually is so the server can resync without guessing. |
| `0x67` `MainStatus` | MC→S | `Report` | `rxFrames(4) · crcErrors(4) · resyncs(4) · txDrops(4) · downlinkDrops(4) · wifiRssi(1, int8) · freeHeap(2)` | `MainController` + bus health, ~10 s. `downlinkDrops` counts commands shed by the outbound queue (§7). |

Firmware pushes to a bus node do **not** go through this block — see §8. `Firmware` (`0x20`) and `ThermostatFirmware` (`0x22`) are ordinary relayed endpoints (block `0x10`-`0x50`, §4), so the server drives the OTA sequence directly against them — the MC needs no OTA-specific code for that case, since the generic relay already carries it both ways. The one target the relay can't reach is MainController itself (§8 step 7): it does not run the bus `Firmware` protocol at all, it has its own dedicated bootloader (`Software/Modules/MainBootloader/`, `Node-Flash-Layout-and-Bootloader-Spec.md` §7/§7.1) that speaks `OtaControl`/`OtaData` above instead.

---

## 6. Server architecture

One long-running process on a LAN host (Pi / NUC / mini-PC), under systemd. A single binary with the web UI embedded. It has four responsibilities and exposes three network surfaces on one HTTP port.

### Responsibilities

1. **Accept** the `MainController` TCP connection and validate its `UplinkHello` token.
2. **Deframe and decode** the frame stream → `{node, endpoint, operation, value, ts}`, where `ts` is the server's wall clock at receipt and `value` is decoded per `Node-Message-Model-Spec.md` §5.
3. **Persist** every `Report` to SQLite and update a **current-state cache** (last value per `{node, endpoint}`) held in memory.
4. **Serve** the web application and **push** every value change to connected browsers.

It also tracks node presence from `Roster` / `NodePresence`, composes downlink frames for operator actions and OTA (§7, §8), and re-asserts stored overrides when a node rejoins.

### Network surfaces

| Surface | Path | Purpose |
|---|---|---|
| App bundle | `GET /`, `/assets/*` | The compiled single-page app (`index.html` + JS + CSS). Downloaded once per browser session. |
| JSON API | `GET`/`POST /api/*` | Everything that is not a live value: history queries, commands, node status, config, map setup, the firmware repository + updates (`/api/firmware*`). Request/response over SQLite + the state cache. |
| WebSocket | `GET /ws` | The live channel. On connect the server sends a full current-state snapshot; thereafter it pushes one message per value change for the life of the socket. |

### Data flow — a node value reaching the browser

```
NINA TCP socket → deframe → decode → ┬→ SQLite  readings row
                                     ├→ current-state cache
                                     └→ WebSocket broadcast → every open browser → re-render the one changed element
```

End-to-end latency (node `Report` → browser update) is sub-second on the LAN.

### Browser application

A single-page app. After the initial bundle load it runs entirely client-side:

- Opens `/ws`, applies the initial snapshot, and renders the map and tables with no loading state.
- Applies each incremental `{node, endpoint, value, ts}` push to the single affected element — no reload, no polling.
- Calls the JSON API for non-live actions: history graphs (`GET /api/readings?node=&endpoint=&from=&to=`), overrides (`POST /api/commands`), the firmware repository + updates (`GET/POST /api/firmware`, `POST /api/firmware/update[-all]`), map edits (`POST /api/map/*`).
- Command outcomes (`Ack` / `Nack` / the resulting `Report`) return over the same WebSocket, so every UI update flows through one path.

The operator pages (§7) and the map-setup page are routes within this one app.

### Database (SQLite)

```
nodes(id PK, module, uid, fw_version, first_seen, last_seen, state)  -- fw_version from SystemInfo
readings(ts, node_id, endpoint, raw BLOB, value_num, value_text)   -- indexed (node_id, endpoint, ts)
commands(ts, node_id, endpoint, operation, payload, user, sent_ts, ack_ts, result)
ota_jobs(id PK, node_id, target, filename, size, crc32, fw_version, module, started, finished, state, last_offset, error, image_path)
                                                                   -- target 'node'|'thermostat'; state 'queued' until the single-flight driver picks it up
firmware_images(module PK, filename, version, size, crc32, uploaded_ts, image_path)  -- one held image per module; version = major<<8|minor, parsed from the filename
thermostats(controller_node_id PK, uid, fw_version, bl_state, link_up, last_seen)    -- from 0x63 ThermostatStatus
config_overrides(node_id, endpoint, value, user, ts)               -- re-asserted when the node rejoins
map_floors(id PK, name, image_path, width_px, height_px)
map_placements(node_id, floor_id, x_px, y_px, poly_json NULL)      -- one row per ControllerNode
```

`readings` is the only growing table; the node count keeps its rate low. A retention / downsample job can be added later (§11).

### Frame codec

One server-side module implements the `NodeLib` wire format: the `Frame` deframer (SYNC scan → LEN → CRC-16/CCITT-FALSE → `{node, endpoint, operation, data}`) and the matching encoder for downlink frames. Endpoint payload decoding is a table keyed by endpoint, following `Node-Message-Model-Spec.md` §5.

---

## 7. Operator pages

| Page | Server | Frames on the link |
|---|---|---|
| **Building map** — floor plan, live per-room temperature (§7.1) | renders from the state cache, live-updates over `/ws` | none beyond the node `Report`s already arriving |
| **Overrides** — change setpoint / damper mode / any writable endpoint | writes `config_overrides`, composes `Set <node> <endpoint> <value>` | downlink `Set` → node `Report` / `Ack` → cache + push |
| **Status** — per-node module / fw / uptime / error flags / bus counters | shows `SystemInfo` / `SystemStatus` / `DiagRxCounters` / `MainStatus` | on-demand `Get` downlink |
| **Firmware** — installed version per node (+ a row per Thermostat), the held image per module, per-node / "update all" push | `SystemInfo` fw + `0x63` + `firmware_images`; accepts a `<Module>_<major>.<minor>.bin` | single-flight OTA queue, OTA sequence (§8) |
| **Map setup** (separate config page) | per-floor floor-plan image upload; click to place each `ControllerNode`; optional room polygon | none |

When a node rejoins, the server re-applies any matching `config_overrides` (a rebooted node returns at defaults).

### 7.1 Building map render

- Each **`ControllerNode`** is one placed point, coloured by its relayed `RoomTemp` (`0x41`), labelled `RoomTemp / RoomSetpoint` (`0x40`), with `RoomMode` (`0x43`) and `RoomLink` (`0x44`) as badges.
- **`TemperatureNode`s** are duct sensors (`SupplyTemp` / `ReturnTemp`) near the outside unit — shown in a fixed readout on the map or status page, not placed on the plan.
- The plan is an uploaded image; the live layer is an **SVG overlay rendered in the browser**. Per node, at its `(x_px, y_px)`:
  - default — a marker plus a soft radial-gradient blob, room-sized radius, colour from a temperature ramp (blue 16 °C → green 21 °C → red 26 °C); overlapping blobs blend into a heatmap.
  - if `poly_json` is set — fill that polygon at ~30 % opacity for a crisp room boundary.
- Multi-floor buildings have one `map_floors` row per floor and a floor switcher on the page.
- The temperature ramp and thresholds are server-side UI config.

### 7.2 Downlink rate discipline

The bus is a round-robin poll cycle into which the master injects its own `Set`s. Rapid UI writes (a slider drag) could back up that injection and starve the poll cycle. Two layers bound it:

- **Server** — debounce per `{node, endpoint}` (send only the latest value after a ~200 ms settle) and cap total downlink at ~10 `Set`/s.
- **`MainController`** — a bounded outbound-to-bus queue (~8 frames); on overflow it drops and increments `downlinkDrops` in `MainStatus`. The uplink never delays a bus `Poll` / `Done`.

---

## 8. Firmware update

Drives the bus OTA sequence in `Node-Flash-Layout-and-Bootloader-Spec.md` §6 **directly** — `Firmware` (`0x20`) is an ordinary relayed endpoint (§4), so the server is the one running this state machine, addressing the target node id over the socket exactly like any other `Set`/`Get`. The MC does no OTA-specific translation for this path; it's the same generic relay it already does for `SystemInfo`, `DamperTarget`, etc.

1. Operator uploads `<module>.bin` on the Status page. The server validates the `ImageDescriptor` (magic, module, size) and computes the CRC-32.
2. Server → bus (relayed): `Set Firmware[EnterBootloader]` addressed to the target node id (the app parks outputs, sets the backup magic, resets — `INodeHandler::PrepareForReset`). Server then polls `Get Firmware` on that node id until it gets back any `Firmware Status` Report — only the bootloader ever sends one, the running app only `Ack`/`Nack`s `Firmware`, so any reply confirms the reset landed.
3. Server → bus: `Set Firmware[Begin] {module, imageSize, imageCrc32, fwVersion}`; the node erases its app slot and reports `bl-receiving`.
4. Server streams `Set Firmware[Write] {offset(2 LE), bytes=32}` one chunk at a time, waiting for that write's own `Ack`/`Nack` (echoing the offset + a flash read-back CRC16) rather than watching a generic `Firmware Status` Report for progress — the reply is tied to the specific write, not a raw `expectedOffset` counter to eyeball for stalls (`Node-Flash-Layout-and-Bootloader-Spec.md` §6.2). On a lost ack, the server resends the same `Write`; on a `Nack`, it resyncs to the offset the `Nack` names. Delivery still rides the target's normal poll window, unchanged.
5. At `imageSize`: `Set Firmware[End]` → poll for `bl-valid` → `Set Firmware[Activate]`. The node clears its magic, resets into the new app, re-announces; the server reads the new `fwVersion` from `SystemInfo`.
6. **Thermostat target** — same steps 3-5, but addressed to `ThermostatFirmware` (`0x22`) on the *owning ControllerNode's* node id instead of `Firmware` on the target's own id, and step 2 is skipped: `ThermostatFirmware[Begin]` does the `EnterBootloader` + bootloader-wait itself, CN-side (`ControllerNode-Thermostat-Link-Spec.md` §5.4).
7. **`targetNodeId == 0`** — MainController self-update, a different mechanism entirely, **not** an extension of steps 1-6. Not reachable via any relay (the MC doesn't relay to itself). The server first parks the running app in its bootloader with `SystemControl[Set] {2}` addressed to `NODE=0` — the app's `UplinkHandler` handles a `NODE=0` `SystemControl` itself instead of relaying it onto the bus; it Acks, sets the backup-register magic and resets. The bootloader (`Software/Modules/MainBootloader/`) dials the uplink back out itself, sends a fresh `UplinkHello`, and answers `OtaControl[Get]` with a status `Report` — the server polls for that (any report proves the reset landed; the running app ignores `OtaControl`). Then server → `OtaControl[Begin]` → `OtaData` chunks (each acked, same offset-resume semantics as the bus's `Firmware[Write]`) → `OtaControl[End]` (verifies) → `OtaControl[Activate]` (resets into the new app), all addressed to `NODE=0` on the uplink block itself (§5). The uplink drops when the app resets into the bootloader and again during `Activate`'s reset, reconnecting each time with a fresh `UplinkHello` (the bootloader's reports `fwVersion = 0`). The server driver is `Webserver/internal/service/ota_main.go`.

The STM32 working buffer for this is small: a `Write` payload is 35 B (`MAX_DATA`, `RS485-Node-Protocol-Spec-STM32G030.md` §7), and the bootloader stages no RAM page buffer at all — each `Write` is programmed to flash immediately.

---

## 9. STM32 code / RAM budget

| Piece | Flash | SRAM |
|---|---|---|
| AT command / response / URC engine | ~2–3 KB | ~0.3 KB line buffer |
| Socket bridge (open / reconnect / two `Frame` pumps) | ~1–2 KB | ~0.1 KB |
| `0x60` block handlers (roster, presence, keepalive, status) | ~1 KB | — |
| **Total** | **~4–6 KB** | **< 1 KB** |

Firmware pushes cost the MC nothing beyond the generic relay it already has — no separate OTA glue line item (§8).

`Frame` / `Message` / `Id` / `Crc` / `NodeMaster` are already linked.

---

## 10. Failure modes

| Event | Behaviour |
|---|---|
| Uplink down | Bus and supervisory logic keep running. `Report`s during the outage are lost, not queued. On reconnect: `UplinkHello` → server pulls `Roster`, issues `Get`s to refill the state cache, re-asserts `config_overrides`. |
| Server restart | Same as uplink down, from the MC's view. |
| NINA wedged | AT watchdog: no URC / `OK` within a timeout → `RESET_NINA` pulse (PA6, open-drain) → re-init → reconnect. |
| Node drops mid-OTA | The node's app slot is invalid → its bootloader stays resident → `NodePresence` down then up → server retries from `ota_jobs.last_offset`. |
| Bad frame on the socket | Dropped by `Frame` CRC / resync, same as on the bus; counted in `MainStatus`. |
| Out-of-range override | Node replies `Nack`; server shows the reason and does not persist the override. |

---

## 11. Open items

1. **`readings` retention** — when and how to downsample stored history.
2. **Map polygon editor** — whether v1 ships the room-polygon drawing tool or point placement only.
