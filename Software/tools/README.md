# Factory tooling

## Provisioning a node's identity

Each `ControllerNode` / `TemperatureNode` gets a one-time 32-byte `ConfigRecord`
written to flash at `0x0800F000` (Config A page). It holds the permanent bus
address (`NodeId`) and module type; firmware only ever reads it
(`NodeLib::ConfigStore`). See `Spec/Node-Flash-Layout-and-Bootloader-Spec.md`
§6.3.

### Per board

```
# 1. bootloader + application, once per board type (identical across units)
make -C Software build
JLinkExe ... flash-temperatureNode-full        # or: cmake --build build --target flash-temperatureNode-full

# 2. identity, once per unit
Software/tools/provision.py --node-id 7 --module temperature
```

`provision.py` builds the record, writes `config-nodeN-<module>.hex` + a J-Link
commander script, then runs `JLinkExe` to erase the config page and program it.
`JLinkExe` must be on `PATH`; with `--dry-run` (or if it is missing) the files
are written and the command is printed for you to run by hand.

Options: `--settings <hex>` (up to 16 bytes of per-node config), `--serial`
(pick a J-Link by serial number), `--out-dir` (keep the generated files),
`--force` (allow a non-bus-node module type).

### Just the blob

`gen_config_record.py` emits the record without touching hardware:

```
tools/gen_config_record.py --node-id 7 --module temperature            # Intel HEX to stdout
tools/gen_config_record.py --node-id 7 --module temperature -o cfg.hex
tools/gen_config_record.py --node-id 7 --module temperature --bin -o cfg.bin
```

The CRC-32 is a plain `zlib.crc32` — the same reflected CRC-32 the firmware
recomputes at boot (`ConfigStore::Valid`).
