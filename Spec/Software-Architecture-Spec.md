# Software Architecture — Module Map & Build/Toolchain

**Status:** Draft — reflects decisions made 2026-09-06, pending confirmation on open items below
**Companion docs:** `RS485-Node-Protocol-Spec-STM32G030.md` (main bus wire protocol), `Node-Bus-Hardware-Design-Spec.md`, `Node-Bus-Power-Path-Spec.md`, `MainController-Spec.md`, `TemperatureNode-Spec.md`, `ControllerNode-Thermostat-Link-Spec.md`

---

## 1. Node roles on the system

| Module | Role | Bus | Node ID (main bus) |
|---|---|---|---|
| `MainController` | RS485 bus master; also the system's outward-facing controller | Main RS485 bus (master) | `0` (reserved, per protocol spec §6) |
| `ControllerNode` | Damper actuator (metal geared servo); also link-master for its own room's `Thermostat` | Main RS485 bus (slave) + point-to-point link to its `Thermostat` | `1..N` |
| `TemperatureNode` | Reads incoming/outgoing air temperature near the outside HVAC unit | Main RS485 bus (slave) | `1..N` (shares the same address space as `ControllerNode`) |
| `Thermostat` | Per-room setpoint/UI; paired 1:1 with the `ControllerNode` for that room | Point-to-point link to its `ControllerNode` only — **not** on the main bus | n/a (2-endpoint link, no bus addressing needed) |

All four run on the STM32 platform. `MainController` and the main-bus slave nodes (`ControllerNode`, `TemperatureNode`) share the same STM32G0-family target and the same `NodeLib` wire protocol (`RS485-Node-Protocol-Spec-STM32G030.md`). `Thermostat` talks only to its own `ControllerNode`, over a separate link that reuses `NodeLib`'s `Id`/`Message`/CRC framing but **not** `NodeMaster`'s round-robin polling/discovery state machine (confirmed — see `ControllerNode-Thermostat-Link-Spec.md` §2). This implies `Lib/NodeLib` should be internally split: a framing layer (`Id`/`Message`/CRC, no arbitration) usable standalone by the Thermostat link, plus the main-bus `Node`/`NodeMaster` layer built on top of it.

---

## 2. Directory layout

```
ClimateControl/
├── CLAUDE.md
├── Spec/                      # design docs (this file + protocol/hardware/power specs)
├── Hardware/                  # PCB/schematic sources (empty so far)
└── Software/                  # everything build-related lives under here — nothing software-side at the repo root
    ├── .clang-format          # copied verbatim from ~/git/rollercoaster — same style, no project-specific tweaks
    ├── CMakeLists.txt         # top-level CMake project — aggregates Lib/* and Modules/*
    ├── cmake/                 # ARM Cortex-M0+ toolchain file (arm-none-eabi-cortex-m0plus.cmake)
    ├── Lib/
    │   ├── HAL/                # thin wrapper around STM32Cube HAL/LL — the only place that touches ST's driver headers directly
    │   ├── Board/              # BoardPins.h + MemoryMap.h — single source of truth for the STM32G031F8P6 pin map (all 3 board types, per Node-Bus-Hardware-Design-Spec.md §6.2) and flash partition map (Node-Flash-Layout-and-Bootloader-Spec.md §3). Header-only INTERFACE lib, depends on HAL for Hal::Pin
    │   ├── Tools/              # DelayTimer (on HAL_GetTick) + Logger (UART + Diagnostics log-ring sink) — shared helpers, MCU-agnostic
    │   ├── Startup/            # shared startup_stm32g031xx.s + syscalls.c (OBJECT lib "Startup", linked into every image)
    │   └── NodeLib/            # ported RS485 v2 protocol (Node/NodeMaster/Id/Message/Endpoint/Operation) + ConfigStore (factory node identity) — depends on Lib/HAL + Lib/Board (Node/NodeMaster take their pins from BoardPins.h, not constructor args). Message model: Node-Message-Model-Spec.md
    └── Modules/
        ├── Bootloader/         # firmware image: bus-resident OTA bootloader, one binary for all boards (base skeleton exists — Node-Flash-Layout-and-Bootloader-Spec.md)
        ├── MainController/     # firmware image: RS485 bus master (base skeleton exists)
        ├── ControllerNode/     # firmware image: damper/servo slave node + ControllerNode<->Thermostat link (master side) — not built yet
        ├── TemperatureNode/    # firmware image: duct temperature slave node — not built yet
        └── Thermostat/         # firmware image: room UI, paired to one ControllerNode — not built yet
```

Each `Modules/*` directory builds its own `.elf` via `add_stm32_executable()` (`cmake/stm32.cmake`) — linked against a per-module `.ld`, the shared `Startup` object, and whichever `Lib/*` static libs it needs — with post-build `.bin`/`.hex`/size and a `flash-<name>` J-Link target.

**Resolved:** `Software/Lib/Tools/` now exists — `DelayTimer` re-implemented on `HAL_GetTick()` instead of `millis()`, plus a `Logger` for a debug UART. Since `arm-none-eabi-gcc` ships a real C++ standard library (unlike the AVR toolchain), `Logger` can use real `<sstream>`/`std::string` directly instead of the old hand-rolled `SStream` shim — still worth watching against the STM32G031's 8 KB SRAM budget (unchanged from the G030), since `std::stringstream` is not free.

---

## 3. Toolchain & build

| Item | Choice |
|---|---|
| Compiler | `arm-none-eabi-gcc` (already installed on this machine, confirmed) |
| Build system | CMake, one `CMakeLists.txt` per `Modules/*` producing a `.elf`, plus a top-level `Software/CMakeLists.txt` aggregating `Lib/*` and `Modules/*` — same shape as `rollercoaster/CMakeLists.txt` → `src/CMakeLists.txt` → `NodeLib`/`libtools`, just swapping the AVR toolchain file for an ARM Cortex-M0+ one. Configure from `Software/` (`cmake -S Software -B build`) |
| Low-level driver layer | STM32Cube HAL/LL (ST's official driver library), wrapped by `Lib/HAL` so `NodeLib`/`Modules` code never includes ST headers directly |
| MCU target | STM32G031F8P6 (Cortex-M0+, 64 MHz, 64 KB flash / 8 KB SRAM, TSSOP20) for all four boards — locked in 2026-09-08. Drop-in for the earlier STM32G030F6P6TR (same pinout/core/RAM); +32 KB flash for a bus-resident DFU bootloader + the NINA driver, plus LPUART1 / RTC+backup-registers / TIM2. HAL device define `STM32G031xx`. |
| Flashing | SEGGER J-Link. `add_stm32_executable()` adds a `flash-<name>` target per module that generates a commander script (`cmake/gen_jlink_script.cmake`) and runs `JLinkExe -device STM32G031F8`. **Note: `JLinkExe` is not installed on this machine — the target configures but won't run until the J-Link Software Pack is installed.** |

**Resolved:** the STM32 CMake toolchain file (`Software/cmake/arm-none-eabi-cortex-m0plus.cmake`, `-mcpu=cortex-m0plus -mthumb`, newlib-nano + `nosys`, `-fno-exceptions/-fno-rtti`). Startup + linker wiring done: shared `Lib/Startup/startup_stm32g031xx.s` + `syscalls.c`, per-module `.ld` scripts, and `cmake/stm32.cmake` (`add_stm32_executable()` → `.elf` + `.bin`/`.hex` + `--print-memory-usage` + `flash-<name>`). `Bootloader` and `MainController` build today (~5.3 KB / ~17.6 KB flash).

---

## 4. Code style

Same C++ style as `~/git/rollercoaster` (and its `node` submodule at `~/git/node`), which this project's `Software/.clang-format` (copied verbatim) formats automatically. Conventions observed in that codebase, not all of which `clang-format` enforces on its own:

- Allman braces, 4-space indent, no tabs, `#pragma once` (no include guards).
- `PascalCase` for classes, methods, and enum values; `camelCase` for member variables and locals; no `m_`/`_` prefixes.
- `enum class` everywhere (`Endpoint`, `Operation`, `PinMode`), each paired with an `operator<<(std::stringstream&, ...)` for logging.
- Constructors use member-initializer lists, one member per line, colon-aligned (`clang-format`'s `BreakConstructorInitializers: AfterColon` handles the wrapping).
- `namespace NodeLib { ... } // namespace NodeLib` — closing-brace comment on every namespace.
- `.cpp` files pull in the specific symbols they need via `using NodeLib::Foo;` near the top, rather than `using namespace`.
- No heap allocation in the protocol / endpoint-handler code (`NodeLib` and the modules' handlers) — fixed-size buffers/queues throughout, consistent with the RS485 spec's buffering section (§7).
- Header comment block on every file:
  ```cpp
  /*************************************************************
  * Created by J. Weij
  *************************************************************/
  ```

---

## 5. Open decisions carried over from other Specs

These are already flagged in their respective docs and repeated here only because they gate module-level work:

- `RS485-Node-Protocol-Spec-STM32G030.md` §9: `MAX_DATA` cap, baud rate, CRC placement, v1/v2 bus coexistence.
- `Node-Bus-Hardware-Design-Spec.md` §7 / `Node-Bus-Power-Path-Spec.md` §6: servo stall current, connector choice, RT resistor value.
