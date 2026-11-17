#!/usr/bin/env python3
"""Deterministischer Verifier fuer den Firmware-Diff alt vs. neu (LAN-Whitelist/`color`).

Prueft die Aussagen aus `analysis/color_fw_lan_whitelist_diff.md` gegen die Rohbinaries.
Reines stdlib, kein Ghidra/capstone noetig.

Aufruf (auf devastator, wo die Binaries liegen):
    python3 tools/color_verify_diff.py [--root <repo>]

Exit 0 = alle Assertions gruen, Exit 1 = mindestens eine Assertion rot, Exit 2 = Binaries fehlen.
"""
import argparse
import os
import re
import struct
import sys

SEG1, SEG2, SEG3 = 0x00100000, 0x1F0032D8, 0x20000040
TABLE = 0x00103098          # {handler_ptr, name_ptr} x47, in allen MRVL-Builds gleich
LAN_TABLE = 0x00103118      # LAN-Handler-Tabelle = Voll[16:], 31 Eintraege
WHITELIST = 0x200009E0      # char*[] der 20 LAN-erlaubten Methoden (color/color2)
DISPATCH_C2 = 0x1F00FEC4    # Ghidra FUN_1f00fec4 (color2 1.4.2_0027)
DISPATCH_C1 = 0x1F00FE74    # gleiche Funktion in color 1.4.2_0061 (0x50 frueher)
DISPATCH_LEN = 492

BUILDS = {
    "color1_142_0061": "firmware/color1_142_0061/upd_yeelink.light.color1.bin",
    "color2_142_0027": "firmware/color2_142_0027/upd_yeelink.light.color2.bin",
    "mono1_142_0055": "firmware/ref_dustcloud/mono1_ffa341cf1e00bbc1ebca4470f64dc5cb_upd_yeelink.light.mono1.bin",
    "ct2_142_0015": "firmware/ref_dustcloud/ct2_50e36abab2bab04d047b8f8020ac2f6e_upd_yeelink.light.ct2.bin",
}

errors = []


def check(cond, msg):
    if cond:
        print("  ok   %s" % msg)
    else:
        print("  FAIL %s" % msg)
        errors.append(msg)


def load(path):
    data = open(path, "rb").read()
    assert data[:4] == b"MRVL", path
    _, _, _, seg_cnt, _ = struct.unpack_from("<4sIIII", data, 0)
    segs = []
    for i in range(seg_cnt):
        stype, off, size, laddr, crc = struct.unpack_from("<IIIII", data, 0x14 + 20 * i)
        segs.append({"off": off, "size": size, "addr": laddr})
    return data, segs


def seg(data, segs, addr):
    for s in segs:
        if s["addr"] == addr:
            return data[s["off"]:s["off"] + s["size"]]
    raise KeyError(hex(addr))


def cstr(code, addr):
    if not (SEG2 <= addr < SEG2 + len(code)):
        return None
    o = addr - SEG2
    e = code.find(b"\x00", o, o + 64)
    if e < 0:
        return None
    raw = code[o:e]
    if not raw or any(c < 0x20 or c > 0x7e for c in raw):
        return None
    return raw.decode()


def table(vec, code, base):
    """Die {handler,name}-Paare liegen in seg1, die Namen-Strings in seg2."""
    names = []
    for k in range(47):
        h, n = struct.unpack_from("<II", vec, base - SEG1 + 8 * k)
        names.append(cstr(code, n))
    return names


def whitelist(db, code, addr):
    out = []
    o = addr - SEG3
    while True:
        (p,) = struct.unpack_from("<I", db, o)
        if p == 0:
            break
        out.append(cstr(code, p))
        o += 4
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    args = ap.parse_args()
    images, missing = {}, []
    for key, rel in BUILDS.items():
        path = os.path.join(args.root, rel)
        if os.path.exists(path):
            images[key] = load(path)
        else:
            missing.append(rel)
    for rel in missing:
        print("  skip %s (nicht vorhanden)" % rel)
    if not images:
        print("FAIL: keine Firmware-Binaries gefunden")
        return 2

    print("[1] Container-Struktur")
    for key, (data, segs) in images.items():
        check(len(segs) == 3 and [s["addr"] for s in segs] == [SEG1, SEG2, SEG3],
              "%s: 3 Segmente an 0x00100000/0x1f0032d8/0x20000040" % key)

    print("[2] Methodentabelle @0x%08x (47 Eintraege, identisch)" % TABLE)
    tables = {}
    for key, (data, segs) in images.items():
        code = seg(data, segs, SEG2)
        tables[key] = table(seg(data, segs, SEG1), code, TABLE)
        check(all(tables[key]), "%s: alle 47 Name-Pointer aufloesbar" % key)
        check(len(tables[key]) == 47, "%s: 47 Tabelleneintraege" % key)
    for key in tables:
        if "color1_142_0061" in tables:
            check(tables[key] == tables["color1_142_0061"],
                  "%s: Tabelle identisch zu color1 1.4.2_0061" % key)
    ref = tables.get("color1_142_0061") or list(tables.values())[0]
    for m in ("set_ps", "get_ps", "config_router", "ota"):
        check(m + "_JsDlgt" in ref, "%s im Firmware-Image vorhanden (Handler existiert)" % m)

    print("[3] LAN-Whitelist @0x%08x" % WHITELIST)
    for key in ("color1_142_0061", "color2_142_0027"):
        if key not in images:
            continue
        data, segs = images[key]
        wl = whitelist(seg(data, segs, SEG3), seg(data, segs, SEG2), WHITELIST)
        check(len(wl) == 20, "%s: 20 Whitelist-Eintraege" % key)
        for forbidden in ("set_ps", "get_ps", "cfg_lan_ctrl", "reboot", "config_router", "ota"):
            check(forbidden not in wl, "%s: '%s' NICHT LAN-whitelisted" % (key, forbidden))
        check(wl[0] == "get_prop" and wl[-1] == "set_name",
              "%s: Whitelist beginnt mit get_prop, endet mit set_name" % key)
        if key == "color2_142_0027" and "color1_142_0061" in images:
            d1, s1 = images["color1_142_0061"]
            wl1 = whitelist(seg(d1, s1, SEG3), seg(d1, s1, SEG2), WHITELIST)
            check(wl == wl1, "color2 1.4.2_0027: Whitelist identisch zu color 1.4.2_0061")

    print("[4] Dispatch-Funktion identisch (nur Adress-Konstanten verschoben)")
    if "color1_142_0061" in images and "color2_142_0027" in images:
        c1 = seg(*images["color1_142_0061"], SEG2)
        c2 = seg(*images["color2_142_0027"], SEG2)
        b1 = c1[DISPATCH_C1 - SEG2:DISPATCH_C1 - SEG2 + DISPATCH_LEN]
        b2 = c2[DISPATCH_C2 - SEG2:DISPATCH_C2 - SEG2 + DISPATCH_LEN]
        prologue = bytes.fromhex("2de9f043c1b0074618468846")
        check(b1[:12] == prologue, "color1 @0x%08x: Prolog == color2-Prolog" % DISPATCH_C1)
        check(b2[:12] == prologue, "color2 @0x%08x: Prolog unveraendert" % DISPATCH_C2)
        diff = sum(1 for a, b in zip(b1, b2) if a != b)
        check(diff <= 64, "nur %d von %d Bytes differieren (Adress-Konstanten)" % (diff, DISPATCH_LEN))
        check(b1[-2:] == b2[-2:] and b1[0x5C:0x60] == b2[0x5C:0x60],
              "Rueckgabe-Epilog identisch (movs r0,#2 / movs r0,#0 Pfade)")

    print("[5] Whitelist-Adresse im Code referenziert")
    for key, disp in (("color1_142_0061", DISPATCH_C1), ("color2_142_0027", DISPATCH_C2)):
        if key not in images:
            continue
        code = seg(*images[key], SEG2)
        pat = struct.pack("<I", WHITELIST)
        hits = [SEG2 + i for i in range(0, len(code) - 3, 2) if code[i:i + 4] == pat]
        check(len(hits) == 1 and hits[0] == disp + 0xCC,
              "%s: 0x%08x genau einmal referenziert, @0x%08x (Dispatch+0xCC)"
              % (key, WHITELIST, disp + 0xCC))
        for addr in (TABLE, LAN_TABLE):
            pat2 = struct.pack("<I", addr)
            n = sum(1 for i in range(0, len(code) - 3, 2) if code[i:i + 4] == pat2)
            check(n >= 1, "%s: 0x%08x im Code referenziert (%d Referenz(en))" % (key, addr, n))

    print("[6] Live-Transkript fw65 (falls vorhanden)")
    tr = os.path.join(args.root, "analysis/lan_probe_fw65_192.168.1.168.txt")
    if os.path.exists(tr):
        txt = open(tr, encoding="utf-8").read()
        check('"result": ["65", "color", "1", "off"]' in txt, "fw_ver 65 / model color / lan_ctrl 1 gelesen")
        check(txt.count('"method not supported"') >= 3, "set_ps/get_ps/miio_test: 'method not supported'")
        m = re.search(r"set_ps\[cfg_lan_ctrl,1\]\s+(\{.*\})", txt)
        check(bool(m) and '"method not supported"' in m.group(1),
              "set_ps [cfg_lan_ctrl,1] wird auf fw65 abgelehnt")
        last = txt.rstrip().splitlines()[-1]
        check("readback get_prop" in last and '"result": ["1"]' in last,
              "Readback nach set_ps: lan_ctrl unveraendert 1")
    else:
        print("  skip Transkript nicht vorhanden")

    print()
    if errors:
        print("FAIL: %d Assertion(s) rot" % len(errors))
        return 1
    print("PASS: Firmware-Diff-Befunde deterministisch bestaetigt")
    return 0


if __name__ == "__main__":
    sys.exit(main())
