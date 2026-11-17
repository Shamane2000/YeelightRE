#!/usr/bin/env python3
"""Zeigt alle DEX-Strings, die einen Nadel-String enthalten - pro APK.

Nutzung: python3 apk_string_grep.py <apk> <needle> [<needle> ...]
"""
from __future__ import annotations

import re
import sys
import zipfile
from pathlib import Path


def grep(apk: Path, needles: list[str]) -> dict[str, list[str]]:
    z = zipfile.ZipFile(apk)
    out: dict[str, set[str]] = {n: set() for n in needles}
    for dex in [x for x in z.namelist() if x.endswith(".dex")]:
        data = z.read(dex)
        for m in re.finditer(rb"[\x20-\x7e]{4,300}", data):
            t = m.group().decode("latin1")
            for n in needles:
                if n in t:
                    out[n].add(t)
    return {k: sorted(v) for k, v in out.items()}


if __name__ == "__main__":
    apk = Path(sys.argv[1])
    needles = sys.argv[2:]
    res = grep(apk, needles)
    print(f"### {apk.name}")
    for n in needles:
        print(f"\n-- '{n}' ({len(res[n])} unique)")
        for t in res[n][:80]:
            print(f"   {t}")
