# MainController ↔ Server Link — Design Spec

**Status:** Draft — shape + most parameters fixed 2026-09-09 (Option 1: node frames relayed verbatim to a server that owns all state and persistence). Decided: **LAN-only, no TLS** for v1; server = **Go daemon + SQLite** (recommendation, not hard-locked — §10); socket RX = `+UUSORD` polling; **no time sync / no RTC**; roster = one node per frame; downlink throttled server-side. Supersedes the "still open" outward-interface note in `MainController-Spec.md` §2/§4.
**Companion docs:** `MainController-Spec.md` (§5 = the NINA-W152 hardware), `RS485-Node-Protocol-Spec-STM32G030.md` (§3 frame format, reused verbatim on this link), `Node-Message-Model-Spec.md` (§3 endpoints, §4 operations, §5 encodings — the server decodes these), `Node-Flash-Layout-and-Bootloader-Spec.md` (§6 the bus OTA sequence this link feeds), `Software-Architecture-Spec.md`

---

## 1. Confirmed role & topology

`MainController` is **bus master + supervisor + a frame bridge** to a single server. It runs the `NodeMaster` poll loop and its own supervisory logic (fault detection, safe-state, aggregation — scope in `MainController-Spec.md` §2); on top of that it holds one outbound TCP connection to the server and moves `NodeLib` v2 frames across it in both directions. Over the *bridge* it does **not** decode endpoint payloads, hold a schema, or serialise anything — the server owns all persistent state and the UI. Its supervisory work runs **regardless of uplink state**; only outward comms go through the bridge.

```
   RS485 bus (115200, half-duplex, HW-DE)          Wi-Fi / LAN or Internet
 ┌───────────┐   ┌───────────┐   ┌───────────┐    ┌──────────────┐        ┌─────────┐
 │Temperature│   │Controller │   │Controller │    │ MainController│  TCP   │ Server  │
 │  Node  3  │───│  Node  5  │───│  Node  7  │────│  (NodeMaster  │═══════▶│  + DB   │
 └───────────┘   └───────────┘   └───────────┘    │  + AT bridge) │  TLS   │  + web  │
                       │  Thermostat link          │   ── NINA ──  │        └────┬────┘
                       └── (local, off-bus) ──     └──────────────┘         WebSocket
                                                                          ┌────┴────┐
                                                                          │ browser │
                                                                          │ 3 pages │
                                                                          └─────────┘
```

- **Connection is outbound** from `MainController` → no inbound port-forward / firewall hole at the building.
- **One socket, plaintext TCP on the LAN.** Decided 2026-09-09: v1 is **LAN-only, no TLS** — the `AT+USECMNG` cert path is out of scope. Auth is a 16-byte shared token in `UplinkHello` (§5) so a stray LAN device can't inject frames. (If the deployment ever goes internet-facing, TLS goes back in — terminated in the NINA, still no STM32 crypto.)
- The bus keeps running normally while the uplink is down — nodes are autonomous and each `ControllerNode`↔`Thermostat` loop is local (`ControllerNode-Thermostat-Link-Spec.md`). The uplink is **supervisory + logging + operator control**, not part of any closed loop.

---

## 2. Why relay raw frames (rationale)

| Concern | Why frame pass-through wins |
|---|---|
| STM32 flash (64 KB, image ~20 KB today) | Reuses `Frame`/`Message`/`Id`/`Crc` already linked — **≈0 new serialisation code**. No JSON writer, no schema, no per-endpoint marshalling. |
| STM32 SRAM (8 KB) | Working set is one frame in + one frame out (~40 B each) plus the AT engine's line buffer. No object model of the building. |
| Protocol churn | New endpoints / value types are a **server-only** change. The bridge never needs a firmware update to carry a new kind of reading. |
| Crypto | None on the STM32 — the NINA does TLS. |
| One decoder | The server implements the `Node-Message-Model-Spec` decode **once** and uses it for both bus history and the web UI. |

The cost is that the server must speak the `NodeLib` wire format. That decoder is ~100 lines (§10) and is a stable, already-specified format.

---

## 3. Transport — NINA socket

- NINA-W152 on USART2, u-connectXpress AT firmware, 115200 8N1, **4-wire HW flow control** (RTS/CTS wired per `MainController-Spec.md` §5 — needed so URC bursts and socket reads don't overrun the STM32).
- Socket lifecycle: `AT+USOCR=6` (TCP) → `AT+USOCO=<s>,"<host>",<port>`. Write with `AT+USOWR=<s>,<len>` in binary mode; inbound data raised as `+UUSORD:<s>,<len>` then pulled with `AT+USORD=<s>,<n>` (`n` bounded to ≤ 256 so the read fits a small buffer).
- **Socket RX = `+UUSORD` polling / plain data mode** (decided 2026-09-09). Extended Data Mode (EDM) — a binary envelope around all socket/event traffic — removes the notify-then-request round-trip and lowers latency for sustained streams, but adds an envelope parser (~0.5 KB + a state machine) on the STM32. Our traffic is sparse (a few small frames/s, bursty only during OTA, which the bus bottlenecks); the polling round-trip is negligible. Revisit EDM only if hardware shows a latency/throughput problem.
- **The AT engine must not block the `NodeMaster` poll loop.** It is a state machine ticked from the same super-loop as `master.Loop()`: issue one AT command, return, collect the response across later iterations. A stalled uplink must never delay a `Poll`/`Done` on the bus.
- **Reconnect:** exponential backoff (1 s → 30 s cap). On reconnect the server re-requests a roster + current state (§7); the STM32 holds no send queue for the uplink — an un-sent `Report` during an outage is simply lost (the value re-reports on change or on the node's next periodic refresh, and the server's history has a gap, which is acceptable for supervisory data).

---

## 4. Wire format over the socket

**Byte-for-byte the RS485 v2 frame** (`RS485-Node-Protocol-Spec-STM32G030.md` §3): `SYNC(0xEE 0x42) · LEN · NODE · ENDPOINT · OPERATION · DATA[LEN] · CRC16` (CRC-16/CCITT-FALSE over `NODE..DATA`, little-endian). `LEN` is capped at 32 on this link too, for a single decoder and a bounded bridge buffer.

TCP is a stream, so the server runs the **same `Frame` deframer** (SYNC scan + LEN + CRC) as a node does — sync loss just resyncs. No length-prefix or envelope is added.

Two frame classes share the link:

| Class | `ENDPOINT` | Meaning |
|---|---|---|
| **Relayed node frame** | `0x10`–`0x5F` | A verbatim copy of a frame seen on / destined for the bus. `NODE` is the real node id. Downlink `Get`/`Set` from the server are put on the bus as-is; uplink `Report`/`Ack`/`Nack` are forwarded up as-is. |
| **Uplink frame** | `0x60`–`0x6F` | Between `MainController` (`NODE = 0`) and the server only — never on the RS485 bus. Roster, link health, time, OTA control/data. Claims the reserved `0x60` block of the shared `Endpoint` enum (`Node-Message-Model-Spec.md` §3). |

`Discover` / `Announce` / `Poll` / `Done` (transport verbs) are **not** relayed — they are bus-internal. Node presence changes are surfaced to the server as `NodePresence` (§5).

---

## 5. Uplink endpoint block (`0x60`–`0x6F`)

All on `ENDPOINT` in the `0x6_` block, `NODE = 0`, encodings little-endian per `Node-Message-Model-Spec.md` §5.

| Endpoint | Dir | Op | Payload | Meaning |
|---|---|---|---|---|
| `0x60` `UplinkHello` | MC→S | `Report` | `fwVersion(2) · uptimeSec(4) · nodeCount(1) · authToken(16)` | First frame after (re)connect. Server drops the connection if `authToken` doesn't match (the whole v1 auth story — no TLS). |
| `0x61` `Roster` | S→MC / MC→S | `Get` / `Report` | Report: repeated `{nodeId(1) · module(1) · state(1) · lastSeenMs(4)}`, **one node per frame**, `nodeId = 0xFF` terminates | Server asks for the full active-node table; MC streams it (~30 tiny frames). Sent unsolicited after `UplinkHello`. `lastSeenMs` = MC uptime-ms at last contact (relative — there is no wall clock on the MC). |
| `0x62` `NodePresence` | MC→S | `Report` | `nodeId(1) · module(1) · up(1: 1=joined 0=dropped)` | Node joined discovery / missed the heartbeat. |
| `0x63` — | | | | *reserved* (was `TimeSync`; removed 2026-09-09 — the system uses no schedules/clock). |
| `0x64` `Keepalive` | ↔ | `Get`/`Report` | none | Idle-liveness. Interval ~30 s; a missed round trips reconnect. |
| `0x65` `OtaControl` | S→MC / MC→S | `Set` / `Report` | Set: `targetNodeId(1) · imageSize(4) · imageCrc32(4) · fwVersion(2) · module(1)`. Report: `state(1) · targetNodeId(1) · nextOffset(4) · lastError(1)` | Start / abort / progress of an image push (§8). `targetNodeId = 0` = MainController self-update. |
| `0x66` `OtaData` | S→MC | `Set` | `offset(4) · bytes(≤27)` — **identical layout to the bus `Firmware Write` payload** | One image chunk. MC rewrites the header and forwards to the node with almost no logic (§8). |
| `0x67` `MainStatus` | MC→S | `Report` | `rxFrames(4) · crcErrors(4) · resyncs(4) · txDrops(4) · downlinkDrops(4) · wifiRssi(1, int8) · freeHeap(2)` | Periodic (~10 s) MainController + bus health. `downlinkDrops` = commands dropped by the MC's bounded outbound queue (§7). |

`OtaControl` / `OtaData` reuse the exact bus `Firmware` sub-opcode and Write-payload shapes so the bridge is a header rewrite, not a repack.

---

## 6. Server responsibilities

The server is the whole application. It:

1. **Deframes** the socket stream → `{node, endpoint, operation, data}` records.
2. **Decodes** per `Node-Message-Model-Spec.md` §5 (e.g. `endpoint 0x38 SupplyTemp` → `int16` centi-°C).
3. **Timestamps and persists** every `Report` — `ts` is the **server's wall clock at receipt** (there is no clock on the STM32; LAN latency is ms, fine for supervisory history). Keeps a **current-state cache** (last value per `{node, endpoint}`) for instant page loads.
4. **Serves the web UI** (three pages, §7) and **pushes** updates to browsers over its own WebSocket / SSE — the STM32 knows nothing about browsers.
5. **Composes downlink frames** for operator actions and OTA, with the rate discipline in §7.
6. **Tracks node presence** from `NodePresence` / `Roster` and flags stale nodes.
7. **Owns auth** — browser↔server is a server concern (sessions); the `MainController`↔server socket is gated on the `UplinkHello` token (§5).

### Database sketch (SQLite — §10)

```
nodes(id PK, module, uid, fw_version, first_seen, last_seen, state)
readings(ts, node_id, endpoint, raw BLOB, value_num, value_text)      -- indexed (node_id, endpoint, ts)
commands(ts, node_id, endpoint, operation, payload, user, sent_ts, ack_ts, result)
ota_jobs(id PK, node_id, filename, size, crc32, fw_version, started, finished, state, last_offset, error)
config_overrides(node_id, endpoint, value, user, ts)                  -- desired values the server re-asserts on node rejoin
```

`readings` is the only table that grows — a handful of nodes reporting every few seconds is trivial volume for SQLite. Add a retention/downsample job if history gets large; no need for a dedicated time-series engine at this scale.

---

## 7. Web pages ↔ link mapping

| Page | Server does | Frames on the link |
|---|---|---|
| **Building map** — floor plan, live temperatures, damper %, room setpoints | render from the current-state cache; live-update via its WebSocket | none extra — it's already receiving every node `Report` (uplink) |
| **Overrides** — change setpoints / damper mode / any writable endpoint | on submit: write `config_overrides`, compose `Set <nodeId> <endpoint> <value>` | downlink relayed `Set` → node `Report`/`Ack` uplink → cache + push |
| **Status** — per-node module/fw/uptime/error flags/bus counters, plus **"push update"** | show `SystemInfo` / `SystemStatus` / `DiagRxCounters` / `MainStatus`; accept a `.bin` upload | `Get` relayed downlink to refresh a node on demand; OTA sequence (§8) |

On node rejoin the server re-asserts anything in `config_overrides` (a rebooted node comes back at defaults).

### Downlink rate discipline

The bus is a round-robin poll cycle; the master injects its own `Set`s into it. A UI firing rapid writes (a slider drag) could back up the master's bus-injection and starve the poll cycle — stale readings, and nodes tripping their master-lost heartbeat. Two layers:

- **Server (primary):** debounce per `{node, endpoint}` — send only the latest value after a ~200 ms settle — and cap total downlink to ~10 `Set`/s.
- **MainController (backstop):** a bounded outbound-to-bus queue (~8 frames). On overflow, drop and increment `downlinkDrops` in `MainStatus` (§5). The MC never lets the uplink delay a bus `Poll`/`Done`.

---

## 8. Firmware update over the uplink

Reuses the bus OTA sequence (`Node-Flash-Layout-and-Bootloader-Spec.md` §6) end-to-end; the link only feeds it.

1. Operator uploads `<module>.bin` on the Status page. Server validates the `ImageDescriptor` (magic, `module`, size) and computes the CRC-32.
2. Server → MC: `Set 0x65 OtaControl {targetNodeId, imageSize, imageCrc32, fwVersion, module}`.
3. MC gets the target into its bootloader: relay `Set Firmware[EnterBootloader]` on the bus (the running app parks outputs, sets the backup magic, resets — `INodeHandler::PrepareForReset`). MC polls `Firmware Get` until the node answers `Status` from the bootloader.
4. MC issues bus `Firmware[Begin]` (from the `OtaControl` params). Node erases its app slot, reports `bl-receiving`.
5. Server streams `Set 0x66 OtaData {offset, bytes≤27}`. For each, MC rewrites the header to a bus `Firmware[Write]` frame (**payload copied verbatim**) and sends it in the target's poll window. MC relays the node's `Firmware Status` up as `0x65 OtaControl Report`; if `nextOffset` stalls, MC asks the server to rewind by reporting the wanted offset.
6. Server sends an end marker (`0x65` with `imageSize` reached) → MC issues bus `Firmware[End]` → polls for `bl-valid` → `Firmware[Activate]`. Node clears its magic and resets into the new app; it re-announces and the server sees the new `fwVersion` in `SystemInfo`.
7. **`targetNodeId == 0` (MainController self-update):** same `0x65`/`0x66` frames, but MC writes its **own** application slot with a RAM-resident flash routine (`Node-Flash` §7.1) instead of relaying to the bus, then `NVIC_SystemReset`. The uplink drops during the write and reconnects on the new image.

Working buffer on the STM32 stays ~32 B (one `OtaData` payload); nothing buffers a whole image.

---

## 9. STM32 code / RAM budget (incremental over today's MainController)

| Piece | Flash | SRAM |
|---|---|---|
| AT command/response/URC engine | ~2–3 KB | ~0.3 KB line buffer |
| Socket bridge (open / reconnect / two `Frame` pumps) | ~1–2 KB | ~0.1 KB |
| `0x60` block handlers (roster, presence, keepalive, status) | ~1 KB | — |
| OTA glue (`0x65`/`0x66` ↔ bus `Firmware`) | ~1–2 KB | ~32 B chunk |
| **Total** | **~5–8 KB** | **< 1 KB** |

MainController image is ~20 KB in a 52 KB slot — comfortable. `Frame`/`Message`/`Id`/`Crc`/`NodeMaster` are already linked. No TLS and no EDM envelope parser keeps this at the low end of the range.

---

## 10. Server implementation notes

**Language — Go daemon (recommendation).** Chosen for an on-prem appliance that must run untouched for years: compiles to one static binary, cross-compiles to ARM (Pi) trivially, no runtime to install or keep patched, and its concurrency model (one socket in, N browser sockets out, DB writes) is idiomatic with goroutines + channels. The stdlib covers TCP + HTTP; `database/sql` + a SQLite driver covers storage; a small WS library covers browser push. Node.js (`net` + `ws`) is an acceptable alternative if a single language across front and back is preferred — the frontend is JS either way; the tradeoff is `npm`/native-module/version churn over the appliance's life. Not hard-locked, but the daemon and the UI should be split regardless: the always-on daemon must be rock solid, the UI can iterate.

**Frame decoder** — the only non-trivial shared logic. Port `Frame`'s state machine + CRC-16/CCITT-FALSE; encode is trivial (build header, CRC, prepend sync). One module so bus semantics live in exactly one place per side.

**Database — SQLite.** Single on-prem box, one file, trivial backup (copy the file), zero ops. Volume from ~10–30 nodes at a few reports/minute is nothing for SQLite; add a retention/downsample job only if `readings` history grows large. A dedicated time-series engine is not warranted at this scale.

**Deployment** — a small always-on box on the LAN (Pi / NUC / mini-PC), the Go binary under systemd, serving the static UI itself. No TLS in v1 (§3).

---

## 11. Failure modes

| Event | Behaviour |
|---|---|
| Uplink down | Bus keeps running; nodes autonomous; the MC's own supervisory logic keeps running. `Report`s during the outage are lost (not queued). On reconnect: `UplinkHello` → server pulls `Roster` + issues `Get`s to refill current state + re-asserts `config_overrides`. |
| Server restart | Same as uplink down from the MC's view. |
| NINA wedged | AT engine watchdog: no URC / no `OK` within N s → `RESET_NINA` pulse (PA6 open-drain) → re-init → reconnect. |
| Node drops mid-OTA | Node's app slot is erased/invalid → its bootloader stays resident → `NodePresence` down then up (in bootloader) → server retries the job from `ota_jobs.last_offset`. |
| Bad frame on the socket | `Frame` CRC/resync drops it, same as the bus. Counted in `MainStatus`. |
| Operator sets an out-of-range value | Node replies `Nack`; server shows the reason, does not persist the override. |

---

## 12. Decisions (2026-09-09) & remaining open items

**Decided:**

| # | Decision |
|---|---|
| Server stack | Go daemon + SQLite; UI as static files it serves. Daemon and UI split. (§10) |
| Deployment | LAN-only, on-prem box. **No TLS** in v1. (§3) |
| Socket auth | 16-byte shared token in `UplinkHello`; server drops the connection on mismatch. (§5) |
| Socket RX | `+UUSORD` polling / plain data mode — not EDM. (§3) |
| Time | None. No `TimeSync`, no RTC, no battery. Readings timestamped by the server on receipt. (§6) |
| MainController role | Bus master + supervisor + bridge; supervisory logic is uplink-independent. (§1) |
| Roster transfer | One node per frame, `0xFF` terminates — `LEN` stays ≤ 32 everywhere. (§5) |
| Downlink throttle | Server debounces per-endpoint (~200 ms) + caps ~10 `Set`/s; MC has an 8-frame backstop queue, drops → `downlinkDrops`. (§7) |

**Still open:**

1. **NINA firmware bring-up** — confirm the u-connectXpress AT command set on the actual `NINA-W152` variant (socket create/connect/write/read, `+UUSORD`, reset behaviour). Nothing in this spec needs MQTT or HTTP from the module.
2. **Server ↔ browser transport** — WebSocket vs SSE for the live push (pure server-side choice).
3. **Map authoring** — how the floor plan + node positions are defined (static SVG + a coords table in the DB, or an in-UI editor). UI concern, not the link.
4. **Retention** — when/whether to downsample `readings` history.
5. **OTA rewind protocol detail** — exact frame the MC sends the server to request a resend from `nextOffset` (a `0x65 OtaControl Report` with the wanted offset is the intent; nail the field on implementation).
