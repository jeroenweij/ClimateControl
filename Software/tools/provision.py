#!/usr/bin/env python3
"""Factory-provision one node's identity: write its ConfigRecord to 0x0800F000.

    tools/provision.py --node-id 7 --module temperature
    tools/provision.py                       (asks for the node id and device)
    tools/provision.py --node-id 7 --module temperature --flash-full

Generates the 32-byte record (gen_config_record.py), emits an Intel HEX file
and a J-Link commander script, then runs JLinkExe to erase the config page and
program it. Run once per unit -- the bootloader and app images are identical
across units, only this page differs (Spec/Node-Flash-Layout-and-Bootloader-
Spec.md Sec6.3).

Bench order per board:
  1. flash-<module>-full        bootloader + app          (once per board type)
  2. provision.py --node-id N --module <m>                (once per unit)

Both steps in one go: --flash-full runs `make flash-full` (bootloader + app,
Release unless --build-type Debug) right after the config page is written;
--no-flash-full skips it. With neither, an interactive run asks. flash-full
only loads the bootloader + app sectors, so the config page survives it.

JLinkExe must be on PATH. With --dry-run (or if JLinkExe is missing) the files
are written and the manual command is printed instead.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_config_record as gcr  # noqa: E402

JLINK_DEVICE = "STM32G031F8"
JLINK_SPEED = 4000
CONFIG_ADDR = gcr.CONFIG_ADDR
CONFIG_PAGE_END = CONFIG_ADDR + 0x7FF  # 2 KB Config A page


def build_jlink_script(hex_path):
    # loadfile erases the affected sector and verifies the write itself.
    return "\n".join([
        "si SWD",
        f"speed {JLINK_SPEED}",
        f"device {JLINK_DEVICE}",
        "connect",
        "halt",
        f"erase 0x{CONFIG_ADDR:08X} 0x{CONFIG_PAGE_END:08X}",
        f"loadfile {hex_path}",
        "r",
        "g",
        "qc",
        "",
    ])


SOFTWARE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Devices offered by the interactive prompt: (label, --module name).
DEVICE_CHOICES = [
    ("ControllerNode", "controller"),
    ("TemperatureNode", "temperature"),
]

# ConfigStore::Module value -> `make flash-full MODULE=` target name.
FLASH_FULL_MODULES = {
    1: "controllerNode",
    2: "temperatureNode",
}


def ask_module():
    sys.stderr.write("Device:\n")
    for index, (label, _) in enumerate(DEVICE_CHOICES, 1):
        sys.stderr.write(f"  {index}) {label}\n")
    while True:
        answer = input(f"select 1..{len(DEVICE_CHOICES)}: ").strip().lower()
        if answer.isdigit() and 1 <= int(answer) <= len(DEVICE_CHOICES):
            return DEVICE_CHOICES[int(answer) - 1][1]
        if answer in gcr.MODULES:
            return answer
        sys.stderr.write("  not a valid choice\n")


def ask_node_id():
    while True:
        answer = input(f"Node id (1..{gcr.MAX_NODES}): ").strip()
        if answer.isdigit() and 1 <= int(answer) <= gcr.MAX_NODES:
            return int(answer)
        sys.stderr.write("  not a valid node id\n")


def ask_flash_full():
    while True:
        answer = input("Also flash bootloader + app (flash-full)? [y/N]: ").strip().lower()
        if answer in ("", "n", "no"):
            return False
        if answer in ("y", "yes"):
            return True
        sys.stderr.write("  answer y or n\n")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--node-id", type=int,
                    help=f"permanent bus address, 1..{gcr.MAX_NODES} "
                    "(asked for if omitted)")
    ap.add_argument("--module", choices=sorted(gcr.MODULES),
                    help="device type (asked for if omitted)")
    ap.add_argument("--settings", type=gcr._parse_settings, default=b"",
                    help="up to 16 bytes of per-node config as hex")
    ap.add_argument("--force", action="store_true",
                    help="allow a non-bus-node module type")
    ap.add_argument("--out-dir", help="keep the generated files here "
                    "(default: a temp dir, removed on success)")
    ap.add_argument("--jlink", default="JLinkExe", help="JLinkExe executable")
    ap.add_argument("--serial", help="J-Link serial number (multiple probes)")
    ap.add_argument("--dry-run", action="store_true",
                    help="write the files and print the command, do not flash")
    ap.add_argument("--flash-full", action=argparse.BooleanOptionalAction,
                    help="after provisioning, run `make flash-full` for this module "
                    "(asked for if omitted)")
    ap.add_argument("--build-type", default="Release", choices=["Release", "Debug"],
                    help="BUILD_TYPE for --flash-full (default: Release)")
    args = ap.parse_args(argv)

    if args.module is None or args.node_id is None:
        if not sys.stdin.isatty():
            ap.error("--node-id and --module are required when not run interactively")
        try:
            if args.module is None:
                args.module = ask_module()
            if args.node_id is None:
                args.node_id = ask_node_id()
        except (EOFError, KeyboardInterrupt):
            sys.stderr.write("\naborted\n")
            return 1

    module = gcr.MODULES[args.module]
    if module == gcr.THERMOSTAT and not args.force:
        ap.error("a Thermostat is not provisioned per unit -- its fixed record "
                 "is part of flash-full (make flash-full MODULE=thermostat)")
    if module not in gcr.PROVISIONABLE and not args.force:
        ap.error(f"module '{args.module}' is not a provisioned bus node; --force to override")

    try:
        record = gcr.build_record(args.node_id, module, args.settings)
    except ValueError as exc:
        ap.error(str(exc))

    if args.flash_full and module not in FLASH_FULL_MODULES:
        ap.error(f"no flash-full target for module '{args.module}'")
    if args.flash_full is None:
        args.flash_full = False
        if module in FLASH_FULL_MODULES and sys.stdin.isatty():
            try:
                args.flash_full = ask_flash_full()
            except (EOFError, KeyboardInterrupt):
                sys.stderr.write("\naborted\n")
                return 1
    make_cmd = ["make", "-C", SOFTWARE_DIR, "flash-full",
                f"MODULE={FLASH_FULL_MODULES.get(module)}", f"BUILD_TYPE={args.build_type}"]
    sys.stderr.write(gcr._describe(record) + "\n")

    out_dir = args.out_dir or tempfile.mkdtemp(prefix="provision-")
    os.makedirs(out_dir, exist_ok=True)
    stem = os.path.join(out_dir, f"config-node{args.node_id}-{args.module}")
    hex_path = stem + ".hex"
    bin_path = stem + ".bin"
    jlink_path = stem + ".jlink"

    with open(hex_path, "w", newline="") as handle:
        handle.write(gcr.render_ihex(record))
    with open(bin_path, "wb") as handle:
        handle.write(gcr.render_bin(record))
    with open(jlink_path, "w") as handle:
        handle.write(build_jlink_script(hex_path))
    sys.stderr.write(f"wrote {hex_path}\n     {bin_path}\n     {jlink_path}\n")

    jlink = shutil.which(args.jlink)
    cmd = [args.jlink, "-NoGui", "1", "-ExitOnError", "1", "-AutoConnect", "1",
           "-Device", JLINK_DEVICE, "-If", "SWD", "-Speed", str(JLINK_SPEED)]
    if args.serial:
        cmd += ["-SelectEmuBySN", args.serial]
    cmd += ["-CommanderScript", jlink_path]

    if args.dry_run or jlink is None:
        if jlink is None and not args.dry_run:
            sys.stderr.write(f"\n{args.jlink} not found on PATH -- run this yourself:\n")
        else:
            sys.stderr.write("\ndry run -- run this to flash:\n")
        sys.stderr.write("  " + " ".join(cmd) + "\n")
        if args.flash_full:
            sys.stderr.write("  " + " ".join(make_cmd) + "\n")
        return 0 if args.dry_run else 1

    sys.stderr.write("\n" + " ".join(cmd) + "\n")
    result = subprocess.run(cmd)
    if result.returncode != 0:
        return result.returncode
    if not args.out_dir:
        shutil.rmtree(out_dir, ignore_errors=True)

    if args.flash_full:
        if args.serial:
            sys.stderr.write("note: flash-full does not take --serial -- it uses the "
                             "default J-Link\n")
        sys.stderr.write("\n" + " ".join(make_cmd) + "\n")
        return subprocess.run(make_cmd).returncode
    return 0


if __name__ == "__main__":
    sys.exit(main())
