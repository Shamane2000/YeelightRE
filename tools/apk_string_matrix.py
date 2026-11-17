#!/usr/bin/env python3
"""DEX-String-Matrix fuer Yeelight-CHERRY-APKs (versionsuebergreifend).

Scannt alle classes*.dex im APK roh nach Protokoll-/Feature-Strings und zaehlt
Treffer. Kein jadx, kein RAM-Hunger -> laeuft auf devastator (1.6 GB) fuer alle
Versionen in einem Durchlauf.

Nutzung: python3 apk_string_matrix.py <apk> [<apk> ...] [--json out.json]
"""
from __future__ import annotations

import hashlib
import json
import re
import sys
import zipfile
from pathlib import Path

GROUPS: dict[str, list[str]] = {
    "provisioning": [
        "miIO.config_router", "config_router", "miIO.", "setMiioRouter",
        "country_domain", "bind_key", "gmt_offset", "apConfig", "ApConfig",
        "set_wifi_config", "hint_ap", "miIO.wifi_",
    ],
    "token": [
        "async_get_token", "get_token", "MiioLocalAPI", "miIO.handshake",
        "handshake", "FULL_TOKEN", "getProp", "token_expired",
    ],
    "lan_ctrl": [
        "cfg_lan_ctrl", "lan_ctrl", "lanControl", "LanControl", "enable_lan",
        "lan_ctrl_status", "isLanCtrlEnabled", "developer_mode",
        "set_developer_mode", "set_ps", "cfg_props",
    ],
    "ota": [
        "latest_version", "update_fw", "ot_token", "ota", "OTA",
        "FirmwareUpdate", "firmware", "checkFirmware", "mcu_version",
        "upgrade", "is_update", "firmwareVersion", "build_no",
    ],
    "crypto": [
        "hmac", "HMAC", "aes_encrypt", "AES/ECB", "AES/CBC", "md5",
        "JNIBridge", "encrypt", "decrypt", "sign", "sha1",
    ],
    "local": [
        "54321", "udp", "UDP", "multicast", "239.255.255.250", "ssdp",
        "SSDP", "discovery", "local_control",
    ],
}

DEX_MIN_STR = 4


def apk_meta(path: Path) -> dict:
    z = zipfile.ZipFile(path)
    meta = {
        "file": path.name,
        "size": path.stat().st_size,
        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        "dex": [n for n in z.namelist() if n.endswith(".dex")],
    }
    man = z.read("AndroidManifest.xml")
    txt = man.decode("latin1", "replace")
    # UTF-16 strings im AXML
    tries = [txt, man.decode("utf-16-le", "replace")]
    for t in tries:
        m = re.search(r"com\.yeelight\.[a-z0-9_.]*", t)
        if m:
            meta["package"] = m.group(0)
            break
    # versionName/Code stehen als UTF-16 im AXML
    u16 = man.decode("utf-16-le", "replace")
    vn = re.findall(r"\d+\.\d+\.\d+", u16)
    if vn:
        meta["versionGuess"] = sorted(set(vn), key=len, reverse=True)[:4]
    return meta


def strings_of(path: Path) -> dict[str, dict[str, int]]:
    """{key: {dexname: count}} plus Total."""
    z = zipfile.ZipFile(path)
    dexes = [n for n in z.namelist() if n.endswith(".dex")]
    blobs = {n: z.read(n) for n in dexes}
    out: dict[str, dict[str, int]] = {}
    for group, keys in GROUPS.items():
        for k in keys:
            per = {}
            for n, data in blobs.items():
                # case-sensitive Treffer im DEX-String-Pool; DEX speichert MUTF-8
                c = data.count(k.encode("utf-8"))
                if c:
                    per[n] = c
            if per:
                out[f"{group}/{k}"] = per
    return out


def sample_strings(path: Path, needle: str, limit: int = 25) -> list[str]:
    """Alle ASCII-Strings, die needle enthalten (Kontext fuer die Doku)."""
    z = zipfile.ZipFile(path)
    found = set()
    for n in [x for x in z.namelist() if x.endswith(".dex")]:
        data = z.read(n)
        for m in re.finditer(rb"[\x20-\x7e]{" + str(DEX_MIN_STR).encode() + rb",240}", data):
            t = m.group().decode("latin1")
            if needle in t:
                found.add(t[:240])
    return sorted(found)[:limit]


def main(argv: list[str]) -> int:
    args = list(argv)
    out_json = None
    if "--json" in args:
        i = args.index("--json")
        out_json = Path(args[i + 1])
        del args[i:i + 2]
    apks = [Path(a) for a in args if not a.startswith("--")]

    report = {}
    for apk in apks:
        meta = apk_meta(apk)
        print("=" * 78)
        print(f"{apk.name}  size={meta['size']}  dex={meta['dex']}")
        print(f"sha256={meta['sha256']}  versionGuess={meta.get('versionGuess')}")
        strings = strings_of(apk)
        for k in sorted(strings):
            print(f"  {k:38s} {sum(strings[k].values()):5d}  {strings[k]}")
        report[apk.name] = {"meta": meta, "strings": {k: sum(v.values()) for k, v in strings.items()}}
    if out_json:
        out_json.write_text(json.dumps(report, indent=2, ensure_ascii=False))
        print(f"\nJSON -> {out_json}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
