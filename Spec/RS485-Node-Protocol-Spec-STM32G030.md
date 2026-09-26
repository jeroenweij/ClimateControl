# RS485 Node Bus — Protocol Design Spec (v2)
### Target MCU: STM32G031F8P6

**Companion docs:** `Node-Message-Model-Spec.md` (endpoints, operations, reporting model built on this framing), `Node-Bus-Hardware-Design-Spec.md` (physical layer), `Node-Flash-Layout-and-Bootloader-Spec.md` (the bootloader's use of this framing)

---

## 1. Goals

- Variable-length data field per message.
- CRC-protected frames — corruption is detected, not silently accepted.
- A round-robin master/slave state machine: `Discover` → `Announce` → `Poll` → `Done` → heartbeat.
- Uses STM32G0 peripherals (hardware CRC unit, USART auto-direction-control).

**Non-goals**
- Multi-master arbitration — single master, polled bus.
- Guaranteed delivery / retransmission at the protocol level — best-effort per round, recovered by the next poll cycle or heartbeat timeout.
- Encryption/authentication — the physical bus is trusted.

---

## 2. MCU / physical layer

| Item | Choice | Notes |
|---|---|---|
| MCU | STM32G031F8P6 | Cortex-M0+, 64 MHz max, 64 KB flash, 8 KB SRAM, TSSOP20. |
| UART | USART1 | Hardware Driver-Enable (DE) output — no manual GPIO toggle + delay needed. |
| Transceiver | THVD2410DR (TI), LCSC `C1849398` | ±70V fault-protected 3.3V half-duplex RS-485, 500 kbps, SOIC-8 (`Node-Bus-Hardware-Design-Spec.md` §6.3). DE/RE tied together, driven by USART1's DE pin. |
| Baud rate | **115 200** | Not an exact integer USART divisor (≈0.08% generator error, the same at the applications' 64 MHz and the bootloaders' 16 MHz — BRR 556 / 139) — negligible next to the crystal-less HSI16 clock's own spread. `1.15×10⁷ bit·m/s` for the ~100 m / 20-node terminated bus sits deep inside the safe region. 250 000 / 500 000 (both exact) are bench-validated fallbacks; 1 Mbit is not used. |
| CRC engine | Hardware CRC peripheral | Offloads CRC calc from the CPU. |

STM32G0's USART1 has `DEM`/`DEP` + `DEAT`/`DDAT` — an assertion/de-assertion time is configured in bit-periods and the peripheral toggles the DE pin around each transmission automatically, with no CPU-side delay loop.

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
| `SYNC` | 2 bytes | `0xEE 0x42`. Only scanned for while the receiver is *not* mid-frame. |
| `LEN` | 1 byte | Length of `DATA` only (0–255). Header (`NODE_ID`/`ENDPOINT`/`OPERATION`) is fixed size and not counted. Firmware caps this at `MAX_DATA` (§7). |
| `NODE_ID` | 1 byte | Target/source node address. `0` = master (reserved); `1..MAX_NODES-1` = slaves; `0xFF` = broadcast (`Set`-only, no reply) — see `Node-Message-Model-Spec.md` §2. |
| `ENDPOINT` | 1 byte | The addressable thing on the node — `NodeLib::Endpoint` enum, `Node-Message-Model-Spec.md` §3. |
| `OPERATION` | 1 byte | `NodeLib::Operation` — `Get`/`Set`/`Report`/`Ack`/`Nack` for endpoint access, plus the transport verbs `Discover`/`Announce`/`Poll`/`Done`. Full table in `Node-Message-Model-Spec.md` §4. |
| `DATA` | `LEN` bytes | Payload — interpreted per `ENDPOINT` (+ `OPERATION`) convention per `Node-Message-Model-Spec.md` §5. |
| `CRC16` | 2 bytes | Computed over `NODE_ID..DATA` inclusive (not over `SYNC` or `LEN`), little-endian on the wire. |

Total frame overhead is 7 bytes on top of the payload.

---

## 4. CRC

- **Algorithm:** CRC-16/CCITT-FALSE (poly `0x1021`, init `0xFFFF`, no reflect, no final XOR) — configurable directly on STM32G0's CRC peripheral (programmable polynomial size 7/8/16/32 bits, programmable polynomial value, programmable init value).
- **Coverage:** `NODE_ID`, `ENDPOINT`, `OPERATION`, and all `DATA` bytes. `SYNC` is excluded (a framing marker, not payload). `LEN` is covered indirectly — the receiver knows where `DATA` ends from it, but it is not itself included in the CRC computation; a corrupted `LEN` is caught by the CRC check failing on the resulting (wrong) byte range, with overwhelming probability, so a separate check on `LEN` itself isn't needed.
- **On receive:** compute CRC over the received header+data, compare to the trailing 2 bytes. Mismatch → drop frame, do not call any handler, let the round-robin/heartbeat mechanism recover on the next cycle.
- **Implementation:** feed bytes to `CRC->DR` as they arrive; read `CRC->DR` for the result. Reset (`CRC->CR |= CRC_CR_RESET`) before each frame.

---

## 5. Framing / resync strategy

Variable-length frames mean a corrupted `LEN` byte would otherwise make the receiver consume the wrong number of bytes and misinterpret the following, real frame. Mitigation:

1. **CRC catches almost all bad frames.** A corrupted `LEN` will, with overwhelming probability, cause the CRC computed over the (wrong) byte range to fail against the trailing 2 bytes, so the bad frame is rejected rather than silently accepted.
2. **Inter-byte timeout.** If more than `T_gap` (2–3 byte-periods at the configured baud, ~175–260 µs at 115 200) elapses between bytes while mid-frame, abandon the partial frame and return to sync-hunting. This bounds how long a single corruption event can wedge the parser.
3. **Max frame guard.** If `LEN` (or a corrupted read of it) would make the frame exceed the configured max, abandon and resync immediately rather than blocking on bytes that will never come in the expected window.
4. **`SYNC` re-armed continuously in payload search** — even while "in frame," if the resulting frame fails CRC, the next sync search starts from the byte immediately after the failed attempt's `SYNC`, not from scratch at the buffer start, so a spuriously-matched `SYNC` inside a garbled stream doesn't cost more than one bad frame.

**Why not COBS/byte-stuffing:** on a short, low-noise wired bus with a small, closed set of nodes, CRC + timeout-based resync is simpler to implement and debug on a Cortex-M0+ with 8 KB RAM, and the failure mode is "occasionally drop one frame, recovered next poll cycle" rather than "corrupt state." A longer/noisier bus or a much higher node count would be the trigger to revisit this.

---

## 6. Addressing, topology, operations

| Item | Value |
|---|---|
| Master ID | `0`, reserved |
| Node ID range | `1 .. MAX_NODES-1` (`NodeLib::MAX_NODES`, currently 21). Each node's ID is factory-provisioned in flash and read-only — `Node-Flash-Layout-and-Bootloader-Spec.md` §6.3. |
| Broadcast | `NODE_ID = 0xFF`, valid with `Operation::Set` only (fire-and-forget, no reply) — `Node-Message-Model-Spec.md` §2 |
| Discovery | Master broadcasts `Discover`; slaves reply with a staggered `Announce` (`(nodeId-1) * nodeSpacing` ms), carrying module type + 96-bit UID for the master's roster. |
| Poll cycle | Master → `Poll` → node dumps its queued `Report`s (on-change + keepalive, `Node-Message-Model-Spec.md` §6.1) → `Done` → master polls next active node. |
| Heartbeat | Master-side timer reset each full poll round; `ConnectionLost()` on lapse. |

---

## 7. Buffering / memory budget (8 KB SRAM total)

| Buffer | Size | Notes |
|---|---|---|
| RX frame buffer | `2 (sync) + 1 (len) + 3 (header) + MAX_DATA + 2 (crc)` | `MAX_DATA = 35` → 43-byte buffer. Sized for `Firmware[Write]`'s payload (`Node-Flash-Layout-and-Bootloader-Spec.md` §6.2): `1 (FirmwareOp) + 2 (byteOffset) + 32 (data) = 35`, exactly, no slack — the next-largest message type needs nowhere near this much. |
| TX queue | Fixed-size queue slots sized at `MAX_DATA` (`queueSize = 25` × 43 bytes/slot ≈ 1.05 KB) — no dynamic allocation. |
| Heap allocation | None — fixed-size slots only, no `malloc`/`new` for frame data. |

Total protocol RAM usage is well under 2 KB, leaving headroom for application state (endpoint values, timers) on the 8 KB part. The `Diagnostics` log ring (`Node-Message-Model-Spec.md` §3, 320 B) adds to this budget.

---

*The natural next artifact once framing changes are needed: a receiver state machine (`enum State { SYNC0, SYNC1, LEN, HEADER, DATA, CRC }`) implementing §3/§5 directly.*
