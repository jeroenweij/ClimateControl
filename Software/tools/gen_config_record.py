#!/usr/bin/env python3
"""Generate the 32-byte factory ConfigRecord written to flash at 0x0800F000.

Layout mirrors NodeLib::ConfigStore (Lib/NodeLib/ConfigStore.cpp) and
Spec/Node-Flash-Layout-and-Bootloader-Spec.md Sec6.3:

    uint32  magic          0x4E4F4443  ('NODC')
    uint16  schemaVersion  1
    uint8   nodeId         1 .. MAX_NODES-1  (permanent bus address)
    uint8   module         1=ControllerNode 2=TemperatureNode
                           3=MainController 4=Thermostat
    uint8   settings[16]   per-node factory config (servo trim, room id, ...)
    uint8   reserved[4]
    uint32  crc32          zlib/PNG CRC-32 over the first 28 bytes

All fields little-endian. The CRC is a plain zlib.crc32 -- the same
table-less reflected CRC-32 the firmware recomputes at boot.

Usable as a module (build_record / render_ihex / render_bin) or a CLI.
"""

import argparse
import struct
import sys
import zlib

MAGIC = 0x4E4F4443  # 'NODC'
SCHEMA_VERSION = 1
CONFIG_ADDR = 0x0800F000
RECORD_SIZE = 32
MAX_NODES = 25  # keep in sync with NodeLib::MAX_NODES

# Accepted --module names -> ConfigStore::Module value.
MODULES = {
    "controllernode": 1,
    "controller": 1,
    "temperaturenode": 2,
    "temperature": 2,
    "temp": 2,
    "maincontroller": 3,
    "main": 3,
    "thermostat": 4,
}

# Only these are real bus nodes with a provisioned identity (ConfigStore::Valid
# rejects nodeId outside 1..MAX_NODES-1, and the master / Thermostat are not on
# the polled bus with an id).
PROVISIONABLE = {1, 2}


def build_record(node_id, module, settings=b"", reserved=b"\xff\xff\xff\xff",
                 schema=SCHEMA_VERSION):
    """Return the 32 raw bytes of a CRC-correct ConfigRecord."""
    if not 1 <= node_id < MAX_NODES:
        raise ValueError(f"node-id {node_id} out of range 1..{MAX_NODES - 1}")
    if not 0 <= module <= 0xFF:
        raise ValueError(f"module {module} out of range")

    settings = bytes(settings)
    if len(settings) > 16:
        raise ValueError("settings must be at most 16 bytes")
    settings = settings.ljust(16, b"\x00")

    reserved = bytes(reserved)
    if len(reserved) != 4:
        raise ValueError("reserved must be exactly 4 bytes")

    head = struct.pack("<IHBB", MAGIC, schema, node_id, module) + settings + reserved
    assert len(head) == RECORD_SIZE - 4, len(head)
    crc = zlib.crc32(head) & 0xFFFFFFFF
    return head + struct.pack("<I", crc)


def _ihex_record(rectype, addr, data):
    body = bytes([len(data), (addr >> 8) & 0xFF, addr & 0xFF, rectype]) + data
    checksum = (-sum(body)) & 0xFF
    return ":" + (body + bytes([checksum])).hex().upper()


def render_ihex(record, addr=CONFIG_ADDR):
    """Intel HEX text (CRLF, matching GNU objcopy) placing 'record' at 'addr'."""
    lines = [
        _ihex_record(0x04, 0x0000, struct.pack(">H", (addr >> 16) & 0xFFFF)),
        _ihex_record(0x00, addr & 0xFFFF, record),
        _ihex_record(0x01, 0x0000, b""),
    ]
    return "\r\n".join(lines) + "\r\n"


def render_bin(record):
    return bytes(record)


def _parse_settings(text):
    if text is None:
        return b""
    cleaned = text.replace(" ", "").replace(":", "").replace("0x", "")
    try:
        return bytes.fromhex(cleaned)
    except ValueError:
        raise argparse.ArgumentTypeError(f"--settings is not valid hex: {text!r}")


def _describe(record):
    magic, schema, node_id, module = struct.unpack("<IHBB", record[:8])
    settings = record[8:24]
    crc = struct.unpack("<I", record[28:32])[0]
    name = next((k for k, v in MODULES.items() if v == module and len(k) > 6), module)
    return (
        f"  magic   0x{magic:08X}  ({'ok' if magic == MAGIC else 'BAD'})\n"
        f"  schema  {schema}\n"
        f"  nodeId  {node_id}\n"
        f"  module  {module}  ({name})\n"
        f"  settings {settings.hex()}\n"
        f"  crc32   0x{crc:08X}"
    )


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--node-id", type=int, required=True,
                    help=f"permanent bus address, 1..{MAX_NODES - 1}")
    ap.add_argument("--module", required=True, choices=sorted(MODULES),
                    help="node module type")
    ap.add_argument("--settings", type=_parse_settings, default=b"",
                    help="up to 16 bytes of per-node config as hex (default: zeros)")
    ap.add_argument("--schema", type=int, default=SCHEMA_VERSION)
    ap.add_argument("--force", action="store_true",
                    help="allow a module type that is not a polled bus node")
    fmt = ap.add_mutually_exclusive_group()
    fmt.add_argument("--hex", action="store_true", help="write Intel HEX (default)")
    fmt.add_argument("--bin", action="store_true", help="write a raw 32-byte blob")
    ap.add_argument("-o", "--out", help="output file (default: stdout for --hex)")
    args = ap.parse_args(argv)

    module = MODULES[args.module]
    if module not in PROVISIONABLE and not args.force:
        ap.error(f"module '{args.module}' is not a provisioned bus node; pass --force "
                 f"to override")

    try:
        record = build_record(args.node_id, module, args.settings, schema=args.schema)
    except ValueError as exc:
        ap.error(str(exc))
    sys.stderr.write(_describe(record) + "\n")

    if args.bin:
        blob = render_bin(record)
        if not args.out:
            ap.error("--bin requires -o/--out")
        with open(args.out, "wb") as handle:
            handle.write(blob)
        sys.stderr.write(f"wrote {args.out} ({len(blob)} bytes)\n")
    else:
        text = render_ihex(record)
        if args.out:
            with open(args.out, "w", newline="") as handle:
                handle.write(text)
            sys.stderr.write(f"wrote {args.out}\n")
        else:
            sys.stdout.write(text)

    return 0


if __name__ == "__main__":
    sys.exit(main())
