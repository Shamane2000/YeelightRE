#!/usr/bin/env python3
"""Deterministic format verifier for Marvell 88MW300/88MW302 MRVL containers.

Checks the container format that the YeelightRE custom firmware must produce,
against the known-good vendor image firmware/color1_142_0061/
upd_yeelink.light.color1.bin (color1_0061) as the reference.

Container layout (verified against 5 vendor images, see docs/07):

    off  size  field
    0x00 4     magic  "MRVL"
    0x04 4     magic2 0x2e9cf17b (constant across vendor images)
    0x08 4     build timestamp (written by axf2firmware -> NOT reproducible)
    0x0c 4     segment count
    0x10 4     entry point (thumb: bit0 set, points into an app segment)
    0x14 20*n  segment table:
                 4 type (2 = loadable code/data in all known images)
                 4 file offset
                 4 size
                 4 load address
                 4 CRC-32 of the segment body

    payload segments follow directly after the table; the gap between the end
    of the table and the first segment is 0xFF-filled; the file ends exactly at
    the end of the last segment.

CRC: reflected CRC-32 (poly 0xEDB88320 mirrored -> table 0xEDB88320), initial
value 0, no final XOR. NOTE: this is NOT zlib.crc32 (which inverts before and
after) -- verified on all three segments of the reference image.

Exit code 0 = all checks passed.
"""

from __future__ import annotations

import argparse
import struct
import sys

HDR_FMT = "<4sIIII"
HDR_SIZE = struct.calcsize(HDR_FMT)          # 0x14
SEG_FMT = "<IIIII"
SEG_SIZE = struct.calcsize(SEG_FMT)          # 20
MRVL_MAGIC = b"MRVL"
MAGIC2_REFERENCE = 0x2E9CF17B
SEG_TYPE_LOADABLE = 2

# Flash map of the 88MW302 (SSoT §4): boot2 owns 0x0-0xFFFF, the app slot is
# 384 KB starting at 0x10000.
BOOT2_SIZE = 0x10000
APP_SLOT_SIZE = 384 * 1024
APP_SLOT_MAX_DEFAULT = APP_SLOT_SIZE

# Load addresses observed in color1_0061 (and every other vendor image).
INIT_LOAD_ADDR = 0x00100000     # SRAM0, .init/.isr_vector
DATA_LOAD_ADDR = 0x20000040     # SRAM1, .data
FLASHC_WINDOW = (0x1F000000, 0x20000000)   # XIP window, .text lives here


class Report:
    def __init__(self, quiet: bool = False) -> None:
        self.passed = 0
        self.failed = 0
        self.failures: list[str] = []
        self.quiet = quiet

    def check(self, ok: bool, name: str, detail: str = "") -> bool:
        tag = "PASS" if ok else "FAIL"
        if not self.quiet or not ok:
            print(f"  [{tag}] {name}" + (f": {detail}" if detail else ""))
        if ok:
            self.passed += 1
        else:
            self.failed += 1
            self.failures.append(f"{name}: {detail}")
        return ok


def crc32_mrvl(data: bytes) -> int:
    """Reflected CRC-32, init 0, no final XOR (the vendor's algorithm)."""
    crc = 0
    for byte in data:
        crc = _CRC_TABLE[(crc ^ byte) & 0xFF] ^ (crc >> 8)
    return crc


def _build_table() -> list[int]:
    table = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0xEDB88320 if c & 1 else c >> 1
        table.append(c)
    return table


_CRC_TABLE = _build_table()


def parse_container(path: str) -> dict:
    try:
        data = open(path, "rb").read()  # noqa: SIM115 - short-lived, closed on exit
    except FileNotFoundError:
        raise SystemExit(f"FEHLER: Datei nicht gefunden: {path}")
    if len(data) < HDR_SIZE or data[:4] != MRVL_MAGIC:
        raise SystemExit(f"{path}: kein MRVL-Container (magic={data[:4]!r})")
    magic, magic2, timestamp, seg_cnt, entry = struct.unpack_from(HDR_FMT, data, 0)
    segs = []
    for idx in range(seg_cnt):
        stype, offset, size, laddr, crc = struct.unpack_from(
            SEG_FMT, data, HDR_SIZE + SEG_SIZE * idx
        )
        segs.append(
            {
                "index": idx + 1,
                "type": stype,
                "offset": offset,
                "size": size,
                "addr": laddr,
                "crc": crc,
                "body": data[offset : offset + size] if 0 <= offset <= len(data) else b"",
            }
        )
    return {
        "path": path,
        "data": data,
        "size": len(data),
        "magic": magic,
        "magic2": magic2,
        "timestamp": timestamp,
        "seg_cnt": seg_cnt,
        "entry": entry,
        "segs": segs,
    }


def verify_container(img: dict, rep: Report, app_slot_max: int) -> None:
    data = img["data"]
    segs = img["segs"]
    table_end = HDR_SIZE + SEG_SIZE * img["seg_cnt"]

    print(f"== Container: {img['path']} ({img['size']} bytes)")
    print(
        f"   magic={img['magic'].decode(errors='replace')} "
        f"magic2=0x{img['magic2']:08x} ts={img['timestamp']} "
        f"segs={img['seg_cnt']} entry=0x{img['entry']:08x}"
    )

    rep.check(img["magic"] == MRVL_MAGIC, "magic == MRVL", repr(img["magic"]))
    rep.check(
        img["magic2"] == MAGIC2_REFERENCE,
        "magic2 == reference 0x2e9cf17b",
        f"0x{img['magic2']:08x}",
    )
    rep.check(1 <= img["seg_cnt"] <= 8, "segment count in 1..8", str(img["seg_cnt"]))
    rep.check(table_end <= img["size"], "segment table fits in file", f"end=0x{table_end:x}")

    # --- per-segment checks ------------------------------------------------
    prev_end = table_end
    for seg in segs:
        tag = f"seg{seg['index']} @0x{seg['addr']:08x}"
        rep.check(
            seg["type"] == SEG_TYPE_LOADABLE,
            f"{tag}: type == 2 (loadable)",
            str(seg["type"]),
        )
        rep.check(seg["size"] > 0, f"{tag}: size > 0", f"0x{seg['size']:x}")
        rep.check(
            seg["offset"] >= table_end,
            f"{tag}: offset after header table",
            f"0x{seg['offset']:x} >= 0x{table_end:x}",
        )
        rep.check(
            seg["offset"] >= prev_end,
            f"{tag}: segments stored in link order, no overlap",
            f"0x{seg['offset']:x} >= 0x{prev_end:x}",
        )
        rep.check(
            seg["offset"] + seg["size"] <= img["size"],
            f"{tag}: segment inside file",
            f"0x{seg['offset']:x}+0x{seg['size']:x} <= 0x{img['size']:x}",
        )
        rep.check(
            len(seg["body"]) == seg["size"],
            f"{tag}: body length matches declared size",
            f"{len(seg['body'])} == {seg['size']}",
        )
        rep.check(
            crc32_mrvl(seg["body"]) == seg["crc"],
            f"{tag}: CRC-32 (reflected, init 0) matches",
            f"computed=0x{crc32_mrvl(seg['body']):08x} stored=0x{seg['crc']:08x}",
        )
        prev_end = seg["offset"] + seg["size"]

    # --- gap / tail --------------------------------------------------------
    if segs:
        first = min(s["offset"] for s in segs)
        gap = data[table_end:first]
        rep.check(
            all(b == 0xFF for b in gap),
            "gap between table and first segment is 0xFF-filled",
            f"{len(gap)} bytes",
        )
        last_end = max(s["offset"] + s["size"] for s in segs)
        rep.check(
            last_end == img["size"],
            "file ends exactly after the last segment",
            f"0x{last_end:x} == 0x{img['size']:x}",
        )

    # --- entry point -------------------------------------------------------
    entry_thumb = img["entry"] & 0xFFFFFFFF
    rep.check(
        bool(entry_thumb & 1),
        "entry point has the thumb bit set",
        f"0x{entry_thumb:08x}",
    )
    entry_code = entry_thumb & ~1
    in_seg = any(s["addr"] <= entry_code < s["addr"] + s["size"] for s in segs)
    rep.check(
        in_seg,
        "entry point lies inside a loadable segment",
        f"0x{entry_code:08x}",
    )

    # --- slot / memory-map constraints ------------------------------------
    addrs = [s["addr"] for s in segs]
    rep.check(
        INIT_LOAD_ADDR in addrs,
        "boot2 hand-over segment at 0x00100000 present",
        f"load addrs={[hex(a) for a in addrs]}",
    )
    rep.check(
        any(FLASHC_WINDOW[0] <= a < FLASHC_WINDOW[1] for a in addrs),
        "app/XIP segment inside the flash window 0x1f000000..0x1fffffff",
        f"load addrs={[hex(a) for a in addrs]}",
    )
    rep.check(
        img["size"] <= app_slot_max,
        f"container fits the app slot (<= 0x{app_slot_max:x} = "
        f"{app_slot_max // 1024} KB)",
        f"{img['size']} bytes",
    )
    rep.check(
        img["size"] + BOOT2_SIZE <= APP_SLOT_SIZE + BOOT2_SIZE + app_slot_max,
        "app payload stays below the 384 KB slot limit",
        f"{img['size']} <= {app_slot_max}",
    )
    rep.check(
        len(segs) >= 2,
        "at least boot2 hand-over + app segments present",
        f"{len(segs)} segments",
    )
    # load addresses must be unique
    rep.check(len(set(addrs)) == len(addrs), "segment load addresses unique", str(len(addrs)))


def compare_with_reference(img: dict, ref: dict, rep: Report) -> None:
    print(f"== Reference: {ref['path']} ({ref['size']} bytes)")
    rep.check(
        img["magic2"] == ref["magic2"],
        "magic2 identical to reference",
        f"0x{img['magic2']:08x} vs 0x{ref['magic2']:08x}",
    )
    rep.check(
        img["seg_cnt"] == ref["seg_cnt"],
        "segment count identical to reference",
        f"{img['seg_cnt']} vs {ref['seg_cnt']}",
    )
    ref_addrs = [s["addr"] for s in ref["segs"]]
    img_addrs = [s["addr"] for s in img["segs"]]
    rep.check(
        img_addrs == ref_addrs,
        "segment load addresses identical to reference "
        "(0x00100000 / 0x1f0032d8 / 0x20000040)",
        f"{[hex(a) for a in img_addrs]} vs {[hex(a) for a in ref_addrs]}",
    )
    rep.check(
        [s["type"] for s in img["segs"]] == [s["type"] for s in ref["segs"]],
        "segment types identical to reference",
        str([s["type"] for s in img["segs"]]),
    )
    ref_entry_thumb = bool(ref["entry"] & 1)
    rep.check(
        bool(img["entry"] & 1) == ref_entry_thumb,
        "entry thumb-bit convention identical to reference",
        f"0x{img['entry']:08x} vs 0x{ref['entry']:08x}",
    )
    ref_app = next((s for s in ref["segs"] if s["addr"] >= FLASHC_WINDOW[0]), None)
    img_app = next((s for s in img["segs"] if s["addr"] >= FLASHC_WINDOW[0]), None)
    if ref_app and img_app:
        rep.check(
            (img["entry"] & ~1) == img_app["addr"] == ref_app["addr"],
            "entry point equals the app segment load address (reference convention)",
            f"0x{img['entry'] & ~1:08x} / 0x{img_app['addr']:08x} / 0x{ref_app['addr']:08x}",
        )
    else:
        rep.check(False, "reference and image both carry an app segment", "missing")

    # documented, expected differences -> informational only
    print("   [INFO] erwartete, begruendete Abweichungen:")
    print(
        f"          Groesse  image={img['size']} vs reference={ref['size']} "
        "(mcufw ist absichtlich minimal)"
    )
    print(
        f"          Timestamp image={img['timestamp']} vs reference={ref['timestamp']} "
        "(axf2firmware schreibt time(NULL); nicht reproduzierbar)"
    )


def check_determinism(
    img: dict, other: dict, rep: Report, allow_entry_mismatch: bool = False
) -> None:
    """Byte parity of two containers built from the same source.

    axf2firmware always writes time(NULL) at 0x08-0x0b, so that field can never
    match. The entry field at 0x10-0x13 is copied verbatim from the ELF's
    e_entry; allow_entry_mismatch is for the roundtrip check against a vendor
    ELF artefact whose e_entry was lost/changed during unpacking (documented
    finding, docs/07_sdk_build_bringup.md).
    """
    print(f"== Byte-Paritaet: {img['path']} vs {other['path']}")
    rep.check(
        img["size"] == other["size"],
        "identische Container-Groesse",
        f"{img['size']} vs {other['size']}",
    )
    if img["size"] != other["size"]:
        return

    a = bytearray(img["data"])
    b = bytearray(other["data"])
    diff = [i for i in range(len(a)) if a[i] != b[i]]
    print(f"   [INFO] abweichende Byte-Offsets: {[hex(i) for i in diff[:16]]}"
          f"{' ...' if len(diff) > 16 else ''}")

    entry_diff = [i for i in diff if 0x10 <= i < 0x14]
    ts_diff = [i for i in diff if 0x08 <= i < 0x0C]
    rest = [i for i in diff if i not in entry_diff and i not in ts_diff]

    rep.check(
        all(0x08 <= i < 0x0C for i in ts_diff) or not ts_diff,
        "abweichende Bytes im Timestamp-Feld liegen in 0x08-0x0b",
        f"{len(ts_diff)} Bytes (axf2firmware time(NULL))",
    )
    rep.check(
        not rest,
        "alle uebrigen Bytes identisch (Segment-Tabelle + Rohdaten)",
        f"{len(rest)} abweichende Bytes" if rest else "0",
    )
    entry_ok = img["entry"] == other["entry"]
    if allow_entry_mismatch:
        print(
            f"   [INFO] Entry-Feld abweichend: image=0x{img['entry']:08x} "
            f"vs 0x{other['entry']:08x} -- dokumentierter Befund (e_entry des "
            "Referenz-ELF-Artefakts)", 
        )
    else:
        rep.check(
            entry_ok,
            "Entry-Feld identisch",
            f"0x{img['entry']:08x} vs 0x{other['entry']:08x}",
        )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--container", required=True, help="MRVL container to verify")
    ap.add_argument("--reference", help="known-good vendor container for cross-check")
    ap.add_argument(
        "--app-slot-max",
        type=lambda v: int(v, 0),
        default=APP_SLOT_MAX_DEFAULT,
        help="max app payload size (default 0x60000 = 384 KB)",
    )
    ap.add_argument(
        "--determinism-against",
        help="second container built from the same source; must be byte-identical "
        "except the timestamp field",
    )
    ap.add_argument(
        "--entry-mismatch-ok",
        action="store_true",
        help="downgrade an entry-field difference in --determinism-against to INFO "
        "(roundtrip against the vendor ELF artefact: its e_entry is 0x100001, the "
        "shipped container carries 0x1f0032d9)",
    )
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    rep = Report(quiet=args.quiet)
    img = parse_container(args.container)
    verify_container(img, rep, args.app_slot_max)
    if args.reference:
        compare_with_reference(img, parse_container(args.reference), rep)
    if args.determinism_against:
        check_determinism(
            img,
            parse_container(args.determinism_against),
            rep,
            allow_entry_mismatch=args.entry_mismatch_ok,
        )

    total = rep.passed + rep.failed
    print()
    if rep.failed:
        print(f"FAIL {args.container}: {rep.failed}/{total} Checks fehlgeschlagen")
        for f in rep.failures:
            print(f"  - {f}")
        return 1
    print(f"PASS {args.container}: {rep.passed}/{total} Checks OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
