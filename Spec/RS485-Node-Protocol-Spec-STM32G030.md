# RS485 Node Bus — Protocol Design Spec (v2)
### Target MCU: STM32G031F8P6 (was STM32G030F6P6TR) — replaces Arduino/ATmega NodeLib

**Status:** Draft for review
**Supersedes:** `NodeLib` fixed-frame protocol (magic bytes + packed struct, no CRC)
**Author's context:** Same physical bus (half-duplex RS485, one master + N slaves), reusing the poll/flush/heartbeat state machine, replacing the wire format with a variable-length, CRC-protected frame.

---

## 1. Goals / non-goals

**Goals**
- Variable-length data field per message (no longer locked to a single `uint8_t` value).
- CRC-protected frames, so corruption is *detected*, not silently accepted.
- Reuse the existing round-robin master/slave state machine (`Discover` → `Announce` → `Poll` → `Done` → heartbeat; ops renamed from v1's `DETECTNODES`/`HELLOWORLD`/`SENDQ`/`ENDOFQ` in `Node-Message-Model-Spec.md` §4), since that part works and isn't hardware-specific.
- Take advantage of STM32G0 peripherals (hardware CRC unit, USART auto-direction-control) that the ATmega/Arduino stack didn't have.

**Non-goals (for v2, call out explicitly if you want these later)**
- Multi-master arbitration — still single master, polled bus.
- Guaranteed delivery / retransmission at the protocol level — still best-effort per round, recovered by the next poll cycle or heartbeat timeout.
- Encryption/authentication — physical bus assumed trusted.

---

## 2. MCU / physical layer

| Item | Choice | Notes |
|---|---|---|
| MCU | STM32G031F8P6 | Cortex-M0+, 64 MHz max, 64 KB flash, 8 KB SRAM, TSSOP20 (locked in 2026-09-08; was STM32G030F6P6TR — drop-in, +32 KB flash, adds LPUART1 / RTC / TIM2). §7 SRAM budget unchanged. |
| UART | USART1 | Supports **hardware Driver-Enable (DE)** output — no manual GPIO toggle + `delay()` needed |
| Transceiver | MAX3485CSA-JSM (JSMSEMI), LCSC `C6395158` | 3.3V half-duplex RS-485, SOP-8 (second source: HTCSEMI `HT83485ARZ`, `C2960978`). DE/RE tied together, driven by USART1's DE pin. Standard EIA-485 common-mode range (-7V to +12V) — margin against this bus's ground-offset estimates checked in `Node-Bus-Hardware-Design-Spec.md` §6, comfortable after the both-ends power feed. |
| Baud rate | **250 000** (§9) | Exact integer USART divisor (no generator error to add to the crystal-less HSI16 spread); 2.5×10⁷ bit·m/s is deep inside the safe region for the ~100 m / 20-node terminated bus. 500 k is a bench-validated fallback; 1 Mbit is not used. |
| CRC engine | Hardware CRC peripheral (`CRC` block) | Offloads CRC calc from CPU, frees it for polling/servo timing |

### Why hardware DE control matters
The old code hand-manages the enable pin (`setEnable()` with 10–15 ms settle delays) because the ATmega has no automatic transceiver-direction feature. STM32G0's USART1 has **DEM/DEP + DEAT/DDAT** — you configure an assertion/de-assertion time in bit-periods and the peripheral toggles the DE pin around each transmission automatically, with no CPU-side delay loop. This removes a whole class of timing bugs and shortens turnaround time between poll and reply. **Recommendation: use hardware DE, drop `setEnable()`/`delay()` entirely.**

---

## 3. Frame format

```
┌─────────┬─────────┬────────┬─────────┬───────────┬──────────┬─────────┐
│  SYNC   │  SYNC   │  LEN   │ NODE_ID │  ENDPOINT │ OPERATION│  DATA   │  CRC16
│  0xEE   │  0x42   │ 1 byte │ 1 byte  │  1 byte   │  1 byte  │ N bytes │ 2 bytes
└─────────┴─────────┴────────┴─────────┴───────────┴──────────┴─────────┘
  byte 0     byte 1    byte 2   byte 3     byte 4      byte 5   byte 6..(6+N-1)   last 2 bytes
```

| Field | Size | Description |
|---|---|---|
| `SYNC` | 2 bytes | Keep `0xEE 0x42` from v1 for continuity. Only scanned for while the receiver is *not* mid-frame. |
| `LEN` | 1 byte | Length of `DATA` **only** (0–255). Header (`NODE_ID`/`ENDPOINT`/`OPERATION`) is fixed size and not counted. Cap enforced in firmware at e.g. 32 bytes (see §7) even though the field allows 255 — keeps buffers small and bounds worst-case bus occupancy. |
| `NODE_ID` | 1 byte | Target/source node address. `0` = master (reserved); `1..MAX_NODES-1` = slaves; `0xFF` = broadcast (`Set`-only, no reply) — see `Node-Message-Model-Spec.md` §2. |
| `ENDPOINT` | 1 byte | The addressable thing on the node — `NodeLib::Endpoint` enum. Was `CHANNEL`/`ChannelId` (an IO-pin selector); redefined as a flat named-endpoint enum in `Node-Message-Model-Spec.md` §3. |
| `OPERATION` | 1 byte | `NodeLib::Operation` — `Get`/`Set`/`Report`/`Ack`/`Nack` for endpoint access, plus the transport verbs `Discover`/`Announce`/`Poll`/`Done`. Full table in `Node-Message-Model-Spec.md` §4. |
| `DATA` | `LEN` bytes | Payload — arbitrary bytes (int16/float/string/blob), interpreted per `ENDPOINT` (+ `OPERATION`) convention per `Node-Message-Model-Spec.md` §5, not enforced by the frame. |
| `CRC16` | 2 bytes | Computed over `NODE_ID..DATA` inclusive (**not** over `SYNC` or `LEN`—see §4 rationale), little-endian on the wire. |

Total frame overhead is 7 bytes (2 sync + 1 len + 4 header/crc-adjacent... see table) vs. payload; for a 1-byte payload that's a larger relative overhead than v1's fixed 6-byte frame, but you get arbitrary payload sizes in return.

---

## 4. CRC

- **Algorithm:** CRC-16/CCITT-FALSE (poly `0x1021`, init `0xFFFF`, no reflect, no final XOR) — a common, well-tested 16-bit CRC with good burst-error detection, and directly configurable on STM32G0's CRC peripheral (programmable polynomial size 7/8/16/32 bits, programmable polynomial value, programmable init value — this exact use case is what the G0 CRC block was designed for, unlike older STM32 families locked to CRC-32/Ethernet poly).
- **Coverage:** `NODE_ID`, `CHANNEL`, `OPERATION`, and all `DATA` bytes. `SYNC` is excluded (it's a framing marker, not payload — including it buys nothing). `LEN` is **included implicitly** by virtue of the receiver knowing where `DATA` ends, but is *not itself* covered by CRC in this scheme — see the resync note in §6 for why that's acceptable here.
- **On receive:** compute CRC over the received header+data, compare to the trailing 2 bytes. Mismatch → drop frame, do not call any handler, let the round-robin/heartbeat mechanism recover on the next cycle (same philosophy as v1 — no in-band retry).
- **Implementation:** feed bytes to `CRC->DR` as they arrive (or in one shot from a fully-buffered frame — see §7 for the buffering strategy); read `CRC->DR` for the result. Reset (`CRC->CR |= CRC_CR_RESET`) before each frame.

---

## 5. Framing / resync strategy

v1 could get away with no resync logic because every frame was the same fixed size — worst case, one garbled frame, next sync search starts fresh at a known offset. Variable length breaks that assumption: **if the `LEN` byte itself is corrupted, the receiver will consume the wrong number of bytes and misinterpret the following, real frame.**

Chosen mitigation (deliberately not full byte-stuffing/COBS — see rationale below):

1. **CRC catches almost all bad frames.** A corrupted `LEN` will, with overwhelming probability, cause the CRC computed over the (wrong) byte range to fail against the trailing 2 bytes, so the bad frame is rejected rather than silently accepted.
2. **Inter-byte timeout.** If more than `T_gap` (recommend 2–3 byte-periods at the configured baud — ~100 µs at 250 000, §9 item 2) elapses between bytes while mid-frame, abandon the partial frame and return to sync-hunting. This bounds how long a single corruption event can wedge the parser.
3. **Max frame guard.** If `LEN` (or a corrupted read of it) would make the frame exceed the configured max (§7), abandon and resync immediately rather than blocking on bytes that will never come in the expected window.
4. **`SYNC` byte still re-armed continuously in payload search**, exactly like v1: even while "in frame," if the resulting frame fails CRC, the next sync search starts from the byte immediately after the failed attempt's SYNC, not from scratch at the buffer start — so a spuriously-matched SYNC inside a garbled stream doesn't cost more than one bad frame.

**Why not COBS/byte-stuffing:** on a short, low-noise wired bus with a small, closed set of nodes, CRC + timeout-based resync is simpler to implement and debug on a Cortex-M0+ with 8 KB RAM, and failure mode is "occasionally drop one frame, recovered next poll cycle" rather than "corrupt state." If you move to a longer/noisier bus or higher node count later, COBS is the natural upgrade — flag it as an explicit v3 candidate rather than building it now.

---

## 6. Addressing, topology, operations

The round-robin transport (discovery / poll cycle / heartbeat) carries over from v1 almost line-for-line — it is wire-format-agnostic. The *message semantics* on top of it (endpoints, operation verbs, reporting model) are specified in `Node-Message-Model-Spec.md`; this section covers only the transport-level items.

| Item | v1 | v2 |
|---|---|---|
| Master ID | `0`, hardcoded constant | `0`, still reserved |
| Node ID range | 1–10 (`numNodes = 10`, compile-time) | `1 .. MAX_NODES-1` (`NodeLib::MAX_NODES`, currently 25). Each node's ID is factory-provisioned in flash and read-only — `Node-Flash-Layout-and-Bootloader-Spec.md` §6.3. |
| Broadcast | — (none; `DETECTNODES` recognised by op only) | `NODE_ID = 0xFF`, valid with `Operation::Set` only (fire-and-forget, no reply) — `Node-Message-Model-Spec.md` §2 |
| Discovery | Broadcast `DETECTNODES`, staggered `HELLOWORLD` replies by `(nodeId-1) * nodeSpacing` ms | Same mechanism; ops renamed `Discover` / `Announce`. `Announce` payload carries module type + 96-bit UID for the master's roster. |
| Poll cycle | Master → `SENDQ` → node dumps queue → `ENDOFQ` → master polls next active node | Same; ops renamed `Poll` / `Done`. A polled node dumps its queued `Report`s (on-change + keepalive, `Node-Message-Model-Spec.md` §6.1). |
| Heartbeat | Master-side timer reset each full poll round; `ConnectionLost()` on lapse | Same |
| `Operation` enum | `GET/SET/SETPWM/VALUE/ERROR/SETMODE/DETECTNODES/HELLOWORLD/SENDQ/ENDOFQ` | Redesigned — `Get/Set/Report/Ack/Nack` + `Discover/Announce/Poll/Done`. Full table: `Node-Message-Model-Spec.md` §4. |

---

## 7. Buffering / memory budget (STM32G031F8: 8 KB SRAM total — same as the G030)

| Buffer | Size | Notes |
|---|---|---|
| RX frame buffer | `2 (sync) + 1 (len) + 3 (header) + MAX_DATA + 2 (crc)` | Recommend `MAX_DATA = 32` → 40-byte buffer. Revisit only if a specific message type genuinely needs more. |
| TX queue | Same struct as v1 (`messageQueue[queueSize]`), but each entry now needs to own/reference a variable-length payload | Simplest approach: fixed-size queue slots sized at `MAX_DATA` (wastes some RAM per slot but avoids dynamic allocation — appropriate on an 8 KB-RAM MCU). `queueSize = 25` from v1 × 40 bytes/slot ≈ 1 KB — fine. |
| Avoid heap allocation | — | No `malloc`/`new` for frame data; fixed-size slots only, matching v1's existing no-heap style. |

This keeps total protocol RAM usage well under 2 KB, leaving headroom for application state (endpoint values, timers) on the 8 KB part. Add the small `Diagnostics` log ring (`Node-Message-Model-Spec.md` §3, ~384 B) to this budget.

---

## 8. Migration notes from `NodeLib`

**Carries over almost unchanged (conceptually):**
- `Node` / `NodeMaster` class split and their `HandleMasterMessage` override pattern.
- Poll → flush → `Done` → advance round-robin state machine (`PollNextNode`, `ActiveNodeCount`, `activeNodes[]`).
- Discovery/staggered-reply mechanism.
- Heartbeat/`ConnectionLost()` contract (via `INodeHandler`, renamed from `IVariableHandler` — `Node-Message-Model-Spec.md` §6.2).

**Needs rework:**
- `Message`/`Id` structs: `Value` (single `uint8_t`) → variable-length `DATA` buffer + explicit `LEN`. Anywhere code does `message.value`, it now needs `message.data`/`message.len`, and typed values (int16, float) out of that buffer need explicit pack/unpack helpers per the `Node-Message-Model-Spec.md` §5 conventions.
- `Id.channel` (`ChannelId`, IO-pin selector) → `Id.endpoint` (`Endpoint`, flat named-endpoint enum) — `Node-Message-Model-Spec.md` §3.
- `Operation` enum redesigned (§6 above); the transport ops rename `DETECTNODES/HELLOWORLD/SENDQ/ENDOFQ` → `Discover/Announce/Poll/Done`.
- Dispatch: `NodeLib` now intercepts the `System*` / `Firmware` / `Diagnostics*` endpoints itself; the module handler sees only its own endpoints (`Node-Message-Model-Spec.md` §6.2).
- `Node::WriteMessage`/`Node::Loop()`: replace the fixed-size `Frame` struct + raw `Serial1.write((uint8_t*)&m, sizeof(m))` with a byte-stream framer/deframer that computes and appends/verifies CRC16 and handles the variable length + timeout-resync logic from §5.
- `setEnable()`: remove; replace with USART1 hardware DE configuration (one-time init, no per-message delay code).
- `numNodes`/`masterNodeId`/`nodeSpacing`: move from `static const int` compile-time constants to constructor/init parameters if you want this new firmware to support a different node count without recompiling the library itself (optional, but cheap to do now).

**New:**
- CRC peripheral init (polynomial, size, init value) as part of `Node::Init()`.
- Inter-byte timeout timer for the resync logic in §5 (a free hardware timer channel, or reuse the existing `DelayTimer` pattern from `tools/`).

---

## 9. Open decisions (need your input before finalizing)

1. **`MAX_DATA` cap** — 32 bytes assumed above; tell me if any planned message type needs more (e.g. streaming a batch of readings in one frame).
2. ~~**Baud rate**~~ — **250 000 baud.** ~100 m total bus (`Node-Bus-Hardware-Design-Spec.md` §7 item 3), 20 nodes, terminated both ends, indoor (~10–40 °C):
   - **Exact integer USART divisor** at both candidate kernel clocks (÷64 at a 16 MHz HCLK, ÷256 at 64 MHz). Zero baud-generator error — the crystal-less HSI16 clock (±~1 % indoors, so ~±2 % node-to-node) already spends most of the async-UART framing budget, so there is no room to add divisor error. 460 800 / 921 600 are not exact and 1 Mbit leaves too little margin against HSI16 spread plus the slower edges from each node's 10 Ω + SM712 + stub loading.
   - **Length·rate = 2.5×10⁷ bit·m/s** — an order of magnitude inside the conservative RS-485 knee (~10⁸) and far inside what the 12 Mbps MAX3485 does over 100 m of terminated pair. Bit period 4 µs vs. ~0.5 µs one-way cable delay → reflections settle in well under a bit.
   - A full 50 KB OTA image transfers in ~6 s (`Node-Flash-Layout-and-Bootloader-Spec.md` §6.2).
   - USART config: oversampling ×16 and the 3-sample majority vote (both defaults) for noise immunity; the §8 inter-byte timeout is specified in byte-periods so it scales automatically.
   - **500 000** (÷128, also exact) is the fallback headroom step, taken only if bench measurement on the real 100 m run shows margin. The `ControllerNode`↔`Thermostat` link runs the same 250 000 (`ControllerNode-Thermostat-Link-Spec.md` §3).
3. **CRC placement (whole-frame vs re-verify per field)** — spec above puts CRC after `DATA`, covering header+data only. Confirm that's acceptable vs. also covering `LEN` (would require restructuring since `LEN` is needed *before* you know where `DATA`/CRC end).
4. **Backward compatibility** — is this a clean-slate rewrite (old ATmega nodes retired), or do you need v1 and v2 nodes coexisting on the same bus during a transition? That changes whether `SYNC` bytes need to differ between versions so a mixed bus doesn't misparse frames.

---

*Happy to turn §3/§5 into actual C structs + a receiver state machine (`enum State { SYNC0, SYNC1, LEN, HEADER, DATA, CRC }`) once the open decisions above are pinned down — that's the natural next artifact.*
