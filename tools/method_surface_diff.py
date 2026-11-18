#!/usr/bin/env python3
"""Compare the LAN-RPC method surface of two Yeelight devices (read-only).

Sends only get_* methods; no state-changing call is issued.
Usage: python3 tools/method_surface_diff.py <ipA> <ipB>
"""
import json
import socket
import sys

PROPS = ["fw_ver", "model", "lan_ctrl", "power", "name", "bright", "color_mode",
         "ct", "rgb", "hue", "sat", "delayoff", "flowing", "music_on", "nl_br",
         "active_mode", "main_power", "bg_power"]
READS = [
    ("get_prop", PROPS),
    ("get_ps", ["cfg_lan_ctrl"]),
    ("get_aging_status", []),
    ("miio_test", []),
    ("get_music_info", []),
    ("get_cron", []),
    ("get_scene", []),
    ("get_adjust", ["ct"]),
]


def call_all(ip, port=55443, timeout=6):
    s = socket.create_connection((ip, port), timeout=timeout)
    s.settimeout(timeout)
    buf = b""
    res = {}
    try:
        for mid, (m, p) in enumerate(READS, start=1):
            s.sendall((json.dumps({"id": mid, "method": m, "params": p}) + "\r\n").encode())
            while b"\r\n" not in buf:
                chunk = s.recv(4096)
                if not chunk:
                    raise RuntimeError("connection closed")
                buf += chunk
            line, buf = buf.split(b"\r\n", 1)
            res[m] = json.loads(line.decode())
    finally:
        s.close()
    return res


def main(a, b):
    ra, rb = call_all(a), call_all(b)
    print(f"A = {a}   B = {b}")
    print(f"{'method':<20} {'A result':<58} {'B result'}")
    for m, _ in READS:
        sa = json.dumps(ra[m])
        sb = json.dumps(rb[m])
        if len(sa) > 56:
            sa = sa[:53] + "..."
        if len(sb) > 56:
            sb = sb[:53] + "..."
        flag = "same" if sa == sb else "DIFF"
        print(f"{m:<20} {sa:<58} {sb}   [{flag}]")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
