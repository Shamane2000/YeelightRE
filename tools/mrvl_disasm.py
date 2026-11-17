#!/usr/bin/env python3
"""Disassemble a load-address range of an unpacked MRVL/Yeelight image with capstone.

Usage:
    python3 tools/mrvl_disasm.py <unpacked_dir> <start_addr> <end_addr>

The image segments (seg1 @0x00100000, seg2 @0x1f0032d8, seg3 @0x20000040) are
mapped at their load addresses; the disassembly is done in ARM Thumb-2 mode,
which is what the 88MW30x application code uses.
"""
import sys
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_LITTLE_ENDIAN

SEGMENTS = [
    ("seg1_0x00100000.bin", 0x00100000),
    ("seg2_0x1f0032d8.bin", 0x1F0032D8),
    ("seg3_0x20000040.bin", 0x20000040),
]


def image(unpacked):
    blobs = []
    for fname, base in SEGMENTS:
        with open(unpacked + "/" + fname, "rb") as fh:
            blobs.append((base, fh.read()))
    return blobs


def slice_addr(blobs, start, end):
    for base, data in blobs:
        if base <= start < base + len(data):
            return base, data[start - base:min(len(data), end - base)]
    raise SystemExit("address 0x%08x not mapped" % start)


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(2)
    unpacked, start, end = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3], 0)
    blobs = image(unpacked)
    base, chunk = slice_addr(blobs, start, end)
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_LITTLE_ENDIAN)
    for ins in md.disasm(chunk, start):
        print("0x%08x  %-8s %-8s %s" % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str))


if __name__ == "__main__":
    main()
