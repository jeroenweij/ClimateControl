#!/usr/bin/env python3
"""Post-build finalize step for an OTA-distributable ClimateControl image.

Patches the ImageDescriptor's imageSize and flags (setting FlagCrcPresent)
and appends the trailing CRC-32 that Modules/Bootloader/FirmwareSlave.cpp's
HandleEnd() checks against -- Lib/Board/ImageDescriptor.h, §4 of
Spec/Node-Flash-Layout-and-Bootloader-Spec.md.

The CRC-32 here is the STM32 CRC unit's *native* mode (Lib/HAL/Crc.cpp's
Poly::Ieee32): poly 0x04C11DB7, init 0xFFFFFFFF, no input/output reflection,
no final XOR. This is NOT the same algorithm as zlib/binascii's crc32 (which
is reflected) -- do not swap this for a stdlib/library CRC32 call, the two
produce different results over the same bytes.

Usage: finalize_image.py <in.bin> <out.bin>
"""

import struct
import sys

DESCRIPTOR_OFFSET = 0xC0
IMAGE_MAGIC = 0x43436D67  # 'CCmg'
FLAG_CRC_PRESENT = 1 << 0
IMAGE_SIZE_OFFSET = DESCRIPTOR_OFFSET + 8
FLAGS_OFFSET = DESCRIPTOR_OFFSET + 16


def crc32_native(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte << 24
        for _ in range(8):
            if crc & 0x80000000:
                crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF
            else:
                crc = (crc << 1) & 0xFFFFFFFF
    return crc


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: finalize_image.py <in.bin> <out.bin>", file=sys.stderr)
        return 1
    in_path, out_path = sys.argv[1], sys.argv[2]

    with open(in_path, "rb") as f:
        data = bytearray(f.read())

    if len(data) < DESCRIPTOR_OFFSET + 32:
        print(f"finalize_image: {in_path} is smaller than the descriptor offset", file=sys.stderr)
        return 1

    magic = struct.unpack_from("<I", data, DESCRIPTOR_OFFSET)[0]
    if magic != IMAGE_MAGIC:
        print(f"finalize_image: bad descriptor magic 0x{magic:08X} in {in_path}", file=sys.stderr)
        return 1

    # imageSize/flags must be patched to their final values *before* the CRC
    # is computed -- the descriptor is part of what HandleEnd() checksums on
    # the device, so the CRC has to cover the bytes exactly as they'll sit in
    # flash, trailing CRC itself excluded.
    final_size = len(data) + 4  # +4 for the trailing CRC-32 appended below
    struct.pack_into("<I", data, IMAGE_SIZE_OFFSET, final_size)

    flags = struct.unpack_from("<H", data, FLAGS_OFFSET)[0]
    flags |= FLAG_CRC_PRESENT
    struct.pack_into("<H", data, FLAGS_OFFSET, flags)

    crc = crc32_native(bytes(data))
    data += struct.pack("<I", crc)

    with open(out_path, "wb") as f:
        f.write(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
