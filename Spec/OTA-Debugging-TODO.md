# OTA / RS485 Bus — Known Issues

**Companion docs:** `Node-Flash-Layout-and-Bootloader-Spec.md` (bootloader/OTA protocol), `RS485-Node-Protocol-Spec-STM32G030.md` (wire format)

---

## 1. TemperatureNode app HardFault on Release builds — unconfirmed, not reproduced under further testing

A **Release**-build TemperatureNode app was once found permanently frozen in a HardFault after a plain J-Link reset: `IPSR=3`, `PC` parked at the weakly-aliased `Default_Handler`/`HardFault_Handler` infinite-loop stub. The auto-stacked exception frame's faulting PC decoded into `Hal::Uart::ServiceIrq()` (`Software/Lib/HAL/Uart.cpp`) — the app's own main-bus RX interrupt handler. `HardFault_Handler` is only ever weakly aliased to the startup file's infinite loop (never given a real definition), and there's no IWDG feeding/resetting it, so this would need a manual power cycle to recover in the field.

A Debug build of the same source did not reproduce it — suggests an optimization-exposed bug (missing `volatile`, aliasing, sequencing) rather than a plain logic bug. A subsequent reflash of the same Release image came up clean, and extensive further Release-build testing (multiple rebuilds, reflashes, resets, a full end-to-end OTA push and reboot into the new image) did not reproduce it again.

**Status: not root-caused.** Leans toward state-dependent (e.g. stale flash/backup-register state left over from an interrupted OTA test) rather than a deterministic firmware bug, but don't treat it as closed. If it recurs, correlate against what state preceded it — especially an aborted/partial OTA push, the condition it was first observed in.

---

## 2. Uplink flap — unconfirmed observation

The server's uplink (MC↔server TCP/NINA link) was observed to flap down for about a second right when a timeout occurred. `UplinkHandler.cpp`'s `LinkWatchdogMs=60000` forces a full NINA reset if the MC sees zero inbound bytes from the server for 60 continuous seconds (outbound-from-MC traffic doesn't count toward it) — a plausible independent mechanism, but not yet correlated against a real timestamped log. Worth checking whether this watchdog should also count outbound activity/a round-trip keepalive instead of pure inbound silence.

---

## 3. Bench tooling notes

- J-Link tools (`JLinkExe`, `JLinkGDBServer`) are installed on the dev machine. `arm-none-eabi-gdb` is *not* installed — use `gdb-multiarch` instead (works fine as a GDB-remote client against `JLinkGDBServer`).
- For live inspection with real variable names across the bootloader/app boundary: `file build/debug/Modules/Bootloader/bootloader.elf` then `add-symbol-file build/debug/Modules/TemperatureNode/temperatureNode.elf`, then `target remote localhost:2331`. Needs a **Debug** build flashed — Release builds only carry function-level symbols, no DWARF line info.
- `pkill -f <pattern>` is dangerous in a harness where the shell wrapper's own invoked command line contains the literal command text being run — `pkill -f JLinkGDBServer` can match and kill its own wrapper process. Use `pkill -x <exact-process-name>` instead, or kill by PID.
- Background J-Link/GDB/capture processes started with `&` need `disown` or they can get reaped when the tool call's foreground command finishes.
