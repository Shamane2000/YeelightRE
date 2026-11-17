#!/usr/bin/env python3
"""Dumpt alle Methoden (inkl. Dalvik-Instruktionen) aller Klassen, deren Name
einen Teilstring enthaelt.

Nutzung:
  apk_class_dump.py <apk> <klassen-teilstring> [--methods-only] [--max-classes N]

Beispiel:
  apk_class_dump.py apk/Yeelight_3.5.4_35402.apk connection/wifi/step/ApConfigStep
"""
from __future__ import annotations

import logging
import sys
import time
import zipfile

logging.disable(logging.CRITICAL)
for name in ("androguard", "androguard.core.dex"):
    logging.getLogger(name).setLevel(logging.CRITICAL)
try:  # androguard >=4 loggt ueber loguru, nicht ueber logging
    from loguru import logger as _lg

    _lg.remove()
except Exception:  # pragma: no cover
    pass

from androguard.core.dex import DEX  # noqa: E402


def main() -> int:
    argv = sys.argv[1:]
    methods_only = "--methods-only" in argv
    argv = [a for a in argv if a != "--methods-only"]
    max_classes = 20
    if "--max-classes" in argv:
        i = argv.index("--max-classes")
        max_classes = int(argv[i + 1])
        del argv[i:i + 2]
    apk, needle = argv[0], argv[1]

    z = zipfile.ZipFile(apk)
    dexes = [n for n in z.namelist() if n.endswith(".dex")]
    t0 = time.time()
    shown = 0
    for dexname in dexes:
        dex = DEX(z.read(dexname))
        for cls in dex.get_classes():
            cname = cls.get_name()
            if needle not in cname:
                continue
            if shown >= max_classes:
                return 0
            shown += 1
            print(f"\n{'=' * 78}\n### [{dexname}] {cname}")
            for meth in cls.get_methods():
                code = meth.get_code()
                try:
                    n = len(list(meth.get_instructions())) if code else 0
                except Exception:
                    n = -1
                print(f"\n--- {meth.get_name()}  ({n} instr)")
                if methods_only or code is None:
                    continue
                try:
                    instrs = list(meth.get_instructions())
                except Exception as exc:  # defekter Code-Item
                    print(f"    !! {exc}")
                    continue
                for i, ins in enumerate(instrs):
                    print(f"  {i:4d}  {ins.get_name()} {ins.get_output()}")
    print(f"\n# {apk}: {shown} Klassen, {time.time() - t0:.1f}s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
