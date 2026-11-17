#!/usr/bin/env python3
"""Extrahiert die Menge aller const-Strings aus allen Methoden einer APK.

Nutzung: apk_stringsets.py <apk> <out.txt>

Ausgabe: eine sortierte, eindeutige Zeile pro String (ohne Anfuehrungszeichen).
Damit lassen sich Versions-Diffs fahren:
  apk_stringsets.py alt.apk alt.txt && apk_stringsets.py neu.apk neu.txt
  comm -23 alt.txt neu.txt   # nur in alt
"""
from __future__ import annotations

import logging
import sys
import zipfile

logging.disable(logging.CRITICAL)
try:  # androguard >=4 loggt ueber loguru
    from loguru import logger as _lg

    _lg.remove()
except Exception:  # pragma: no cover
    pass

from androguard.core.dex import DEX  # noqa: E402


def main() -> int:
    apk, out = sys.argv[1], sys.argv[2]
    z = zipfile.ZipFile(apk)
    seen: set[str] = set()
    for dexname in [n for n in z.namelist() if n.endswith(".dex")]:
        dex = DEX(z.read(dexname))
        for cls in dex.get_classes():
            for meth in cls.get_methods():
                try:
                    instrs = list(meth.get_instructions())
                except Exception:
                    continue
                for ins in instrs:
                    if not ins.get_name().startswith("const-string"):
                        continue
                    txt = ins.get_output()
                    q = txt.find('"')
                    if q < 0:
                        continue
                    seen.add(txt[q + 1:].rstrip('"'))
    with open(out, "w", encoding="utf-8", errors="replace") as fh:
        for s in sorted(seen):
            fh.write(s.replace("\n", "\\n") + "\n")
    print(f"{apk}: {len(seen)} distinct const-strings -> {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
