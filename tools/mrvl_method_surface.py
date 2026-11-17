#!/usr/bin/env python3
"""Extract the miIO method surface (handler table + LAN whitelist) from a Marvell
88MW30x Yeelight firmware image (MRVL container).

Usage:
    python3 tools/mrvl_method_surface.py <firmware.bin> [<firmware.bin> ...]

Prints, per build:
  * container header (timestamp, segments, entry)
  * firmware version strings found in .rodata
  * every {handler_ptr, name_ptr} handler table in the vector/table segment
  * every RAM pointer array (method-name whitelist candidates) in the .data segment

No Ghidra required: the tables are plain pointer arrays in the loaded segments.
"""
import struct, re, sys
from datetime import datetime, timezone

def parse_container(path):
    data = open(path, "rb").read()
    if data[:4] != b"MRVL":
        raise ValueError(path + ": not an MRVL image")
    magic, magic2, ts, seg_cnt, entry = struct.unpack_from("<4sIIII", data, 0)
    pos = 0x14
    segs = []
    for _ in range(seg_cnt):
        stype, off, size, laddr, crc = struct.unpack_from("<IIIII", data, pos)
        pos += 20
        segs.append({"type": stype, "off": off, "size": size, "addr": laddr, "crc": crc})
    return data, ts, entry, segs

def seg_bytes(data, seg):
    return data[seg["off"]:seg["off"] + seg["size"]]

def cstr(data, base, addr, maxlen=64):
    if not (base <= addr < base + len(data)):
        return None
    o = addr - base
    e = data.find(b"\x00", o, o + maxlen)
    if e < 0:
        return None
    s = data[o:e]
    if not s or any(c < 0x20 or c > 0x7e for c in s):
        return None
    return s.decode()

def find_handler_tables(code, code_base, ro, ro_base):
    """Runs of >=3 consecutive {handler_ptr, name_ptr} pairs pointing into .rodata."""
    runs = []
    for align in (0, 4):
        cur = []
        i = align // 4
        while i < len(code) // 4 - 1:
            h = struct.unpack_from("<I", code, 4 * i)[0]
            n = struct.unpack_from("<I", code, 4 * i + 4)[0]
            s = cstr(ro, ro_base, n)
            ok = (ro_base <= h < ro_base + len(ro)) and s and re.fullmatch(r"[A-Za-z0-9_]{3,48}", s)
            if ok:
                cur.append((code_base + 4 * i, h, s))
            else:
                if len(cur) >= 3:
                    runs.append(cur)
                cur = []
            i += 2
        if len(cur) >= 3:
            runs.append(cur)
    runs.sort(key=lambda r: r[0][0])
    return runs

def find_pointer_runs(data_seg, data_base, ro, ro_base, minlen=8):
    """Runs of >=minlen consecutive .rodata pointers inside a RAM/.data segment."""
    words = [struct.unpack_from("<I", data_seg, 4 * i)[0] for i in range(len(data_seg) // 4)]
    runs, cur = [], []
    for i, w in enumerate(words):
        if ro_base <= w < ro_base + len(ro):
            cur.append((data_base + 4 * i, w))
        else:
            if len(cur) >= minlen:
                runs.append(cur)
            cur = []
    if len(cur) >= minlen:
        runs.append(cur)
    return runs

def version_strings(ro, ro_base):
    out = []
    seen = set()
    for m in re.finditer(rb"[0-9]\.[0-9]\.[0-9]_[0-9]{4}", ro):
        s = m.group().decode()
        if s not in seen:
            seen.add(s)
            out.append((ro_base + m.start(), s))
    return out

def analyse(path):
    data, ts, entry, segs = parse_container(path)
    print("=" * 78)
    print("FILE      : " + path + "  (" + str(len(data)) + " bytes)")
    print("TIMESTAMP : %d  (%sZ)  entry=0x%08x" % (ts, datetime.fromtimestamp(ts, timezone.utc).isoformat(), entry))
    for s in segs:
        print("  seg load=0x%08x size=0x%05x crc=0x%08x" % (s["addr"], s["size"], s["crc"]))
    code_seg = max(segs, key=lambda s: s["size"])
    data_seg = min(segs, key=lambda s: s["size"])
    vec_seg = [s for s in segs if s is not code_seg and s is not data_seg][0]
    code, code_base = seg_bytes(data, code_seg), code_seg["addr"]
    ro, ro_base = code, code_base
    db, dbase = seg_bytes(data, data_seg), data_seg["addr"]
    vec, vbase = seg_bytes(data, vec_seg), vec_seg["addr"]
    vs = version_strings(ro, ro_base)
    print("VERSIONS  : " + (str(vs) if vs else "none"))
    for run in find_handler_tables(vec, vbase, ro, ro_base):
        print("TABLE     : @0x%08x entries=%d" % (run[0][0], len(run)))
        print("            " + str([r[2] for r in run]))
    for run in find_pointer_runs(db, dbase, ro, ro_base):
        names = [cstr(ro, ro_base, a) for _, a in run]
        if all(n and re.fullmatch(r"[A-Za-z0-9_]{2,48}", n) for n in names):
            print("RAM-ARRAY : @0x%08x entries=%d  <-- method whitelist candidate" % (run[0][0], len(run)))
            print("            " + str(names))
        else:
            print("ram-array : @0x%08x entries=%d (not a name array)" % (run[0][0], len(run)))

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    for p in sys.argv[1:]:
        analyse(p)
