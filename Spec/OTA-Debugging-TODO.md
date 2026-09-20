# OTA / RS485 Bus Debugging — TODO

**Status:** Open — findings from a 2026-09-20 live J-Link/GDB debugging session on the bench (node 1, TemperatureNode). Three separate issues surfaced; none are fully root-caused yet.
**Companion docs:** `Node-Flash-Layout-and-Bootloader-Spec.md` (bootloader/OTA protocol), `RS485-Node-Protocol-Spec-STM32G030.md` (wire format), `Software-Architecture-Spec.md` (build/toolchain)

---

## 1. TemperatureNode app HardFault on Release builds — needs re-verification

Found a **Release**-build TemperatureNode app permanently frozen in a HardFault, reproduced identically twice via a plain J-Link reset (`r`/`g`): `IPSR=3`, `PC` parked at the weakly-aliased `Default_Handler`/`HardFault_Handler` infinite-loop stub. The auto-stacked exception frame's faulting PC decoded into `Hal::Uart::ServiceIrq()` (`Software/Lib/HAL/Uart.cpp`) — the app's own main-bus RX interrupt handler. `HardFault_Handler` is only ever weakly aliased to the startup file's infinite loop (never given a real definition), and there's no IWDG feeding/resetting it, so this would need a **manual power cycle** to recover in the field.

Rebuilding the same source in **Debug** and flashing did *not* reproduce the crash — suggested an optimization-exposed bug (missing `volatile`, aliasing, sequencing) rather than a plain logic bug.

**However:** after a plain reflash (of what should be the same Release image), the node came up clean with no HardFault and has been reporting normally since. So either:
- the original crash was tied to specific stale flash/backup-register state left over from an interrupted OTA test (not a deterministic firmware bug), or
- it *is* real but timing/state-dependent and didn't happen to trigger on this particular boot.

**Next step:** don't treat this as fixed. Deliberately try to reproduce again — repeated power-cycles of a known-Release build, and/or reflashing Release right after an aborted/partial OTA push (the state this was first observed in) — before concluding it's a non-issue. If it doesn't reproduce after a handful of tries, downgrade this to "watch for it," not "root cause."

---

## 2. Near-complete OTA stall (96%) — status report not credited in time

One push (19868-byte image) reached `expectedOffset=19062` (96%) before the server gave up with "timeout waiting for write progress" (`Webserver/internal/service/ota.go`, `otaWriteRetries`/`otaReportWait` = 4 attempts × 3s = 12s budget). Live GDB inspection at the moment of failure showed the node had *not* crashed or lost data — `FirmwareSlave` was cleanly cycling in `Loop()`, had already applied that final write, and had already sent its status reply (`statusPending=false`). The reply just never got credited by the server in time.

**Working theory, not confirmed:** the bench has ~21 configured node IDs but only node 1 is physically present. `NodeMaster`'s round-robin (`Software/Lib/NodeLib/NodeMaster.cpp`) waits a full `pollTimeoutMs=200ms` for each of the ~20 offline nodes before moving on, so one full sweep back to node 1 is ~4+ seconds — close enough to the fixed 3s-per-attempt window to occasionally straddle badly. This is already called out as a known "occasional" residual risk in `ota.go`'s comments around `otaWriteRetries`.

**Next step:** correlate a live, timestamped MC serial capture (`/dev/ttyUSB1`, 9600 8N1) against OTA job progress polling across a full run, to see whether the round-robin sweep time (or a periodic `DetectNodes` cycle) is what's eating the retry budget. **2026-09-20: the §6.2.1 protocol redesign (see §3 below) fixes this class of bug by construction** — a synchronous ack tied to the specific write removes the "which future poll cycle carries the reply" ambiguity entirely — so this may not need its own separate fix once that lands.

---

## 3. CRC mismatch after a "100% complete" transfer — real data corruption, unexplained

A separate push reported reaching **100%** (19868/19868 bytes) but then failed the bootloader's own end-of-transfer verification with `lastError=6` (`ErrCrcMismatch`). This is worse than #2: the transfer nominally succeeded, but the image assembled in flash doesn't match the source.

At the failure point, `NodeLib::Frame`'s counters showed `crcErrors=0` and `resyncs=0` (no per-frame CRC16 failures on the wire all session) — every individual frame that was received and parsed was internally valid. So the corruption is not obviously wire-level bit-flipping.

Dumped the actual flashed image via J-Link (`AppBase=0x08002800`, `imageSize=19868`) and checked it: **no runs of `0xFF`** (every byte position genuinely got written — no skipped/unwritten gaps), but the recomputed CRC32 over the body matches neither the image's own trailing 4 bytes nor the job's declared CRC. Could not get a byte-exact diff against the true pristine source `.bin` in this session — it lives under `/opt/ccserver` on the bench server, owned by the `ccserver` user under a `ProtectSystem=strict` systemd sandbox; the `jeroen` SSH login doesn't have read/traverse permission there and no sudo password was available non-interactively, and there's no HTTP download route for stored firmware images either.

**This is the most concerning open finding.** Possible next steps:
- Get read access to the bench server's stored `TemperatureNode_1.0.bin` (loosen permissions, copy the file out, or add a debug-only download route) for a real byte-exact diff against the flash dump.
- Add temporary logging to the bootloader (raw USART2 writes, patterned after `OtaUart.cpp` — the bootloader doesn't link `Hal::Uart`/`Tools::Logger`) to log each accepted Write's offset + bytes as it's applied, and catch the corruption chunk-by-chunk live rather than only after the fact.
- Consider whether the master's retry/rewind logic (`ota.go`'s `run()`, resending via a `Get` probe rather than resending the same chunk) has an edge case that can apply the wrong bytes to a given offset — re-read that logic skeptically now that there's a confirmed corruption case to test it against, rather than trusting the "this is safe" reasoning in its comments.

**2026-09-20: chosen direction is a protocol redesign, not a patch.** Rather than chasing this specific edge case in the current stream-and-poll design, `Node-Flash-Layout-and-Bootloader-Spec.md` §6.2.1 now proposes replacing it with a synchronous per-write `Ack`/`Nack` (immediate reply, carrying a CRC16 of the staged chunk) — this fixes #2 above by construction too (the ack is no longer riding an incidental future `Poll`). See that section for the full design and §8 item 9 there for the open decisions (bus time-slicing during a job is the biggest one — it directly trades off against §6.1's "other nodes keep running while one updates" goal).

---

## 4. Uplink flap — unconfirmed observation

Separately, the server's uplink (MC↔ccserver TCP/NINA link) was observed to flap down for about a second right when a timeout occurred. `UplinkHandler.cpp`'s `LinkWatchdogMs=60000` forces a full NINA reset if the MC sees **zero inbound bytes from the server** for 60 continuous seconds (outbound-from-MC traffic doesn't count toward it) — a plausible independent mechanism, but not yet correlated against a real timestamped log. Worth checking whether this watchdog should also count outbound activity/a round-trip keepalive instead of pure inbound silence.

---

## 5. Tooling notes for whoever picks this up

- J-Link tools (`JLinkExe`, `JLinkGDBServer`) are installed on the dev machine now. `arm-none-eabi-gdb` is *not* installed — use `gdb-multiarch` instead (works fine as a GDB-remote client against `JLinkGDBServer`).
- For live inspection with real variable names across the bootloader/app boundary: `file build/debug/Modules/Bootloader/bootloader.elf` then `add-symbol-file build/debug/Modules/TemperatureNode/temperatureNode.elf`, then `target remote localhost:2331`. Needs a **Debug** build flashed — Release builds only carry function-level symbols, no DWARF line info.
- `pkill -f <pattern>` is dangerous in a harness where the shell wrapper's own invoked command line contains the literal command text being run — `pkill -f JLinkGDBServer` can match and kill its own wrapper process. Use `pkill -x <exact-process-name>` instead, or kill by PID.
- Background J-Link/GDB/capture processes started with `&` need `disown` or they can get reaped when the tool call's foreground command finishes.
- Bench server SSH: `ssh jeroen@host` (works, key-based, no separate creds needed) — but no permission to read `/opt/ccserver` (the `ccserver` service user's own files) without sudo.
