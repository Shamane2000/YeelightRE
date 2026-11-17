#!/usr/bin/env python3
"""Findet in einem APK alle Methoden, die einen Nadel-String als Konstanten
enthalten, und dumpt deren Dalvik-Instruktionen (const-string / invoke-*).

Schnell: parst nur die DEX-Container (androguard.core.dex.DEX), ohne
Cross-Reference-Analyse -> ~5 s pro 40-MB-APK auf devastator.

Nutzung:
  python3 apk_method_owner.py <apk> <needle> [<needle> ...]
                              [--max-methods N] [--full]
"""
from __future__ import annotations

import re
import sys
import time
import zipfile
from collections import defaultdict

from androguard.core.dex import DEX


def scan(apk_path: str, needles: list[str], full: bool, max_methods: int, owners_only: bool = False, any_match: bool = False) -> None:
    z = zipfile.ZipFile(apk_path)
    dexes = [n for n in z.namelist() if n.endswith(".dex")]
    hits: dict[str, list[tuple[str, str, str, list[str]]]] = defaultdict(list)
    t0 = time.time()
    for dexname in dexes:
        dex = DEX(z.read(dexname))
        for cls in dex.get_classes():
            cname = cls.get_name()
            for meth in cls.get_methods():
                code = meth.get_code()
                if code is None:
                    continue
                consts: list[str] = []
                instrs: list[str] = []
                for ins in meth.get_instructions():
                    name = ins.get_name()
                    out = ins.get_output()
                    instrs.append(f"{name} {out}")
                    if ins.get_name() in ("const-string", "const-string/jumbo"):
                        m = re.search(r'"([^"]*)"', out)
                        if m:
                            consts.append(m.group(1))
                for n in needles:
                    if any_match:
                        if any(n in line for line in instrs):
                            hits[n].append((dexname, cname, meth.get_name(), instrs))
                    elif any(c == n or n in c for c in consts):
                        hits[n].append((dexname, cname, meth.get_name(), instrs))

    print(f"# {apk_path}  dex={dexes}  scan={time.time()-t0:.1f}s")
    for n in needles:
        rows = hits.get(n, [])
        print("=" * 78)
        print(f"### Nadel '{n}': {len(rows)} Methoden")
        if owners_only:
            for dexname, cname, mname, instrs in rows:
                print(f"    [{dexname}] {cname}->{mname}  ({len(instrs)} instr)")
            continue
        for dexname, cname, mname, instrs in rows[:max_methods]:
            print(f"\n--- [{dexname}] {cname}->{mname}  ({len(instrs)} instr)")
            if full:
                for i, line in enumerate(instrs):
                    print(f"  {i:4d}  {line}")
            else:
                for line in instrs:
                    if line.startswith(("const-string", "invoke", "iget", "sget", "sput", "new-instance")):
                        print(f"        {line}")


if __name__ == "__main__":
    argv = sys.argv[1:]
    full = "--full" in argv
    owners_only = "--owners" in argv
    any_match = "--any" in argv
    argv = [a for a in argv if a not in ("--full", "--owners", "--any")]
    max_methods = 8
    if "--max-methods" in argv:
        i = argv.index("--max-methods")
        max_methods = int(argv[i + 1])
        del argv[i:i + 2]
    scan(argv[0], argv[1:], full, max_methods, owners_only, any_match)
