# Node Flash Layout, Bootloader & Persistent Identity — Design Spec

**Companion docs:** `RS485-Node-Protocol-Spec-STM32G030.md` (the wire protocol the bootloader speaks a subset of), `Node-Bus-Hardware-Design-Spec.md` §6.1 (BOOT0/option-byte state, `NRST`), `Software-Architecture-Spec.md` (module map this adds to), `MainController-Spec.md` §5 (persistence), `ControllerNode-Thermostat-Link-Spec.md` (Thermostat is off the main bus — §7 here), `Node-Message-Model-Spec.md` §4 (the `Ack`/`Nack` operations §6.2 below uses)

---

## 1. Scope & goals

Three things, one flash design:

1. **A bus-resident bootloader** that speaks enough of `NodeLib` v2 to receive a firmware image over the main RS485 bus and program it — so a `ControllerNode` / `TemperatureNode` in a duct never needs a J-Link for a firmware update.
2. **The application image** itself — same STM32G031F8P6, linked to sit above the bootloader.
3. **Persistent node identity** — a wear-safe, power-fail-safe place to keep the node's bus address (`NodeId`) and a small amount of per-node config, so a given physical node keeps the same address across reboots and firmware updates.

**Goals**
- Never permanently brick a node through a failed/interrupted update, as long as the bootloader region and the bus are intact.
- Bootloader is small, self-contained, and never updated over the bus (only via SWD) — it is the recovery anchor.
- Same bootloader binary on all four boards; the flash map is identical MCU-wide (`STM32G031F8P6`).
- Reuse what already exists: the `Frame`/`Crc`/`Id`/`Message` framing layer, `Lib/HAL`, `Lib/Board`.

**Non-goals (v1)**
- Image signing / encryption — the bus is assumed trusted, same stance as `RS485-Node-Protocol-Spec-STM32G030.md` §1. Flagged in §8 as a later option (flash RDP).
- Dual-slot A/B images — a single 50 KB app slot, with the resident bootloader as the recovery path (§5). Revisit only if images that pass CRC yet fail to run become a real risk.
- Delta/compressed images.
- A field-modifiable `NodeId` — identity is written once at factory and is read-only to firmware (§6.3).

---

## 2. STM32G031F8P6 flash geometry (RM0444)

| Property | Value |
|---|---|
| Main flash | 64 KB @ `0x0800_0000` – `0x0800_FFFF` |
| Page size / count | **2 KB × 32 pages** (erase granularity = one page) |
| Program granularity | **64-bit double-word**, to previously-erased flash only |
| SRAM | 8 KB @ `0x2000_0000` (bootloader and app never run at the same time — each may assume the full 8 KB) |
| 96-bit unique ID | read-only at `0x1FFF_7590` (used for provisioning, §6.3) |
| Backup registers | `TAMP->BKPxR` (5 × 32-bit) — retained across a warm reset (`NRST`, software reset, IWDG), cleared on power-on / brown-out. Exactly the semantics wanted for the app→bootloader handoff (§5). No VBAT battery fitted, so these do not survive the ENABLE-line power cut — deliberately. |

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
| Bootloader | `0x0800_0000` | 10 KB | J-Link only | Holds the reset vector; runs first on every boot. Carries the full OTA slave (main bus + the Thermostat link on USART2, `ControllerNode-Thermostat-Link-Spec.md` §5.5), which needs the 5th page. Links at ~8.0 KB, ~2 KB headroom. |
| Application | `0x0800_2800` | 50 KB | Bootloader (OTA) | Linked with `FLASH ORIGIN = 0x08002800`. First 0xC0 bytes = vector table; image descriptor at fixed offset `0xC0` (§4). The biggest module uses about half the slot. |
| Config A | `0x0800_F000` | 2 KB | J-Link at factory only | A single static `ConfigRecord` at the page base — `NodeId` + per-node factory config. Read-only to firmware (§6.3). |
| Config B | `0x0800_F800` | 2 KB | — (reserved) | Spare page, unused in v1 — held back for a future *runtime-writable* setting, which would turn A/B into a ping-pong log. |

`Lib/Board/MemoryMap.h` (header-only) is the single source of truth for these four constants; bootloader linker script, app linker script, `ConfigStore`, and the flash-program HAL all include it.

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

- The **last 4 bytes** of the image are a `CRC32` over bytes `[0x08002800, 0x08002800 + imageSize - 4)`, appended by a post-build step on the `.bin`. **CRC32, not CRC16** — a whole-image CRC16 has a ~1/65536 miss probability, not good enough for a 50 KB image; CRC32 is effectively free on the STM32 CRC unit. Config: STM32 CRC-unit **native** mode — poly `0x04C11DB7`, init `0xFFFF_FFFF`, no input/output bit-reversal, no final XOR (the post-build tool matches this exactly — the STM32 hardware's native CRC-32 is *not* the same algorithm as Go's stdlib `crc32.ChecksumIEEE`/zlib, which is reflected; both `Hal::Crc(Poly::Ieee32)` and `Webserver/internal/nodelib/crc.go`'s `CRC32()` implement the non-reflected form so the two sides agree).
- **Bootloader validity check** — the *only* gate on whether the app runs, checked on every boot (a CRC32 sweep of ~50 KB by the hardware CRC unit, a few ms): `magic` matches **and** trailing CRC32 matches **and** `imageSize <= 50 KB`. Pass → runnable. Fail → stay in the bootloader (§5). No separate "app valid" flag — the CRC32 is it.
- The base accepts an app on `magic` alone when `FlagCrcPresent` is clear (a raw SWD-flashed dev image); the finalize step (which sets the flag and appends the real CRC-32) runs only for OTA-distributed images, via `add_stm32_executable()`'s `MODULE` path (`Software/cmake/finalize_image.py`, wired into `cmake/stm32.cmake`) — dev/SWD builds never go through it.
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
   Boot-fail counter over threshold? ──────────────┘
      │ no
      ▼
   Jump to app (§4.1)
```

- **App→bootloader handoff:** app receives `EnterBootloader` (§6.2), writes `ENTER_BL_MAGIC` to `TAMP->BKP0R`, calls `NVIC_SystemReset()`. Backup register survives the warm reset; bootloader consumes and clears it.
- **Interrupted update is self-healing:** if power drops mid-write, the app region fails its CRC32 check on next boot, so the bootloader stays resident regardless of any flag, re-announces on the bus, and the master restarts the push.
- **Manufacturing:** a board flashed with *only* the bootloader (+ a provisioning record, §6.3) has no valid app → it comes up in the bootloader and the master loads the app over the bus. Field boards can be built app-less.
- **Boot-fail counter:** bootloader increments a `TAMP->BKP1R` counter (`Tools::BootHealth`) right before attempting to jump to a CRC-valid, non-forced-entry image; the app clears it once it has run for a fixed window (10 s) without ever getting there via a crash. Over 5 attempts without a clear → stay in bootloader, same as a failed CRC. A freshly-received image resets the counter (`FirmwareSlave::HandleBegin`), so it never inherits the previous image's failure count. Closes a real failure mode: a bad OTA push that boots but crashes would otherwise strand the node — CRC-valid, so always eligible to jump — until someone notices and re-pushes.

---

## 6. OTA over NodeLib

### 6.1 What the bootloader implements

Only entered when `ConfigStore::Valid()` (a provisioned node — see §6.3). A hand-written minimal slave loop — framing layer only, no `NodeMaster`, no `std::stringstream` `Logger` (too big for the 10 KB region):

- Reads its bus address from `ConfigStore` (§6.3) — same `NodeId` the app uses, so addressing is stable across the app↔bootloader transition.
- Responds to `Discover` with `Announce` (so the master sees it and knows it is in the bootloader).
- Serves one endpoint, `Endpoint::Firmware` (`Node-Message-Model-Spec.md` §3), plus the `Transport` plumbing.
- Transmits only when polled (`Poll`) — identical bus discipline to a normal slave, so the master's round-robin is undisturbed and other nodes keep running while one updates.

This requires `Lib/NodeLib` to be split, per `Software-Architecture-Spec.md` §1: a framing sub-library (`Frame`/`Crc`/`Id`/`Message`/`EEndpoint`/`EOperation`, logging compiled out) that the bootloader links, plus the `Node`/`NodeMaster` layer on top for the apps.

### 6.2 Transfer protocol — synchronous per-write acknowledgement

All OTA messages target `Endpoint::Firmware`. `data[0]` is a `FirmwareOp` sub-opcode; the transfer is strictly sequential (no out-of-order buffering, no bitmap). This is the "richer command set expressed as one endpoint acted on with `Set`/`Report`" pattern from `Node-Message-Model-Spec.md` §4 — the `Operation` verb set does not grow for OTA.

**Master → node** (`Operation::Set`, `data[0]` = `FirmwareOp`):

| `FirmwareOp` | name | payload (`data[1..]`) | node action |
|---|---|---|---|
| `0x01` | `Begin` | `module`(1) · `imageSize`(4 LE) · `imageCrc32`(4 LE) · `fwVersion`(2) | Confirm in bootloader; sanity-check size; erase all 26 app pages; reset write pointer to 0. |
| `0x02` | `Write` | `byteOffset`(2 LE, uint16) · `bytes`(32, fixed) | Program the chunk at `AppBase + byteOffset` immediately (no RAM page buffer — see below) and reply with `Ack`/`Nack`. |
| `0x03` | `End` | — | Verify trailing CRC32 over `[base, base+imageSize-4)`; set status to `bl-valid` or `bl-crcfail`. |
| `0x04` | `Activate` | — | Ensure `BKP0R` is clear; `NVIC_SystemReset()` → boots the new app (now valid by its CRC32, §4). |
| `0x05` | `Abort` | — | Discard; status back to `bl-idle` (app region left erased/invalid — master must retry). |
| `0x06` | `EnterBootloader` | — | **Handled by the running app** (via the `NodeLib` `Firmware` interceptor + `INodeHandler::PrepareForReset()`): park outputs, write `ENTER_BL_MAGIC` to `BKP0R`, `NVIC_SystemReset()`. |

**`Write`'s reply is `Ack`/`Nack`, not a generic `Report`** — this is `Node-Message-Model-Spec.md` §4's bidirectional `Ack`/`Nack` primitive, ties each reply unambiguously to the write that caused it (a lost ack and a lost write would otherwise be indistinguishable from the sender's side, and a raw `expectedOffset` counter carries no reference to which attempt produced it), and lets each reply attest to the integrity of that one write's data rather than only catching a mismatch at the very end. `data[0..2)` = `FirmwareOp = Status` reserved for other purposes (see the `Status` report below); the `Write` reply itself carries:

| field | bytes | |
|---|---|---|
| `byteOffset` | 2 LE | the offset this ack/nack is *for* — echoes the request |
| `chunkCrc16` | 2 LE | CRC16 (`Hal::Crc::Poly::Ccitt16`, the same peripheral config `Frame` already uses) over the flash bytes just read back at this offset — a true post-`Program()` read-back CRC, not a CRC over the RAM copy |
| `programFailed` | 1 | `0` = `Hal::Flash::Program()` reported success; `1` = it reported a failure (`FLASH_SR` error flags) — distinct from a `chunkCrc16` mismatch, which the master detects itself by comparing against the CRC it computed before sending |

The master compares `chunkCrc16` against the CRC it computed over the exact bytes it sent for `byteOffset` before sending — a mismatch means the node staged the wrong bytes, and the master can immediately resend just that one chunk instead of discovering the problem at the end with no idea which write was at fault.

**Node → master** (`Operation::Report`, `data[0]` = `FirmwareOp::Status` `0x07`, queued, sent on the next `Poll`):

| field | bytes | |
|---|---|---|
| `state` | 1 | 0=app · 1=bl-idle · 2=bl-erasing · 3=bl-receiving · 4=bl-valid · 5=bl-error |
| `expectedOffset` | 4 LE | next byte offset the node wants |
| `lastError` | 1 | |
| `runningFwVersion` | 2 | |

**Flow:** `Begin` → wait for its `Ack`/`Nack` → stream `Write` frames, each waiting for its own `Ack`/`Nack` → on a lost ack, resend the same `Write`; on a `Nack`, resync to the offset it names → repeat to `imageSize` → `End` → wait for its `Ack`/`Nack` → `Activate`. `Begin`/`End`/`Abort` reuse the same correlation via `Ack`/`Nack` too, for consistency with `Write` — the reply is a single `lastError` byte (`0` on `Ack`, one of `Lib/NodeLib/EFirmware.h`'s `FirmwareError` codes on `Nack`), since none of the three has anything else to report. The master still keeps a `Status`-report probe (`Get` on `Endpoint::Firmware`, unconditionally re-armed) alongside this on the same timeout budget, to recover from a lost `Set` or a lost `Ack`/`Nack` — the `Status` report's `state`/`lastError` say the same thing the op's own reply would have.

**Sequencing guard — `offset == expectedOffset`.** Each `Write` carries its own explicit destination address (`AppBase + byteOffset`) and doesn't depend on any buffer state, but tracking the frontier is still required for a hard hardware reason: **STM32G0 flash allows a given double-word address to be programmed exactly once per erase cycle** — a resend of an already-applied chunk must be answered without ever calling `Hal::Flash::Program()` again for that address, since a second write to an already-programmed double-word sets the `PROGERR` flag even for byte-identical data:
- `offset` is behind `expectedOffset` (a duplicate — the write landed, only its ack was lost): read back the flash bytes already at that address, derive `chunkCrc16` from that read, and `Ack` with it — `Flash::Program()` is not called.
- `offset` is ahead of `expectedOffset` (a genuine gap — should not happen if the master only ever sends the next expected chunk, but must be handled): `Nack` with `expectedOffset` in place of the echoed offset, so the master can resync without guessing.

Retries are bounded (a small fixed count) before declaring the chunk failed.

**Chunk size: 32 data bytes**, via `byteOffset` as a 2-byte `uint16` (the 50 KB app slot fits comfortably in that range) and `MAX_DATA = 35` (`RS485-Node-Protocol-Spec-STM32G030.md` §7): `1 (FirmwareOp) + 2 (offset) + 32 (data) = 35`, exactly, no slack. Since `AppBase` (`0x0800_2800`) is double-word-aligned and chunks are sent strictly sequentially, a chunk size that's itself a multiple of 8 (4× 8-byte double-words) makes every chunk's start address a multiple of 8 automatically, so every `Write` is independently, immediately programmable on its own — no RAM staging buffer at all. A real image's final chunk is short whenever `imageSize` isn't a multiple of 32 (the normal case, not a rare edge); `Hal::Flash::Program()` pads a trailing partial double-word with `0xFF`.

**Partial-program failure within a 32-byte chunk.** A 32-byte chunk is 4 double-words programmed in sequence by one `Hal::Flash::Program()` call; its loop stops at the *first* double-word that fails, so a partial failure is always a clean prefix — some leading double-words are now permanently programmed, the rest were never attempted. **The node tracks, per in-flight chunk, how many of its double-words are already committed, and resumes programming only from the first not-yet-done one on any retry of that offset** — never re-touching ones already locked in. This is internal node bookkeeping; the wire protocol (`Write`/`Ack` always at the full 32-byte granularity) doesn't change, and the master always just resends the whole 32-byte chunk.

**`chunkCrc16` width.** CRC16 per chunk has a non-negligible (~few %) cumulative miss probability across a full ~1600-chunk image — an acceptable trade given the whole-image CRC32 in `End` stays as an unconditional final backstop; this per-chunk check is for fast/early/localized detection, not a replacement for it.

**Bus scheduling.** `Ack`/`Nack` rides the existing "queue a reply, deliver on the node's next `Poll`" mechanism, unchanged — no dedicated "Write grants an immediate reply window" transport behavior, no bus-dedication mode. `NodeMaster`'s round-robin only ever stops on a node already flagged `active`, so an unprovisioned/undetected ID is skipped for free — the round-robin already only cycles through genuinely active nodes, fast in practice. This keeps §6.1's "other nodes keep running while one updates" property exactly as-is.

**Wire-format changes here need mirroring** in `Webserver/internal/nodelib` alongside `Software/Lib/NodeLib`/`Modules/Bootloader`, per the byte-compatibility requirement in `CLAUDE.md`.

### 6.3 Persistent identity — `ConfigStore` (read-only at runtime)

The `NodeId` is written **once, at factory provisioning, via J-Link** and is **read-only** to every firmware image thereafter. No wire command changes it, no field re-assignment, no ping-pong log — the config region is programmed at manufacture and only ever read after that.

`Lib/NodeLib/ConfigStore.{h,cpp}` is a **read-only accessor**, used by both bootloader and app:

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

The record CRC is a small table-less software CRC-32 (zlib flavour — poly `0xEDB88320`, init/final `0xFFFF_FFFF`), not the hardware CRC unit: it runs once over 28 bytes at boot, and keeping it self-contained means `ConfigStore` has no dependency on the shared CRC peripheral (which `Frame` owns) and the PC-side provisioning script can use `zlib.crc32` directly. (The §4 whole-image check is a different, larger job and does use the HW unit.)

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

`NodeLib::Node::Init()` reads its address from `ConfigStore` when it is not the master; an invalid/blank record calls `ErrorHandler::Error(false)` (never returns — solid error LED). `NodeMaster` hard-codes id `0` in its constructor and skips the flash read for it.

- **Unprovisioned / corrupt record** (blank `0xFF` page, bad magic, bad CRC): the node does **not** join the bus. `NodeLib::Node::Init()` raises a non-recoverable `ErrorHandler::Error(false)` (error LED solid) — a board that reached the field un-provisioned is a manufacturing escape, not something to paper over with a default address.
- **Provisioning:** a CMake `provision` target generates the 32-byte `ConfigRecord` blob (given `nodeId` + `module` + optional settings) and `JLinkExe` writes it to `0x0800_F000` — same tool and bench step as flashing the bootloader. The bootloader and app are then identical across units; only this one page differs.
- A provisioned node answers `Discover` with its stored id (in the `Announce` payload).

---

## 7. Per-board notes

**Two bootloader binaries.** `Software/Modules/Bootloader` is the board-agnostic RS485 OTA slave for `ControllerNode`/`TemperatureNode`/`Thermostat`. `Software/Modules/MainBootloader` is a separate, MainController-only binary that owns the on-board NINA-W152 directly and updates MainController over the server uplink instead (§7.1) — MainController is the bus master, never a bus slave, so it has no bus to receive a push over and no way to relay a push to itself.

| bootloader binary | runs on | stay-resident behaviour |
|---|---|---|
| `Software/Modules/Bootloader` | a provisioned `ControllerNode` / `TemperatureNode` / `Thermostat` (`ConfigStore::Valid()` true) | run the RS485 OTA slave loop (§6), addressed at `ConfigStore::NodeId()` (or the Thermostat link, see the table below) |
| `Software/Modules/Bootloader` | an unprovisioned node | passive wait; needs the bench (can't do addressed OTA without an address) |
| `Software/Modules/MainBootloader` | `MainController` only | bring up the NINA uplink and drive `Firmware`'s erase/write/verify/activate state machine from the server's `OtaControl`/`OtaData` messages (§7.1) |

`Software/Modules/Bootloader`'s `main.cpp` is identical on every one of those three board types, branching only on `ConfigStore::Valid()`; only the factory `ConfigRecord` (or its absence, on an unprovisioned node) differs. `MainBootloader` is a separate CMake target/image (`Software/Modules/MainBootloader/CMakeLists.txt`) with its own `main.cpp`, `mainBootloader.ld`, and flash-writer (`Firmware.cpp`, the same erase/program/verify state machine as `Bootloader/FirmwareSlave.cpp`, re-plumbed onto the NINA point-to-point transport instead of the RS485 bus's Poll-gated one — see `MainController-Server-Link-Spec.md` §5/§8 step 7 for the wire protocol). Both binaries are flashed to the same 10 KB region (§3); a given unit carries whichever one matches the board it's built for.

| Board | OTA path |
|---|---|
| `ControllerNode`, `TemperatureNode` | Main bus, exactly as §6. |
| `Thermostat` | **Not on the main bus** (`ControllerNode-Thermostat-Link-Spec.md`). `Bootloader`'s binary serves the image over the point-to-point link — `OtaUart` selects USART2 when `ConfigStore::GetModule() == Thermostat`, everything else is unchanged. The paired `ControllerNode` **application** (not its bootloader) is the OTA master on the link, delegated from the main bus via the `ThermostatFirmware` endpoint. Full design: `ControllerNode-Thermostat-Link-Spec.md` §5. |
| `MainController` | It is the bus master — nothing pushes to it over the bus, and it never relays to itself. `MainBootloader` dials the server directly (§7.1); recovery from a failed one is SWD/J-Link on site — acceptable because the MainController is the one physically-accessible unit (screw terminals, enclosure), not a duct-buried node. |

### 7.1 MainController self-update over NINA (bootloader-resident)

`Software/Modules/MainBootloader` owns the on-board NINA-W152 directly (`Firmware.h`/`.cpp`, `UplinkHandler.h`/`.cpp`, `NinaAt.h`/`.cpp`, `NinaUart.h`/`.cpp`, `NinaLineParser.h`/`.cpp`) and dials the same server uplink the app uses:

1. `SystemControl[reset->bootloader]` (or the boot-health "too many failed boots" path) parks the app into the bootloader as usual (§5). `MainBootloader`'s `UplinkHandler` brings NINA up (Wi-Fi join, TCP peer connect, data mode — blocking/sequential is fine here, there is nothing else to attend to without a connection) and sends a fresh `UplinkHello`.
2. Server → MC (`NODE=0`): `OtaControl[Begin] {imageSize, imageCrc32, fwVersion}` (`MainController-Server-Link-Spec.md` §5). `Firmware::HandleBegin()` erases the 50 KB app slot and replies `Ack`/`Nack` on `OtaControl`.
3. Server streams `OtaData {byteOffset(2 LE), bytes≤32}` chunks; each is programmed to flash immediately (no RAM staging, mirroring §6.2.1's bus design) and replied to individually on `OtaData` with `Ack`/`Nack {byteOffset, chunkCrc16, programFailed}` — same offset-resume/duplicate-write handling as the bus `Firmware[Write]` protocol.
4. At `imageSize`: `OtaControl[End]` — whole-image CRC-32 check (§4) against the flashed bytes, replies `Ack`/`Nack`. `OtaControl[Activate]` on success clears the stay-resident magic and resets; the app boots on the new image and reconnects, giving the server a fresh `UplinkHello` with the new `fwVersion`.
5. **If interrupted** (uplink drop, power loss, NAK'd chunk): the half-written app fails its CRC-32 check on the next boot, the bootloader stays resident and simply reconnects over NINA to retry `Begin` on its own — a J-Link visit is only needed if NINA itself is unreachable (no Wi-Fi credentials, hardware fault), not for the routine "OTA got interrupted" case.

Flash cost: **~81% of the 10 KB budget in both Release and Debug** (`Lib/HAL/CMakeLists.txt`'s `HalCoreOs`/`Lib/Tools/CMakeLists.txt`'s `ToolsOs` keep the HAL/Tools code MainBootloader links at the same size-optimised level regardless of build type). Kept small mainly by *not* linking `Lib/HAL/Uart` (`NinaUart.h`/`.cpp` is a from-scratch ~100-line ISR-driven driver instead, ~17 KB smaller) and by hand-rolled `AppendStr`/`AppendUInt` AT-command building in place of `snprintf` (pulls in nano's general formatting engine otherwise).

---

## 8. Open items

1. **`ConfigRecord.settings[16]`** — is 16 bytes of per-node factory config enough (servo end-stop trim, room id, sensor offset…), or should the record grow to 48/64 bytes? Cheap to size generously now.
2. **Flash RDP level 1** in production (blocks SWD image readout; reversible only via full mass-erase)? Default: no — revisit only if the image is considered sensitive.
3. **Ack-timeout and retry-count constants** (§6.2) — the existing timeout logic mostly carries over from before the `Ack`/`Nack` change; revisit its exact value only if real-world use reveals the current budget is miscalibrated.
4. **`MainBootloader` (§7.1) has no logging** — `Tools::Logger` is not linked, to stay inside the 10 KB budget, so only the error LED reports faults; a bootloader that never reconnects can't be diagnosed from outside.
