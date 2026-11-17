#!/usr/bin/env python3
"""Dumpt alle Methoden (inkl. Dalvik-Instruktionen) aller Klassen, deren Name
einen Teilstring enthaelt.

Nutzung:
  apk_class_dump.py <apk> <klassen-teilstring> [--methods-only] [--max-classes N]

Beispiel:
  apk_class_dump.py apk/Yeelight_3.5.4_35402.apk connection/wifi/step/ApConfigStep

Hinweis zur androguard-API (Wichtig, Bugfix 2026-11):
  `EncodedMethod.get_code()` liefert ein `DalvikCode`-Objekt. Die Instruktionen
  haengen NICHT an `DalvikCode` selbst, sondern am Bytecode darunter:
      code.get_bc().get_instructions()
  Ein Direktaufruf `method.get_instructions()` bzw. `code.get_instructions()`
  wirft `AttributeError` und erzeugt "-1 instr"-Artefakte. `_instructions()`
  deckt beide API-Varianten ab (androguard 3.x Analyse-Objekte und 4.x DEX).
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


def _instructions(meth):
    """Liefert die Instruktionsliste einer Methode (leer, wenn kein Code).

    Robust gegen die androguard-API-Varianten:
      * 4.x DEX-Objekte:  meth.get_code().get_bc().get_instructions()
      * 3.x Analyse:      method_analysis.get_instructions()
      * Fallback:         code.get_instructions()
    """
    code = meth.get_code()
    if code is None:
        return []
    bc = None
    if hasattr(code, "get_bc"):
        bc = code.get_bc()
    for obj in (bc, code, meth):
        if obj is None:
            continue
        getter = getattr(obj, "get_instructions", None)
        if callable(getter):
            return list(getter())
    raise AttributeError(
        f"keine get_instructions-API auf {type(code).__name__} / {type(meth).__name__}"
    )


def main() -> int:
    argv = sys.argv[1:]
    methods_only = "--methods-only" in argv
    argv = [a for a in argv if a != "--methods-only"]
    max_classes = 20
    if "--max-classes" in argv:
        i = argv.index("--max-classes")
        max_classes = int(argv[i + 1])
        del argv[i:i + 2]
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
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
                print(f"\n# {apk}: {shown} Klassen, {time.time() - t0:.1f}s (max-classes erreicht)")
                return 0
            shown += 1
            print(f"\n{'=' * 78}\n### [{dexname}] {cname}")
            for meth in cls.get_methods():
                try:
                    instrs = _instructions(meth)
                except Exception as exc:  # defekter Code-Item
                    print(f"\n--- {meth.get_name()}  (FEHLER)")
                    print(f"    !! {exc}")
                    continue
                print(f"\n--- {meth.get_name()}  ({len(instrs)} instr)")
                if methods_only:
                    continue
                for i, ins in enumerate(instrs):
                    print(f"  {i:4d}  {ins.get_name()} {ins.get_output()}")
    print(f"\n# {apk}: {shown} Klassen, {time.time() - t0:.1f}s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
