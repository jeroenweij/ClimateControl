# Node Flash Layout, Bootloader & Persistent Identity — Design Spec

**Status:** Draft — first pass 2026-09-08. Partition map and OTA transport approach proposed here; decisions in §8 need your confirmation before any of this is built.
**Companion docs:** `RS485-Node-Protocol-Spec-STM32G030.md` (the v2 wire protocol the bootloader speaks a subset of), `Node-Bus-Hardware-Design-Spec.md` §6.1 (BOOT0/option-byte state, `NRST`), `Software-Architecture-Spec.md` (module map this adds to), `MainController-Spec.md` §4 item 4 (persistence — this spec answers "where"), `ControllerNode-Thermostat-Link-Spec.md` (Thermostat is off the main bus — §7 here)

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

**Sizing:** 50 KB ÷ 27 B/frame ≈ 1900 `Write` frames; a full-size update is **~6 s at the 250 000 baud bus rate** (`RS485-Node-Protocol-Spec-STM32G030.md` §9). The rest of the bus keeps polling normally throughout.

`MAX_DATA` stays **32** (`RS485-Node-Protocol-Spec-STM32G030.md` §9 default) — no node's RX buffer grows. The 27-byte `Write` payload is `32 − 1 (FirmwareOp) − 4 (offset)`.

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
3. ~~**OTA baud rate**~~ — no OTA-specific rate. The whole bus runs **250 000 baud** (`RS485-Node-Protocol-Spec-STM32G030.md` §9), giving a ~6 s full-image transfer with no mid-session baud switching.
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
    - **still to build:** `Lib/HAL/Flash.{h,cpp}` (erase/program, `.RamFunc` variant for §7.1); the `Lib/NodeLib` framing/bus split so the bootloader can link the framer without `Node`/`NodeMaster`; the bootloader's OTA slave loop (§6); the post-build image-finalize step (patch `imageSize` + append CRC32 + set `FlagCrcPresent`)
8. **Image-descriptor CRC gating** — the base accepts an app on `magic` alone when `FlagCrcPresent` is clear (raw SWD-flashed dev image). Confirm that's the right default, and that the finalize step (which sets the flag) is only ever run for OTA-distributed images.

---

*Done so far: `Lib/Board/MemoryMap.h`, `Lib/NodeLib/ConfigStore.{h,cpp}` + `Node` reading its id from flash (2026-09-08). Natural next artifact once §8 is pinned: the `ImageDescriptor` struct + the bootloader state machine (`enum class BlState { Idle, Erasing, Receiving, Valid, Error }`), `Lib/HAL/Flash`, and the master-side OTA driver.*
