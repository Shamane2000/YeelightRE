#!/usr/bin/env python3
"""Parse and unpack Marvell 88MW300/88MW302 (MRVL) firmware images in pure Python."""

import argparse
import os
import struct


def parse_mrvl(path: str, out_dir: str | None = None) -> None:
    data = open(path, "rb").read()
    if len(data) < 0x30 or data[:4] != b"MRVL":
        raise ValueError(f"Not a MRVL firmware image (magic={data[:4]!r})")

    magic, magic2, timestamp, seg_cnt, entry = struct.unpack_from("<4sIIII", data, 0)
    print(f"File: {path} ({len(data)} bytes)")
    print(f"Magic={magic.decode()} Magic2=0x{magic2:08x} Timestamp={timestamp} Segments={seg_cnt} Entry=0x{entry:08x}")

    pos = 0x14
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    for idx in range(seg_cnt):
        seg_type, offset, size, laddr, crc = struct.unpack_from("<IIIII", data, pos)
        pos += 20
        print(f"  Segment {idx+1}: type={seg_type} file_off=0x{offset:x} size=0x{size:x} ({size}) load_addr=0x{laddr:08x} crc=0x{crc:08x}")
        if out_dir:
            out_path = os.path.join(out_dir, f"seg{idx+1}_0x{laddr:08x}.bin")
            with open(out_path, "wb") as wf:
                wf.write(data[offset:offset + size])
            print(f"    -> wrote {out_path}")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("firmware_bin")
    ap.add_argument("--extract", help="Output directory for segment binaries")
    args = ap.parse_args()
    parse_mrvl(args.firmware_bin, args.extract)
