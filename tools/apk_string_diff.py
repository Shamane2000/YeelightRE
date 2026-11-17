#!/usr/bin/env python3
"""Vergleicht zwei const-String-Mengen (aus apk_stringsets.py) und schreibt
einen nach Kategorien gefilterten Diff-Report (Markdown).

Nutzung: apk_string_diff.py <alt.txt> <neu.txt> <alt-label> <neu-label> <out.md>
"""
from __future__ import annotations

import re
import sys

# Nur druckbares ASCII, max. 120 Zeichen -> filtert Regex-/Binaermuell raus
PRINTABLE = re.compile(r"^[\x20-\x7e]{3,120}$")

CATEGORIES: list[tuple[str, re.Pattern[str]]] = [
    ("miIO-/Local-API-Methoden", re.compile(r"^(miIO|miot)\.", re.I)),
    ("Provisioning/SmartConfig", re.compile(r"(smart_config|smartconfig|config_router|wifi_config|ap_config|bind_key|bindkey)", re.I)),
    ("LAN-Control / GeekMode", re.compile(r"(cfg_lan_ctrl|lan_ctrl|geek)", re.I)),
    ("OTA/Firmware", re.compile(r"(ota|upgrade|firmware|oad|latest_version)", re.I)),
    ("Tokens/Handshake", re.compile(r"(handshake|hencrypt|_token|token=|FULL_TOKEN)", re.I)),
    ("Cloud-Endpunkte", re.compile(r"^(/|https?://)[\w./-]+$")),
    ("Developer/Debug", re.compile(r"(developer|debug_mode|devupgrade|is_debug)", re.I)),
]


def load(path: str) -> set[str]:
    with open(path, encoding="utf-8", errors="replace") as fh:
        return {ln.rstrip("\n") for ln in fh if ln.strip()}


def main() -> int:
    alt, neu, alt_label, neu_label, out = sys.argv[1:6]
    a, b = load(alt), load(neu)
    only_alt, only_new = a - b, b - a
    lines = [
        f"# const-String-Diff {alt_label} vs {neu_label}",
        "",
        f"- {alt_label}: {len(a)} Strings",
        f"- {neu_label}: {len(b)} Strings",
        f"- nur in {alt_label}: {len(only_alt)}",
        f"- nur in {neu_label}: {len(only_new)}",
        "",
    ]
    for title, rx in CATEGORIES:
        sel_alt = sorted(s for s in only_alt if PRINTABLE.match(s) and rx.search(s))
        sel_new = sorted(s for s in only_new if PRINTABLE.match(s) and rx.search(s))
        lines.append(f"## {title}")
        lines.append("")
        lines.append(f"### nur in {alt_label} ({len(sel_alt)})")
        lines.append("```")
        lines.extend(sel_alt[:40] or ["(keine)"])
        lines.append("```")
        lines.append("")
        lines.append(f"### nur in {neu_label} ({len(sel_new)})")
        lines.append("```")
        lines.extend(sel_new[:40] or ["(keine)"])
        lines.append("```")
        lines.append("")
    with open(out, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))
    print(f"{out} geschrieben")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
