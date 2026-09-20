# Node Flash Layout, Bootloader & Persistent Identity — Design Spec

**Status:** Draft — first pass 2026-09-08. Partition map and OTA transport approach proposed here; decisions in §8 need your confirmation before any of this is built. **2026-09-20: §6.2 (the transfer protocol) was found buggy from bench testing; §6.2.1 (the v2 redesign) is now implemented and bench-verified** — a full OTA push against the real bench `TemperatureNode` completed end to end (`writing` progressed cleanly to 20152/20152 bytes, whole-image CRC32 verified, `Activate` succeeded, node reported back online running the new image with no fault). In `Software/Lib/NodeLib`, `Modules/Bootloader/FirmwareSlave.{h,cpp}`, `Modules/ControllerNode/{ThermostatLink,ControllerHandler}.{h,cpp}`, and mirrored in `Webserver/internal/nodelib`/`internal/service/ota.go`. §8 item 9 still has a few open sub-decisions (ack-timeout tuning, whether `Begin`/`End`/`Abort` also move to `Ack`/`Nack`).
**Companion docs:** `RS485-Node-Protocol-Spec-STM32G030.md` (the v2 wire protocol the bootloader speaks a subset of), `Node-Bus-Hardware-Design-Spec.md` §6.1 (BOOT0/option-byte state, `NRST`), `Software-Architecture-Spec.md` (module map this adds to), `MainController-Spec.md` §4 item 4 (persistence — this spec answers "where"), `ControllerNode-Thermostat-Link-Spec.md` (Thermostat is off the main bus — §7 here), `Node-Message-Model-Spec.md` §4 (the `Ack`/`Nack` operations §6.2.1 below reuses), `OTA-Debugging-TODO.md` (the bench findings that motivated §6.2.1)

---

## 1. Scope & goals

Three things, one flash design:

1. **A bus-resident bootloader** that speaks enough of `NodeLib` v2 to receive a firmware image over the main RS485 bus and program it — so a `ControllerNode` / `TemperatureNode` in a duct never needs a J-Link for a firmware update.
2. **The application image** itself — same STM32G031F8P6, linked to sit above the bootloader.
3. **Persistent node identity** — a wear-safe, power-fail-safe place to keep the node's bus address (`NodeId`) and a small amount of per-node config, so a given physical node keeps the same address across reboots and firmware updates.

**Goals**
- Never permanently brick a node through a failed/interrupted update, as long as the bootloader region and the bus are intact.
- Bootloader is small, self-contained, and **never updated over the bus** (only via SWD) — it is the recovery anchor.
- Same bootloader binary on all four boards; the flash map is identical MCU-wide (`STM32G031F8P6`, locked 2026-09-08).
- Reuse what already exists: the `Frame`/`Crc`/`Id`/`Message` framing layer, `Lib/HAL`, `Lib/Board`.

**Non-goals (v1)**
- Image signing / encryption — the bus is assumed trusted, same stance as `RS485-Node-Protocol-Spec-STM32G030.md` §1. Flagged in §8 as a later option (flash RDP).
- Dual-slot A/B images — confirmed 2026-09-08: **single 50 KB app slot**, with the resident bootloader as the recovery path (§5). Revisit only if images that pass CRC yet fail to run become a real risk.
- Delta/compressed images.
- A field-modifiable `NodeId` — confirmed 2026-09-08: identity is written **once at factory** and is read-only to firmware (§6.3).

---

## 2. STM32G031F8P6 flash geometry (RM0444)

| Property | Value |
|---|---|
| Main flash | 64 KB @ `0x0800_0000` – `0x0800_FFFF` |
| Page size / count | **2 KB × 32 pages** (erase granularity = one page) |
| Program granularity | **64-bit double-word**, to previously-erased flash only |
| SRAM | 8 KB @ `0x2000_0000` (bootloader and app never run at the same time — each may assume the full 8 KB) |
| 96-bit unique ID | read-only at `0x1FFF_7590` (used for provisioning, §6.3) |
| Backup registers | `TAMP->BKPxR` (5 × 32-bit) — retained across a warm reset (`NRST`, software reset, IWDG), **cleared on power-on / brown-out**. Exactly the semantics wanted for the app→bootloader handoff (§5). No VBAT battery fitted, so these do not survive the ENABLE-line power cut — deliberately. |

---

## 3. Flash partition map

```
0x0800_0000  ┌────────────────────────────┐
             │  Bootloader                │  10 KB  (5 pages)   — SWD-flashed only, never OTA
0x0800_2800  ├────────────────────────────┤
             │  Application               │  50 KB  (25 pages)  — OTA target; vector table at 0x0800_2800
             │                            │
0x0800_F000  ├────────────────────────────┤
             │  Config page A             │  2 KB   (1 page)    ┐ ping-pong record log
0x0800_F800  ├────────────────────────────┤                    │ (persistent NodeId + per-node config)
             │  Config page B             │  2 KB   (1 page)    ┘
0x0801_0000  └────────────────────────────┘
```

| Region | Base | Size | Written by | Notes |
|---|---|---|---|---|
| Bootloader | `0x0800_0000` | 10 KB | J-Link only | Holds the reset vector; runs first on every boot. Carries the full OTA slave (main bus + the Thermostat link on USART2, `ControllerNode-Thermostat-Link-Spec.md` §5.5), which needs the 5th page. |
| Application | `0x0800_2800` | 50 KB | Bootloader (OTA) | Linked with `FLASH ORIGIN = 0x08002800`. First 0xC0 bytes = vector table; image descriptor at fixed offset `0xC0` (§4). The biggest module uses about half the slot. |
| Config A | `0x0800_F000` | 2 KB | J-Link at factory only | A single static `ConfigRecord` at the page base — `NodeId` + per-node factory config. **Read-only to firmware** (§6.3). |
| Config B | `0x0800_F800` | 2 KB | — (reserved) | Spare page, unused in v1 — held back for a future *runtime-writable* setting, which would turn A/B into a ping-pong log. |

`Lib/Board/MemoryMap.h` (new, header-only) is the single source of truth for these four constants; bootloader linker script, app linker script, `ConfigStore`, and the flash-program HAL all include it.

---

## 4. Application image format

The app is linked at `0x0800_2800`. Immediately after the Cortex-M0+ vector table (48 entries = `0xC0` bytes on the G031) the linker places a 32-byte **image descriptor** in a `.image_descriptor` section at fixed offset `0xC0`:

```cpp
struct __attribute__((packed)) ImageDescriptor  // 32 bytes
{
    uint32_t magic;          // 0x43436D67  ('CCmg') — ClimateControl image
    uint16_t headerVersion;  // 1
    uint16_t module;         // 1=ControllerNode 2=TemperatureNode 3=MainController 4=Thermostat
    uint32_t imageSize;      // bytes from 0x08002800 to end of image, INCLUDING the trailing CRC32
    uint16_t fwVersionMajor;
    uint16_t fwVersionMinor;
    uint32_t buildId;        // git short hash, for traceability
    uint8_t  reserved[8];
};
```

- The **last 4 bytes** of the image are a `CRC32` over bytes `[0x08002800, 0x08002800 + imageSize - 4)`, appended by a post-build step on the `.bin`. **Decided 2026-09-08: CRC32, not CRC16** — a whole-image CRC16 has a ~1/65536 miss probability, not good enough for a 50 KB image; CRC32 is effectively free on the STM32 CRC unit. Config: STM32 CRC-unit **native** mode — poly `0x04C11DB7`, init `0xFFFF_FFFF`, **no input/output bit-reversal, no final XOR** (the post-build tool is set to match; this side-steps the zlib-vs-STM32 reflection mismatch). Needs a CRC32 entry point added to `Hal::Crc` (today CCITT-16 only).
- **Bootloader validity check** — the *only* gate on whether the app runs, checked on every boot (a CRC32 sweep of ~50 KB by the hardware CRC unit, a few ms): `magic` matches **and** trailing CRC32 matches **and** `imageSize <= 50 KB`. Pass → runnable. Fail → stay in the bootloader (§5). No separate "app valid" flag — the CRC32 is it.
- The OTA `Begin` command (§6.2) carries `imageSize` and the expected CRC32 so the bootloader can reject a wrong-size or truncated push early, and so the master (which parsed the `.bin`) and the node agree.

### 4.1 Jump to application

Bootloader, once the app is validated and no "stay in bootloader" condition holds:

1. Deinit what it touched: `USART1` (disable + reset), CRC unit, SysTick IRQ, any GPIO AF back to analog, `HAL_RCC_DeInit()` back to HSI.
2. `SCB->VTOR = 0x0800_2800;`
3. `__set_MSP(*(uint32_t*)0x0800_2800);`
4. Jump to `*(uint32_t*)0x0800_2804` (app reset vector).

The app does a full `Hal::System::Init()` + clock + peripheral bring-up as if from cold — it must not assume any bootloader state.

---

## 5. Boot decision flow

```
POR / any reset
      │
      ▼
Bootloader entry (0x08000000)
      │
      ├─ TAMP->BKP0R == ENTER_BL_MAGIC ?  ── yes ─┐   (app asked for it, §6.2 op 0x06)
      │        │ clear BKP0R                       │
      │        no                                  │
      ▼                                            │
   App image valid? (§4)  ── no ──────────────────►├─►  Stay in bootloader:
      │ yes                                         │      run minimal NodeLib slave (§6.1),
      ▼                                             │      wait for OTA from master
   (optional) boot-fail counter over threshold? ───┘
      │ no
      ▼
   Jump to app (§4.1)
```

- **App→bootloader handoff:** app receives `EnterBootloader` (§6.2), writes `ENTER_BL_MAGIC` to `TAMP->BKP0R`, calls `NVIC_SystemReset()`. Backup register survives the warm reset; bootloader consumes and clears it.
- **Interrupted update is self-healing:** if power drops mid-write, the app region fails its CRC32 check on next boot, so the bootloader stays resident regardless of any flag, re-announces on the bus, and the master restarts the push.
- **Manufacturing:** a board flashed with *only* the bootloader (+ a provisioning record, §6.3) has no valid app → it comes up in the bootloader and the master loads the app over the bus. Field boards can be built app-less.
- **Optional boot-fail counter** (§8 item 7): bootloader increments a `TAMP->BKP1R` counter before each jump; the app clears it once it has run healthily for N seconds. Over threshold → stay in bootloader. Not in v1 unless you want it.

---

## 6. OTA over NodeLib

### 6.1 What the bootloader implements

Only entered when `ConfigStore::Valid()` (a provisioned node — see §7). A hand-written minimal slave loop — **framing layer only**, no `NodeMaster`, no `std::stringstream` `Logger` (too big for the 10 KB region):

- Reads its bus address from `ConfigStore` (§6.3) — same `NodeId` the app uses, so addressing is stable across the app↔bootloader transition.
- Responds to `Discover` with `Announce` (so the master sees it and knows it is in the bootloader).
- Serves one endpoint, `Endpoint::Firmware` (`Node-Message-Model-Spec.md` §3), plus the `Transport` plumbing.
- Transmits only when polled (`Poll`) — identical bus discipline to a normal slave, so the master's round-robin is undisturbed and other nodes keep running while one updates.

This requires `Lib/NodeLib` to be split as `Software-Architecture-Spec.md` §1 already calls for: a framing sub-library (`Frame`/`Crc`/`Id`/`Message`/`EEndpoint`/`EOperation`, logging compiled out) that the bootloader links, plus the `Node`/`NodeMaster` layer on top for the apps.

### 6.2 Transfer protocol

All OTA messages target `Endpoint::Firmware`. `data[0]` is a `FirmwareOp` sub-opcode; the transfer is **strictly sequential** (no out-of-order buffering, no bitmap) so the bootloader needs only **one 2 KB page buffer** in RAM. This is the "richer command set expressed as one endpoint acted on with `Set`/`Report`" pattern from `Node-Message-Model-Spec.md` §4 — the `Operation` verb set does not grow for OTA.

**Master → node** (`Operation::Set`, `data[0]` = `FirmwareOp`):

| `FirmwareOp` | name | payload (`data[1..]`) | node action |
|---|---|---|---|
| `0x01` | `Begin` | `module`(1) · `imageSize`(4 LE) · `imageCrc32`(4 LE) · `fwVersion`(2) | Confirm in bootloader; sanity-check size; **erase all 26 app pages**; reset write pointer to 0. |
| `0x02` | `Write` | `byteOffset`(4 LE) · `bytes`(≤ 27) | If `byteOffset == expectedOffset`: append to page buffer; on crossing a 2 KB boundary, program that page (double-words) and advance. Else: drop silently (master will rewind). |
| `0x03` | `End` | — | Program the final partial page; verify trailing CRC32 over `[base, base+imageSize-4)`; set status to `bl-valid` or `bl-crcfail`. |
| `0x04` | `Activate` | — | Ensure `BKP0R` is clear; `NVIC_SystemReset()` → boots the new app (now valid by its CRC32, §4). |
| `0x05` | `Abort` | — | Discard; status back to `bl-idle` (app region left erased/invalid — master must retry). |
| `0x06` | `EnterBootloader` | — | **Handled by the running app** (via the `NodeLib` `Firmware` interceptor + `INodeHandler::PrepareForReset()`): park outputs, write `ENTER_BL_MAGIC` to `BKP0R`, `NVIC_SystemReset()`. |

**Node → master** (`Operation::Report`, `data[0]` = `FirmwareOp::Status` `0x07`, queued, sent on the next `Poll`):

| field | bytes | |
|---|---|---|
| `state` | 1 | 0=app · 1=bl-idle · 2=bl-erasing · 3=bl-receiving · 4=bl-valid · 5=bl-error |
| `expectedOffset` | 4 LE | next byte offset the node wants |
| `lastError` | 1 | |
| `runningFwVersion` | 2 | |

**Flow:** `Begin` → poll until `state==bl-receiving` → stream a batch of `Write` frames (roughly a page's worth, then poll) → on each `Status`, if `expectedOffset` didn't advance as far as sent, **rewind and resend from `expectedOffset`** → repeat to `imageSize` → `End` → poll for `bl-valid` → `Activate`.

**Sizing:** 50 KB ÷ 27 B/frame ≈ 1900 `Write` frames; a full-size update was estimated at **~6 s at the (then-assumed) 250 000 baud bus rate** (`RS485-Node-Protocol-Spec-STM32G030.md` §9) — stale now that the bus runs 115 200 baud and `Write` is a synchronous per-frame Ack/Nack (§6.2.1); not re-derived here. The rest of the bus keeps polling normally throughout.

This subsection describes v1 as originally designed, with `MAX_DATA = 32` and a 27-byte `Write` payload (`32 − 1 FirmwareOp − 4 offset`). **Decided 2026-09-20: `MAX_DATA` grows to 35 and the `Write` payload changes shape — see §6.2.1, which supersedes the `Write` row of the table above and the sizing note in this paragraph.**

**Status 2026-09-20: this section describes the protocol as originally designed and as first implemented. It has bugs — see §6.2.1.** Bench testing (`OTA-Debugging-TODO.md`) found two distinct failure modes in the built system: a report going uncredited near the very end of an otherwise-successful transfer (a timing/latency problem, §6.2.1 fixes this by construction), and — more seriously — a transfer that reported reaching 100% but failed the final CRC32 check, with no wire-level frame CRC failures recorded anywhere in the session (genuine data corruption, not a lost/garbled frame). §6.2.1 is a proposed redesign closing both gaps; §8 has the specific decisions still needed before it's built.

### 6.2.1 Transfer protocol v2 — synchronous per-write acknowledgement (implemented 2026-09-20)

**Problem with §6.2 as built.** The node never transmits anything except in reply to the ordinary bus `Poll` (`Node-Message-Model-Spec.md` §6.1: "the queued `Report` goes out on the next `Poll`") — a `Write`'s only observable effect is `statusPending = true`, and the resulting `Status` report rides whatever `Poll` the master's round-robin happens to send next, at some indeterminate time later. Two consequences, both seen on the bench:

1. **The reply carries no correlation to the write that caused it.** The server's retry logic (`otaReportWait` in `Webserver/internal/service/ota.go`) waits for *a* report to show up on a channel filtered by node+endpoint, not *the* reply to *this* write — a report just states the node's current `expectedOffset`, a raw counter with no reference to which specific write, or which specific attempt, produced it. A lost ack and a lost write are indistinguishable from the server's side, and if two reports were ever in flight close together there's no way to tell which is "for" which request.
2. **The only data-integrity check is the whole-image CRC32 in `HandleEnd()`**, at the very end — by the time that fails, ~1900 writes have already happened and there is no way to tell which one was wrong.

(An earlier draft of this section also argued the reply's *timing* was unbounded because a full round-robin sweep could take seconds on a bus with many configured-but-inactive node IDs. That reasoning was wrong — see the "Bus scheduling" note below — and isn't needed to motivate this change; the correlation and integrity problems above are the real reasons.)

**The fix: give each `Write` a reply that's actually *about* that write, not a generic status counter.** `Node-Message-Model-Spec.md` §4 already has the right primitive for this and it's simply unused here — `Ack`/`Nack`, marked bidirectional (`↔`), distinct from `Report`'s generic value-delivery role. Delivery timing is unchanged from today (queued, goes out on the node's next `Poll` — see "Bus scheduling" below); what changes is the *content*, so the reply can finally be tied to one specific write and can attest to the integrity of that write's data:

- **Master → node**, `Set(Firmware, Write)`, **payload changes shape — decided 2026-09-20**: `byteOffset`(**2 LE, uint16** — was 4; the 50 KB app slot's max offset, 51199, fits comfortably) · `bytes`(**32, fixed** — was ≤27; the final chunk of the whole image may be shorter, see "Flash-verification granularity" below). `1 (FirmwareOp) + 2 (byteOffset) + 32 (bytes) = 35` — exactly `MAX_DATA`'s new value (`RS485-Node-Protocol-Spec-STM32G030.md` §7/§9), with no slack. This is what drove `MAX_DATA` from 32 to 35 in the first place: 32 usable data bytes is required for every chunk to be double-word-aligned (see below), and there was no way to fit that plus a useful offset field in the old 32-byte ceiling.
- **Node → master**, queued exactly as a `Report` is today and delivered on the node's next `Poll`: `Ack(Firmware)` if the chunk was accepted, `Nack(Firmware)` if not, both carrying:

  | field | bytes | |
  |---|---|---|
  | `byteOffset` | 2 LE | the offset this ack/nack is *for* — echoes the request, so a reply is unambiguously tied to one write even if the server has more than one attempt in flight |
  | `chunkCrc16` | 2 LE | CRC16 (`Hal::Crc::Poly::Ccitt16`, the same peripheral config `Frame` already uses) over the flash bytes just read back at this offset (see the "Flash-verification granularity" note below — this requires shrinking the chunk so every write is independently flashable) |
  | `programFailed` | 1 | `0` = `Hal::Flash::Program()` reported success; `1` = it reported a failure (`FLASH_SR` error flags) — distinct from a `chunkCrc16` mismatch, which the server detects itself by comparing against the CRC it computed before sending |

  This ack payload (5 bytes) is far under `MAX_DATA` either way — 35 is sized for the `Write` request, not the reply. The server compares `chunkCrc16` against the CRC it computed over the exact bytes it sent for `byteOffset` *before* sending — a mismatch means the node staged the wrong bytes (a receive/logic bug, independent of flash), and the server can immediately resend just that one chunk instead of discovering the problem ~1600 writes later with no idea which one was at fault.

- **Extend, don't just apply, `HandleWrite`'s existing sequencing guard — though its justification changes, and it becomes load-bearing for a hard hardware rule, not just a RAM-buffer convenience.** Today, `offset == expectedOffset` exists because `AppendImageBytes`'s single 2 KB page buffer only knows how to accumulate bytes contiguously. If chunks shrink to 24/32 bytes and get programmed independently (see "Flash-verification granularity" below), that specific reason goes away — each `Write` carries its own explicit destination address (`AppBase + byteOffset`) and doesn't depend on buffer state. But a *stricter* reason to keep tracking the frontier takes its place: **STM32G0 flash allows a given double-word address to be programmed exactly once per erase cycle — not a wear/count limit, a hard one-shot rule.** Per ST's own confirmation of this flash IP's behavior: "Programming a previously programmed address with a non-zero data is not allowed. Any such attempt sets `PROGERR` flag of the FLASH status register (`FLASH_SR`)" — this fires even for byte-identical data. So a resend of an already-applied chunk must be answered **without ever calling `Hal::Flash::Program()` again for that address** — the guard's job is "is this address already programmed (answer from a read-back, never touch `Program()`) or genuinely new (safe to program)," not "would re-programming this be harmless." Silently dropping a mismatched write with no reply at all, as today, is no longer acceptable either way, because the server now treats every write as expecting a reply:
  - `offset` is behind `expectedOffset` (a duplicate — the write landed, only its ack was lost): **read back** the flash bytes already at that address, derive `chunkCrc16` from that read, and `Ack` with it — `Flash::Program()` is not called.
  - `offset` is ahead of `expectedOffset` (a genuine gap — should not happen if the server only ever sends the next expected chunk, but must be handled): `Nack` with `expectedOffset` in place of the echoed offset, so the server can resync without guessing.

**Lost-ack recovery, not blind resend.** If the server's (short — see §8) ack-timeout for a write fires, it must not blindly resend the same `Write` and hope the node's guard catches it — it retries the *same* `Write` frame, and correctness now depends entirely on the guard above routing a resend to the read-back path rather than `Program()`. Get this wrong and it's not wasted flash wear, it's a hardware `PROGERR` on real firmware data. Retries are bounded (small fixed count) before declaring the chunk failed. This is a simplification over the current code's "resend triggers a `Get` probe instead of the data" workaround (`ota.go`'s comment on `otaWriteRetries`) — that workaround existed specifically because a resent `Write` used to vanish into the silent-drop case with no way to tell whether it landed; once every `Write` gets an explicit `Ack`/`Nack` naming the offset it's about, resending the actual data is no longer ambiguous, provided the guard is implemented correctly.

**Flash-verification granularity — `chunkCrc16` should attest to actual flash, not just the RAM copy, which means shrinking the chunk. Decided 2026-09-20: 32 data bytes, via `MAX_DATA=35` + a 2-byte offset (§8 item 9, resolved).** As specified above, `chunkCrc16` covers whatever the node "staged for this offset" — but `AppendImageBytes` today only stages bytes in a RAM buffer and calls `Hal::Flash::Program()` once a full 2 KB page (or the final partial page, at `End`) is ready, because `Program()` only writes 64-bit double-words to previously-erased flash and today's 27-byte chunk isn't double-word-aligned. A CRC over the RAM copy alone can't tell you the flash programming itself succeeded.

The fix: since `AppBase` (`0x0800_2800`) is double-word-aligned and chunks are sent strictly sequentially, a chunk size that's itself a multiple of 8 makes *every* chunk's start address a multiple of 8 automatically. **32 bytes** (4× 8-byte double-words) is the chosen size — the largest multiple of 8 that fits once the `byteOffset` field shrinks to 2 bytes (`uint16`, the 50 KB app slot fits easily), giving `1 (FirmwareOp) + 2 (offset) + 32 (data) = 35 = MAX_DATA`, exactly, no slack. Every `Write` becomes independently, immediately programmable on its own, with no RAM staging buffer needed at all — `AppendImageBytes`/`pageBuffer`/`FlushPage` go away entirely, and `chunkCrc16` on *every* ack becomes a true post-`Program()` flash read-back CRC — the node reads back the bytes it just wrote and CRCs those, not its RAM copy. `pageError` becomes unnecessary and is dropped from the ack payload in favor of `programFailed` (see the payload table above). Cost: for a real image (not the full 50 KB slot — e.g. the 19868-byte `TemperatureNode` build used on the bench this session), chunk count is `ceil(imageSize / 32)`, with the final chunk short whenever `imageSize` isn't a multiple of 32 (the normal case, not a rare edge — 19868 / 32 = 620 full chunks + one 28-byte final chunk). Fewer chunks either way than v1's `ceil(imageSize / 27)` despite the bigger header, since the freed offset bytes buy more usable data per frame than `MAX_DATA` grew by. The whole-image CRC32 check in `HandleEnd()` stays exactly as it is regardless — a final backstop that doesn't depend on any of this.

**Partial-program failure within a 32-byte chunk — a real hazard, node-side bookkeeping resolves it without giving up the wire efficiency.** A 32-byte chunk is 4 double-words programmed in sequence by one `Hal::Flash::Program()` call. Its loop (`for (...; ok) { ...; ok = Finish(); }`) stops at the *first* double-word that fails — so a partial failure is always a clean prefix: some leading double-words in the chunk are now permanently programmed (one-shot, per the hardware rule above), the rest were never attempted. If a retry of that same 32-byte offset naively called `Program()` again over the full 32 bytes, it would hit `PROGERR` on the already-succeeded leading double-words. **The node must track, per in-flight chunk, how many of its double-words are already committed, and resume programming only from the first not-yet-done one on any retry of that offset — never re-touching ones already locked in.** This is internal to the node; the wire protocol (`Write`/`Ack` always at the full 32-byte granularity) doesn't change, and the server never needs to know about this finer-grained state — it always just resends the whole 32-byte chunk, and the node's own bookkeeping guarantees that's safe.

**Server-side chunking bug — `chunkCrc16` can't catch it, needs its own check.** Everything above (`chunkCrc16`, frame CRC16, `PROGERR` detection) verifies that what the node put in flash matches what the *server sent*. None of it verifies that what the server sent for a given `byteOffset` was actually the *correct* slice of the uploaded image — if `ota.go`'s chunking loop has a bug (an off-by-one, a stale loop variable, a bad rewind after a `Nack`) and sends the wrong 32 bytes for an offset, the server's own pre-send CRC and the node's post-program CRC agree perfectly, because they're both computed over the same (wrong) bytes. This is a distinct failure class from anything discussed above, and it went un-checked in v1 too — the frame CRC16 (confirmed via the bench session's `crcErrors=0` even while Finding C's corruption happened) only ever protected the wire hop, never the server's own slicing logic. Two cheap, independent checks close this:

1. **Slice from the wire value, not a parallel loop variable.** The chunk bytes sent must be derived by re-slicing `d.image[byteOffset : byteOffset+32]` using the *exact* `byteOffset` value being written into the `Write` message's offset field — not from a separately-incremented loop counter (like today's `o`) that merely *should* track the same value. If those two ever need to be the same variable by construction, they can never silently drift apart. This is a one-line implementation discipline, not a runtime check, but it's the difference between "this bug class is structurally impossible" and "this bug class is merely unlikely."
2. **Distinguish the failure signature on a final CRC32 mismatch.** If every chunk's `chunkCrc16` matched what the server expected for the entire job (the normal case — a real per-chunk mismatch would already have triggered a mid-job retry/abort), *and yet* the whole-image CRC32 in `HandleEnd()` still fails, that specific combination is the signature of a server-side chunking bug, not a node/flash bug: the data was self-consistent end-to-end (every chunk verified against what the server believed it sent) but still wrong relative to the true source. The server should report this as a distinct, specifically-worded error (e.g. "image verified chunk-by-chunk during transfer but failed final CRC — check the server's chunking logic, not the node") rather than the generic CRC-mismatch message, so a future debugging session isn't pointed at the node/flash path the way this session's Finding C investigation was, when the actual bug is upstream.
3. **Re-verify the in-memory image once per job, before sending anything.** `d.image` is validated (via `nodelib.ParseImage()`, CRC32 computed and stored) once at upload time; a cheap, one-time re-check that its CRC32 still matches that stored value immediately before a job starts streaming chunks catches any corruption of the in-memory/on-disk copy between upload and use, separately from anything about the chunking loop itself.

**Bus scheduling — use the existing queue/`Poll` delivery, not a new transport mode.** An earlier draft of this section assumed the master's round-robin pays a `pollTimeoutMs` for every *configured* node ID, active or not, and worried a bench-like setup (few real nodes, many provisioned-but-absent IDs) would make per-write latency unacceptable. That's wrong: `NodeMaster::PollNextNode()`'s node-selection loop (`Software/Lib/NodeLib/NodeMaster.cpp:148-156`, `do { nodeId++; ... } while (!slaveNodes[nodeId-1].active)`) only ever stops on a node already flagged `active` — an unprovisioned/undetected ID is skipped for free, no timeout paid. So the round-robin already only cycles through genuinely active nodes, and returns to the OTA target about as often as there are other active nodes to interleave with — fast in practice, not the multi-second sweep the earlier draft assumed. **Decision: for now, `Ack`/`Nack` rides the existing "queue a reply, deliver on the node's next `Poll`" mechanism, unchanged — no new "Write grants an immediate reply window" transport behavior, no bus-dedication mode.** This keeps §6.1's "other nodes keep running while one updates" property exactly as-is, with no special-casing. Revisit only if a real deployment with many simultaneously-active nodes makes the natural round-robin cadence a genuine bottleneck — not a concern to design around speculatively now.

### 6.3 Persistent identity — `ConfigStore` (read-only at runtime)

**Decided 2026-09-08:** the `NodeId` is written **once, at factory provisioning, via J-Link** and is **read-only** to every firmware image thereafter. No wire command changes it, no field re-assignment, no ping-pong log — the config region is programmed at manufacture and only ever read after that. This is simpler and removes a whole class of "who owns the address" questions.

`Lib/NodeLib/ConfigStore.{h,cpp}` (new) is a **read-only accessor**, used by both bootloader and app:

```cpp
struct __attribute__((packed)) ConfigRecord  // 32 bytes, single instance at 0x0800_F000
{
    uint32_t magic;         // 0x4E4F4443  ('NODC')
    uint16_t schemaVersion; // 1
    uint8_t  nodeId;        // 1 .. NodeLib::MAX_NODES-1 — the node's permanent bus address
    uint8_t  module;        // 1=ControllerNode 2=TemperatureNode 3=MainController 4=Thermostat
    uint8_t  settings[16];  // per-node factory config: servo end-stop trim, room id, sensor offset, ...
    uint8_t  reserved[4];
    uint32_t crc32;         // standard reflected CRC-32 (zlib/PNG) over bytes [0..27]
};
```

The record CRC is a small **table-less software CRC-32** (zlib flavour — poly `0xEDB88320`, init/final `0xFFFF_FFFF`), not the hardware CRC unit: it runs once over 28 bytes at boot, and keeping it self-contained means `ConfigStore` has no dependency on the shared CRC peripheral (which `Frame` owns) and the PC-side provisioning script can use `zlib.crc32` directly. (The §4 whole-image check is a different, larger job and does use the HW unit.)

```cpp
class ConfigStore
{
  public:
    enum class Module : uint8_t { Unknown, ControllerNode, TemperatureNode, MainController, Thermostat };

    static bool           Valid();      // magic + crc32, and 1 <= NodeId < MAX_NODES, on the record at 0x0800_F000
    static uint8_t        NodeId();     // only meaningful when Valid()
    static Module         GetModule();
    static const uint8_t* Settings();   // 16 bytes, caller interprets per GetModule()
};
```

**Implemented 2026-09-08** (`Lib/NodeLib/ConfigStore.{h,cpp}`, `Lib/Board/MemoryMap.h`): `NodeLib::Node::Init()` now reads its address from `ConfigStore` when it is not the master; an invalid/blank record calls `ErrorHandler::Error(false)` (never returns — solid error LED). `Node::SetId()` is removed. `NodeMaster` is unchanged — it hard-codes id `0` in its constructor, and `Node::Init()` skips the flash read for it.

- **Unprovisioned / corrupt record** (blank `0xFF` page, bad magic, bad CRC): the node does **not** join the bus. `NodeLib::Node::Init()` raises a non-recoverable `ErrorHandler::Error(false)` (error LED solid) — a board that reached the field un-provisioned is a manufacturing escape, not something to paper over with a default address.
- **Provisioning:** a CMake `provision` target generates the 32-byte `ConfigRecord` blob (given `nodeId` + `module` + optional settings) and `JLinkExe` writes it to `0x0800_F000` — same tool and bench step as flashing the bootloader. The bootloader and app are then identical across units; only this one page differs.
- **`NodeLib` changes are minimal:** `Node` reads `nodeId` from `ConfigStore` at `Init()` instead of the current hard-coded `nodeId(99)` + external `SetId()`. No discovery changes — a provisioned node answers `Discover` with its stored id (in the `Announce` payload) exactly as before. (`Endpoint::Firmware` and the redesigned `Operation` verbs come from `Node-Message-Model-Spec.md`, not this spec.)

---

## 7. Per-board notes

**One bootloader binary, all four boards** (confirmed 2026-09-08). The master and the nodes are updated by different mechanisms, but that does **not** split the bootloader — it branches at runtime on `ConfigStore::Valid()`:

| bootloader is running on | `ConfigStore::Valid()` | stay-resident behaviour |
|---|---|---|
| a provisioned `ControllerNode` / `TemperatureNode` | true (has a `NodeId`) | run the RS485 OTA slave loop (§6), addressed at `ConfigStore::NodeId()` |
| `MainController` | false — the master has no node identity, so no `ConfigRecord` is written for it | passive wait; it updates itself app-assisted over NINA (§7.1), recovery is SWD |
| an unprovisioned node | false | passive wait; needs the bench (can't do addressed OTA without an address) |

The same check that decides *"can I be a bus node"* decides *"can I receive OTA over the bus"* — no board-specific code, no compile switch. `main.cpp` is identical on every unit; only the factory `ConfigRecord` (or its absence, on the master) differs. Base skeleton implements this branch today; the RS485 slave loop itself is still stubbed.

| Board | OTA path |
|---|---|
| `ControllerNode`, `TemperatureNode` | Main bus, exactly as §6. |
| `Thermostat` | **Not on the main bus** (`ControllerNode-Thermostat-Link-Spec.md`). This same bootloader binary serves the image over the point-to-point link — `OtaUart` selects USART2 when `ConfigStore::GetModule() == Thermostat`, everything else is unchanged. The paired `ControllerNode` **application** (not its bootloader) is the OTA master on the link, delegated from the main bus via a new `ThermostatFirmware` endpoint. Full design: `ControllerNode-Thermostat-Link-Spec.md` §5. |
| `MainController` | It is the bus master — nothing pushes to it over the bus. Same flash map, same board-agnostic bootloader binary. Normal update path is **app-assisted over NINA/Wi-Fi** (§7.1); recovery from a failed one is **SWD/J-Link on site** — acceptable because the MainController is the one physically-accessible unit (screw terminals, enclosure), not a duct-buried node. |

### 7.1 MainController self-update over NINA (app-assisted)

The bootloader stays dumb and board-agnostic — it does **not** grow a NINA/AT transport (that would blow the 10 KB budget and the "one binary everywhere" property). Instead the **running app**, which already carries the full u-connectXpress driver, does the update:

1. App learns an image is available (mechanism = whatever MainController's outward interface ends up being — MQTT / HTTP / its cloud backend; `MainController-Spec.md` §4 open item 1) and downloads it over Wi-Fi in small blocks.
2. App stops polling the bus (slaves ride out the gap on their heartbeat/resync), disables interrupts, and runs a **RAM-resident** erase+program routine (`__attribute__((section(".RamFunc")))`) — required because erasing the flash bank stalls instruction fetch for the whole ~40 ms/page, and on the G031 there is only one bank. It programs the 50 KB app slot block-by-block as blocks arrive (no staging area — there is no room for one).
3. `NVIC_SystemReset()`. On reboot the bootloader CRC-checks the new image (§4) and jumps to it.
4. **If interrupted:** the half-written app fails its CRC32 check, the bootloader stays resident — but it has no Wi-Fi, so recovery is a J-Link visit. This is the accepted trade for keeping the bootloader small and uniform.

The RAM-resident flash helper lives in `Lib/HAL/Flash` as a `.RamFunc` variant alongside the normal (flash-resident) one the bootloader uses.

---

## 8. Open items — need your input before finalizing

**Resolved 2026-09-08:** single 50 KB app slot + resident-bootloader recovery (§3, §5); CRC32 for the whole-image check (§4); `NodeId` is factory-written and read-only (§6.3); `MainController` updates app-assisted over NINA with SWD as the recovery path (§7.1).

1. **`ConfigRecord.settings[16]`** — is 16 bytes of per-node *factory* config enough (servo end-stop trim, room id, sensor offset…), or should the record grow to 48/64 bytes? Cheap to size generously now.
2. **MainController Wi-Fi image source (§7.1 step 1)** — MQTT / HTTP GET / push from its own backend? Belongs in `MainController-Spec.md` §4 item 1, flagged here because it shapes the app-side updater.
3. ~~**OTA baud rate**~~ — no OTA-specific rate. The whole bus runs **115 200 baud** (`RS485-Node-Protocol-Spec-STM32G030.md` §9, as actually implemented in `BoardPins.h`) with no mid-session baud switching. The old "~6 s full-image transfer" estimate predates both this baud and the v2 synchronous per-write Ack/Nack protocol and needs re-deriving (§6.2.1's sizing note is likewise stale on this point).
4. **Boot-fail counter** (§5) — include the watchdog-style "app resets N times without going healthy → stay in bootloader" fallback in v1, or leave it out? Adds one backup register and a bit of app-side "I'm healthy" bookkeeping.
5. ~~**Bootloader size**~~ — 10 KB reserved; the full image (boot decision + validate + jump + the OTA slave loop, main bus and Thermostat link) links at ~8.0 KB, ~2 KB headroom.
6. **Flash RDP level 1** in production (blocks SWD image readout; reversible only via full mass-erase)? Default: no — revisit only if the image is considered sensitive.
7. **New module / lib layout** — mostly built 2026-09-08:
    - ✅ `Lib/Board/MemoryMap.h` (partition constants) + `Board::EnterBootloaderMagic`
    - ✅ `Lib/Board/ImageDescriptor.h` — the §4 descriptor + `CC_IMAGE_DESCRIPTOR(...)` macro
    - ✅ `Lib/NodeLib/ConfigStore.{h,cpp}` (read-only identity accessor) + `Node` wiring
    - ✅ `Lib/HAL/Backup.{h,cpp}` (TAMP backup registers), `Hal::System::SetVectorTable` / `JumpToApplication`, `Hal::Crc::Poly::Ieee32` + `Compute32`
    - ✅ `Lib/Startup/` — shared `startup_stm32g031xx.s` + `syscalls.c`; `Modules/*/*.ld` (bootloader 10 KB @ `0x0800_0000`, app 50 KB @ `0x0800_2800` with the descriptor at `0xC0`); `cmake/stm32.cmake` `add_stm32_executable()` (elf→bin/hex, size, `flash-<name>` J-Link target)
    - ✅ `Modules/Bootloader/` — base skeleton (`main.cpp` + `AppImage.{h,cpp}`); ✅ `Modules/MainController/` — base firmware (`main.cpp` runs `NodeMaster`, `ImageInfo.cpp` embeds the descriptor)
    - ✅ `Lib/HAL/Flash.{h,cpp}` (erase/program) — the `.RamFunc` variant for §7.1 (MainController self-update) is still open, tracked there, not here.
    - ✅ The `Lib/NodeLib` framing/bus split — `Modules/Bootloader/FirmwareSlave.cpp` links `NodeLib::Frame` directly, no `Node`/`NodeMaster`.
    - ✅ The bootloader's OTA slave loop (§6/§6.2.1) — `Modules/Bootloader/FirmwareSlave.{h,cpp}`.
    - ✅ **The post-build image-finalize step, 2026-09-20**: `Software/cmake/finalize_image.py`, wired into `add_stm32_executable()`'s `MODULE` path in `cmake/stm32.cmake` (replaces the old plain-copy `_ota_bin` step) — patches `imageSize`, sets `FlagCrcPresent`, and appends the real trailing CRC-32. Verified end to end on both `TemperatureNode` and `ControllerNode` builds (Debug and Release): the finalized `imageSize` matches the file length, the trailing 4 bytes match an independently-recomputed CRC-32 against the standard CRC-32/MPEG-2 check vector, and `Webserver/internal/nodelib.ParseImage()` (see the CRC32 fix below) accepts the result. **This also fixed a real, previously-undiscovered bug**: the server was computing the image's announced CRC-32 with Go's stdlib `crc32.ChecksumIEEE` (reflected, zlib-style), while the node's `Hal::Crc(Poly::Ieee32)` computes the STM32 hardware's *native* (non-reflected, no final XOR) CRC-32 — two different algorithms over the same bytes, essentially guaranteed to disagree. `HandleEnd()`'s `computed == imageCrc32` check was therefore failing on every push regardless of transfer correctness; this is the most likely actual explanation for the "100% complete but CRC mismatch" finding in `OTA-Debugging-TODO.md` §3, more than anything about chunking. Fixed by adding a matching non-reflected `CRC32()` to `Webserver/internal/nodelib/crc.go` and using it in `ParseImage()` instead.
8. **Image-descriptor CRC gating** — the base accepts an app on `magic` alone when `FlagCrcPresent` is clear (raw SWD-flashed dev image). Confirm that's the right default, and that the finalize step (which sets the flag) is only ever run for OTA-distributed images.
9. **§6.2.1 transfer protocol v2 — decisions needed before building:**
    - ~~**Bus time-slicing during an active OTA job**~~ — resolved 2026-09-20: `NodeMaster`'s round-robin already only visits `active`-flagged nodes (confirmed against `NodeMaster.cpp:148-156`), so it's already fast in practice and there's no need for a bus-dedication mode. `Ack`/`Nack` rides the existing queue/`Poll` delivery unchanged (§6.2.1 "Bus scheduling").
    - **Ack-timeout and retry-count constants** — since delivery still rides the normal `Poll` cadence (not a new immediate-reply transport), the existing `otaReportWait`-style timeout logic mostly carries over; revisit its exact value only if the correlation change (echoed offset+CRC) reveals the current 3 s / 4-attempt budget is miscalibrated for some other reason, not because delivery timing changed.
    - **Should `Begin`/`End`/`Abort` also move to `Ack`/`Nack`** (same correlation benefit as `Write`, still delivered via the normal queue/`Poll`) — or is `Write` the only one that needs it, since `Begin`/`End`/`Abort` each happen once per job rather than ~2000 times? Leaning toward yes for consistency, but it's more surface area to change at once.
    - ~~**Chunk size / flash-verification granularity**~~ — **decided 2026-09-20: 32 data bytes/chunk** (4× 8-byte double-words), via shrinking `byteOffset` to a 2-byte `uint16` (the 50 KB app slot fits easily) and growing `MAX_DATA` to 35 (`RS485-Node-Protocol-Spec-STM32G030.md` §7/§9) — `1 (FirmwareOp) + 2 (offset) + 32 (data) = 35`, exactly, no slack. A real image's final chunk is short whenever `imageSize` isn't a multiple of 32 — the normal case (e.g. the bench's 19868-byte build: 620 full chunks + one 28-byte final one), not a rare edge case only `AppSize`-sized pushes would hit; `Hal::Flash::Program()` already pads a trailing partial double-word with `0xFF`. Still *fewer* frames than v1's `ceil(imageSize/27)`, despite the bigger header — the freed offset bytes more than pay for themselves. `AppendImageBytes`/`pageBuffer`/`FlushPage` are deleted; every `Write` is independently, immediately flashable and `chunkCrc16` is always a true post-`Program()` read-back CRC.
    - ~~**Partial-program failure within a chunk**~~ — resolved 2026-09-20 (§6.2.1 "Partial-program failure within a 32-byte chunk"): the node tracks, per in-flight chunk, how many of its 4 double-words are already committed (a clean prefix, since `Hal::Flash::Program()`'s loop stops at the first failure), and resumes only from the first not-yet-done one on any retry of that offset — this is internal node bookkeeping, the wire protocol doesn't change. Flagged because a naive "just resend the whole chunk" would hit `PROGERR` re-programming already-succeeded double-words within it.
    - **`chunkCrc16` width** — CRC16 per chunk has a non-negligible (~few %) cumulative miss probability across a full ~1600-chunk image; that's an acceptable trade *given the whole-image CRC32 in `HandleEnd()` stays as an unconditional final backstop* (this per-chunk check is for fast/early/localized detection, not a replacement for it) — confirm that reasoning is accepted rather than assumed.
    - **This changes the wire protocol** — needs mirroring in `Webserver/internal/nodelib` alongside `Software/Lib/NodeLib`/`Modules/Bootloader` per the usual byte-compatibility requirement (`CLAUDE.md`).

---

*Done so far: `Lib/Board/MemoryMap.h`, `Lib/NodeLib/ConfigStore.{h,cpp}` + `Node` reading its id from flash (2026-09-08). Natural next artifact once §8 is pinned: the `ImageDescriptor` struct + the bootloader state machine (`enum class BlState { Idle, Erasing, Receiving, Valid, Error }`), `Lib/HAL/Flash`, and the master-side OTA driver.*
