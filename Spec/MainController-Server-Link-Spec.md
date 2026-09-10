# MainController ↔ Server Link — Design Spec

**Status:** Draft. The `MainController` relays `NodeLib` v2 frames verbatim between the RS485 node bus and a single LAN server; the server decodes, stores to SQLite, and serves the operator web application.
**Companion docs:** `MainController-Spec.md` §5 (NINA-W152 hardware), `RS485-Node-Protocol-Spec-STM32G030.md` §3 (frame format, reused on this link), `Node-Message-Model-Spec.md` §3–§5 (endpoints, operations, encodings — the server decodes these), `Node-Flash-Layout-and-Bootloader-Spec.md` §6 (the bus OTA sequence this link feeds).

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

- NINA-W152 on USART2, u-connectXpress AT firmware, 115200 8N1, 4-wire hardware flow control (RTS/CTS per `MainController-Spec.md` §5).
- Socket: `AT+USOCR=6` (TCP) → `AT+USOCO=<s>,"<host>",<port>`. Send with `AT+USOWR=<s>,<len>` (binary mode). Inbound data is signalled by `+UUSORD:<s>,<len>` and read with `AT+USORD=<s>,<n>`, `n` ≤ 256 so a read fits a small buffer.
- The **AT engine is a non-blocking state machine** ticked from the same super-loop as `master.Loop()`: it issues one command and returns, collecting the response over later iterations. A stalled uplink never delays a bus `Poll` / `Done`.
- **Reconnect:** exponential backoff, 1 s → 30 s cap. The MCU keeps no uplink send queue — a `Report` produced during an outage is dropped; the value re-reports on its next change or periodic refresh, leaving only a gap in stored history.

---

## 4. Wire format over the socket

Byte-for-byte the RS485 v2 frame (`RS485-Node-Protocol-Spec-STM32G030.md` §3):

```
SYNC(0xEE 0x42) · LEN · NODE · ENDPOINT · OPERATION · DATA[LEN] · CRC16
```

CRC-16/CCITT-FALSE over `NODE..DATA`, little-endian. `LEN` ≤ 32. TCP is a stream, so the server runs the same `Frame` deframer (SYNC scan → LEN → CRC) as a node; a sync loss just resynchronises. No length prefix or envelope is added.

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
| `0x60` `UplinkHello` | MC→S | `Report` | `fwVersion(2) · uptimeSec(4) · nodeCount(1) · authToken(16)` | First frame after every (re)connect. Connection is dropped on token mismatch. |
| `0x61` `Roster` | S→MC / MC→S | `Get` / `Report` | Report: `{nodeId(1) · module(1) · state(1) · lastSeenMs(4)}`, one node per frame, `nodeId = 0xFF` terminates | Full active-node table. Streamed unsolicited after `UplinkHello` and on request. `lastSeenMs` is `MainController` uptime-ms at last contact (relative — the MCU has no wall clock). |
| `0x62` `NodePresence` | MC→S | `Report` | `nodeId(1) · module(1) · up(1)` | A node joined discovery (`up = 1`) or missed its heartbeat (`up = 0`). |
| `0x63` | — | — | — | reserved |
| `0x64` `Keepalive` | ↔ | `Get` / `Report` | none | Idle liveness, ~30 s interval. A missed round trip triggers reconnect. |
| `0x65` `OtaControl` | S→MC / MC→S | `Set` / `Report` | Set: `targetNodeId(1) · imageSize(4) · imageCrc32(4) · fwVersion(2) · module(1)`. Report: `state(1) · targetNodeId(1) · nextOffset(4) · lastError(1)` | Start / abort / progress of an image push (§8). `targetNodeId = 0` = `MainController` self-update. |
| `0x66` `OtaData` | S→MC | `Set` | `offset(4) · bytes(≤27)` — identical layout to the bus `Firmware Write` payload | One image chunk; the bridge rewrites the header and forwards it (§8). |
| `0x67` `MainStatus` | MC→S | `Report` | `rxFrames(4) · crcErrors(4) · resyncs(4) · txDrops(4) · downlinkDrops(4) · wifiRssi(1, int8) · freeHeap(2)` | `MainController` + bus health, ~10 s. `downlinkDrops` counts commands shed by the outbound queue (§7). |

`OtaControl` / `OtaData` share the exact sub-opcode and payload shapes of the bus `Firmware` endpoint, so relaying is a header rewrite rather than a repack.

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
| JSON API | `GET`/`POST /api/*` | Everything that is not a live value: history queries, commands, node status, config, map setup, firmware upload. Request/response over SQLite + the state cache. |
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
- Calls the JSON API for non-live actions: history graphs (`GET /api/readings?node=&endpoint=&from=&to=`), overrides (`POST /api/commands`), firmware upload (`POST /api/ota`), map edits (`POST /api/map/*`).
- Command outcomes (`Ack` / `Nack` / the resulting `Report`) return over the same WebSocket, so every UI update flows through one path.

The operator pages (§7) and the map-setup page are routes within this one app.

### Database (SQLite)

```
nodes(id PK, module, uid, fw_version, first_seen, last_seen, state)
readings(ts, node_id, endpoint, raw BLOB, value_num, value_text)   -- indexed (node_id, endpoint, ts)
commands(ts, node_id, endpoint, operation, payload, user, sent_ts, ack_ts, result)
ota_jobs(id PK, node_id, filename, size, crc32, fw_version, started, finished, state, last_offset, error)
config_overrides(node_id, endpoint, value, user, ts)               -- re-asserted when the node rejoins
map_floors(id PK, name, image_path, width_px, height_px)
map_placements(node_id, floor_id, x_px, y_px, poly_json NULL)      -- one row per ControllerNode
```

`readings` is the only growing table; the node count keeps its rate low. A retention / downsample job can be added later.

### Frame codec

One server-side module implements the `NodeLib` wire format: the `Frame` deframer (SYNC scan → LEN → CRC-16/CCITT-FALSE → `{node, endpoint, operation, data}`) and the matching encoder for downlink frames. Endpoint payload decoding is a table keyed by endpoint, following `Node-Message-Model-Spec.md` §5.

---

## 7. Operator pages

| Page | Server | Frames on the link |
|---|---|---|
| **Building map** — floor plan, live per-room temperature (§7.1) | renders from the state cache, live-updates over `/ws` | none beyond the node `Report`s already arriving |
| **Overrides** — change setpoint / damper mode / any writable endpoint | writes `config_overrides`, composes `Set <node> <endpoint> <value>` | downlink `Set` → node `Report` / `Ack` → cache + push |
| **Status** — per-node module / fw / uptime / error flags / bus counters, plus firmware upload | shows `SystemInfo` / `SystemStatus` / `DiagRxCounters` / `MainStatus`; accepts a `.bin` | on-demand `Get` downlink; OTA sequence (§8) |
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

Drives the bus OTA sequence in `Node-Flash-Layout-and-Bootloader-Spec.md` §6; the link only feeds it.

1. Operator uploads `<module>.bin` on the Status page. The server validates the `ImageDescriptor` (magic, module, size) and computes the CRC-32.
2. Server → MC: `Set 0x65 OtaControl {targetNodeId, imageSize, imageCrc32, fwVersion, module}`.
3. MC puts the target into its bootloader: relay `Set Firmware[EnterBootloader]` on the bus (the app parks outputs, sets the backup magic, resets — `INodeHandler::PrepareForReset`), then poll `Firmware Get` until the node answers `Status` from the bootloader.
4. MC issues bus `Firmware[Begin]`; the node erases its app slot and reports `bl-receiving`.
5. Server streams `Set 0x66 OtaData {offset, bytes≤27}`. For each, MC rewrites the header to a bus `Firmware[Write]` (payload copied verbatim) and sends it in the target's poll window. MC relays each `Firmware Status` up as `0x65 OtaControl Report`; if `nextOffset` stalls, MC reports the wanted offset for the server to rewind to.
6. At `imageSize`, server sends the end marker (`0x65`) → MC issues bus `Firmware[End]` → polls for `bl-valid` → `Firmware[Activate]`. The node clears its magic, resets into the new app, re-announces; the server reads the new `fwVersion` from `SystemInfo`.
7. **`targetNodeId == 0`** — same `0x65` / `0x66` frames, but MC writes its own application slot with a RAM-resident flash routine (`Node-Flash` §7.1) instead of relaying, then resets. The uplink drops during the write and reconnects on the new image.

The STM32 working buffer stays ~32 B (one `OtaData` payload); no whole image is buffered.

---

## 9. STM32 code / RAM budget (over today's MainController)

| Piece | Flash | SRAM |
|---|---|---|
| AT command / response / URC engine | ~2–3 KB | ~0.3 KB line buffer |
| Socket bridge (open / reconnect / two `Frame` pumps) | ~1–2 KB | ~0.1 KB |
| `0x60` block handlers (roster, presence, keepalive, status) | ~1 KB | — |
| OTA glue (`0x65` / `0x66` ↔ bus `Firmware`) | ~1–2 KB | ~32 B chunk |
| **Total** | **~5–8 KB** | **< 1 KB** |

The MainController image is ~20 KB in a 52 KB slot. `Frame` / `Message` / `Id` / `Crc` / `NodeMaster` are already linked.

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

1. **NINA bring-up** — confirm the u-connectXpress AT command set on the actual NINA-W152 variant (socket create / connect / write / read, `+UUSORD`, reset). Nothing here needs MQTT or HTTP from the module.
2. **`readings` retention** — when and how to downsample stored history.
3. **OTA rewind frame** — exact fields of the `0x65 OtaControl Report` the MC uses to request a resend from `nextOffset`.
4. **Map polygon editor** — whether v1 ships the room-polygon drawing tool or point placement only.
