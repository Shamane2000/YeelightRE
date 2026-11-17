#!/usr/bin/env python3
"""Findet Literal-Pool-Referenzen (LE32) auf gegebene Adressen in den
entpackten `color2`-Segmenten und ordnet sie per Ghidra-`functions.txt`
der enthaltenen Funktion zu.

Aufruf:
    python3 tools/color2_xref.py <export_dir> <unpacked_dir> 0x1f043fa8 [...]
"""
import bisect
import os
import struct
import sys

SEGMENTS = [("seg1", 0x00100000, "seg1_0x00100000.bin"),
            ("seg2", 0x1F0032D8, "seg2_0x1f0032d8.bin"),
            ("seg3", 0x20000040, "seg3_0x20000040.bin")]


def load_functions(export_dir):
    rows = []
    with open(os.path.join(export_dir, "functions.txt")) as fh:
        for line in fh:
            addr, size, name = line.rstrip("\n").split("\t")
            rows.append((int(addr, 16), int(size), name))
    rows.sort()
    return rows


def containing(rows, starts, addr):
    idx = bisect.bisect_right(starts, addr) - 1
    if idx < 0:
        return None
    base, size, name = rows[idx]
    if base <= addr < base + max(size, 2):
        return rows[idx]
    return None


def cstring(blobs, addr, limit=64):
    for _, base, data in blobs:
        if base <= addr < base + len(data):
            raw = data[addr - base:addr - base + limit].split(b"\x00")[0]
            try:
                return raw.decode("ascii")
            except UnicodeDecodeError:
                return "<bin>"
    return ""


def main():
    export_dir, unpacked = sys.argv[1], sys.argv[2]
    targets = [int(a, 16) for a in sys.argv[3:]]
    rows = load_functions(export_dir)
    starts = [r[0] for r in rows]
    blobs = []
    for name, base, fname in SEGMENTS:
        with open(os.path.join(unpacked, fname), "rb") as fh:
            blobs.append((name, base, fh.read()))
    for target in targets:
        print("## le32-xrefs -> 0x%08x %r" % (target, cstring(blobs, target)))
        hits = 0
        for name, base, data in blobs:
            for off in range(0, len(data) - 4, 2):
                if struct.unpack_from("<I", data, off)[0] in (target, target + 1):
                    addr = base + off
                    fn = containing(rows, starts, addr)
                    print("   %s 0x%08x -> %s" % (
                        name, addr,
                        "0x%08x size=%d %s" % fn if fn else "-"))
                    hits += 1
        if not hits:
            print("   (keine Treffer)")
    print("done")


if __name__ == "__main__":
    main()
