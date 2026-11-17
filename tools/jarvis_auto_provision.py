#!/usr/bin/env python3
"""Autonomous Yeelight AP scanner & provisioner for Jarvis (MT7601U USB Wi-Fi)."""

import argparse
import os
import re
import socket
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import yeelight_standalone as ys  # noqa: E402


def run(cmd: list[str], check: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, check=check)


def detect_wifi_iface() -> str:
    r = run(["nmcli", "-t", "-f", "DEVICE,TYPE", "dev", "status"])
    for line in r.stdout.splitlines():
        dev, typ = (line.split(":", 1) + [""])[:2]
        if typ == "wifi":
            return dev
    raise RuntimeError("No Wi-Fi interface found via nmcli")


def scan_yeelight_aps(iface: str) -> list[dict]:
    run(["sudo", "-n", "nmcli", "dev", "wifi", "rescan", "ifname", iface], check=False)
    time.sleep(2.5)
    r = run(["sudo", "-n", "nmcli", "-t", "-f", "SSID,BSSID,SIGNAL,SECURITY", "dev", "wifi", "list", "ifname", iface])
    aps = []
    seen = set()
    for line in r.stdout.splitlines():
        # BSSID has escaped colons '\:'
        cleaned = line.replace("\\:", "#")
        parts = cleaned.split(":")
        if len(parts) < 4:
            continue
        ssid = parts[0].strip()
        bssid = parts[1].replace("#", ":").strip()
        signal = parts[2].strip()
        sec = parts[3].strip()
        if re.match(r"^(yeelink-light-|miap)", ssid, re.I) and ssid not in seen:
            seen.add(ssid)
            aps.append({"ssid": ssid, "bssid": bssid, "signal": signal, "security": sec})
    return aps


def get_iface_gateway(iface: str) -> str | None:
    r = run(["ip", "route", "show", "dev", iface], check=False)
    for line in r.stdout.splitlines():
        m = re.search(r"default via (\d+\.\d+\.\d+\.\d+)", line)
        if m:
            return m.group(1)
    r2 = run(["ip", "-4", "addr", "show", "dev", iface], check=False)
    m2 = re.search(r"inet (\d+\.\d+\.\d+)\.\d+/", r2.stdout)
    if m2:
        return f"{m2.group(1)}.1"
    return None


def scan_lan_for_did(target_did: str, subnet: str = "192.168.1", timeout: float = 0.7) -> str | None:
    import concurrent.futures

    def probe_one(i: int) -> str | None:
        ip = f"{subnet}.{i}"
        h = ys.miio_hello(ip, timeout=timeout)
        if h and h["did_hex"].lower() == target_did.lower():
            return ip
        return None

    with concurrent.futures.ThreadPoolExecutor(max_workers=64) as ex:
        for res in ex.map(probe_one, range(1, 255)):
            if res:
                return res
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description="Autonomous Yeelight provisioner on Jarvis")
    ap.add_argument("--scan-only", action="store_true", help="Only scan for Yeelight APs")
    ap.add_argument("--ssid", help="Target home 2.4GHz SSID (defaults to ~/.config/yeelight-prov/.env)")
    ap.add_argument("--password", help="Target home 2.4GHz password")
    args = ap.parse_args()

    iface = detect_wifi_iface()
    print(f"Using Wi-Fi interface: {iface}")
    aps = scan_yeelight_aps(iface)
    if not aps:
        print("No Yeelight/miap setup APs found in range.")
        return 1 if not args.scan_only else 0

    print("Found setup APs:", aps)
    if args.scan_only:
        return 0

    env_defaults = ys.load_env_defaults()
    home_ssid = (args.ssid or env_defaults.get("YEELIGHT_WIFI_SSID") or "").strip()
    home_pw = args.password if args.password is not None else env_defaults.get("YEELIGHT_WIFI_PASSWORD", "")
    if not home_ssid or not home_pw:
        print("Missing target Wi-Fi credentials (--ssid/--password or ~/.config/yeelight-prov/.env)")
        return 1

    ap_ssid = aps[0]["ssid"]
    con_name = "yeelight-ap-tmp"
    run(["sudo", "-n", "nmcli", "con", "delete", con_name], check=False)
    try:
        print(f"Connecting {iface} to {ap_ssid} (ipv4.never-default=yes)...")
        run([
            "sudo", "-n", "nmcli", "con", "add",
            "type", "wifi", "ifname", iface, "con-name", con_name,
            "ssid", ap_ssid,
            "ipv4.method", "auto", "ipv4.never-default", "yes",
            "ipv6.method", "ignore",
        ])
        run(["sudo", "-n", "nmcli", "con", "up", con_name])
        time.sleep(2.5)

        gw = get_iface_gateway(iface) or "192.168.10.1"
        print(f"AP gateway detected: {gw}")
        info = ys.miio_hello(gw, timeout=2.5)
        if not info or not info["token_readable"]:
            print(f"Failed to read cleartext AP token at {gw}: {info}")
            return 2

        did = info["did_hex"]
        token = info["token"]
        print(f"Captured AP token={token} did={did}")
        try:
            dev_info = ys.miio_send(gw, token, "miIO.info", []).get("result", {})
            print(f"Model={dev_info.get('model')} FW={dev_info.get('fw_ver')} MAC={dev_info.get('mac')}")
        except Exception as exc:
            print("miIO.info warning:", exc)

        for attempt in range(1, 4):
            try:
                res_ps = ys.miio_send(gw, token, "set_ps", ["cfg_lan_ctrl", "1"], req_id=10 + attempt, timeout=2.5)
                print(f"set_ps ['cfg_lan_ctrl', '1'] (try {attempt}):", res_ps)
                if isinstance(res_ps, dict) and res_ps.get("result") == ["ok"]:
                    break
            except Exception as exc:
                print(f"set_ps try {attempt} err:", exc)

        try:
            print("get_prop ['lan_ctrl']:", ys.miio_send(gw, token, "get_prop", ["lan_ctrl"], req_id=20, timeout=2.5))
        except Exception as exc:
            print("get_prop err:", exc)

        res_cfg = ys.miio_send(gw, token, "miIO.config_router", {
            "ssid": home_ssid,
            "passwd": home_pw,
            "uid": 0,
            "tz": "Europe/Berlin",
        }, req_id=21)
        print("miIO.config_router:", res_cfg)
    finally:
        run(["sudo", "-n", "nmcli", "con", "down", con_name], check=False)
        run(["sudo", "-n", "nmcli", "con", "delete", con_name], check=False)

    print(f"Waiting for did={did} to join home LAN...")
    lan_ip = None
    for _ in range(8):
        time.sleep(2.5)
        lan_ip = scan_lan_for_did(did)
        if lan_ip:
            break
    if not lan_ip:
        print(f"did={did} did not appear in 192.168.1.0/24 within 20s.")
        return 3

    print(f"Bulb online in home LAN at {lan_ip}!")
    print("TCP 55443 test:", ys.check_tcp_55443(lan_ip))
    return 0


if __name__ == "__main__":
    sys.exit(main())
