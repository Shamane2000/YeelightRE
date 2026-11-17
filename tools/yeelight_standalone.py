#!/usr/bin/env python3
"""Zero-dependency Yeelight / miIO Wi-Fi provisioner & LAN Control enabler.

Uses only the Python 3 standard library (includes compact AES-128-CBC).
Works on any Linux host without pip/venv/python-miio.
"""

import argparse
import getpass
import hashlib
import json
import os
import socket
import struct
import sys
from pathlib import Path


def load_env_defaults() -> dict[str, str]:
    """Load optional local defaults from ~/.config/yeelight-prov/.env."""
    env_path = Path.home() / ".config" / "yeelight-prov" / ".env"
    data: dict[str, str] = {}
    if not env_path.is_file():
        return data
    for line in env_path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        data[k.strip()] = v.strip().strip('"').strip("'")
    return data

# --- Minimal pure-Python AES-128-CBC (zero external dependencies) ---
_SBOX = (
    0x63,0x7C,0x77,0x7B,0xF2,0x6B,0x6F,0xC5,0x30,0x01,0x67,0x2B,0xFE,0xD7,0xAB,0x76,
    0xCA,0x82,0xC9,0x7D,0xFA,0x59,0x47,0xF0,0xAD,0xD4,0xA2,0xAF,0x9C,0xA4,0x72,0xC0,
    0xB7,0xFD,0x93,0x26,0x36,0x3F,0xF7,0xCC,0x34,0xA5,0xE5,0xF1,0x71,0xD8,0x31,0x15,
    0x04,0xC7,0x23,0xC3,0x18,0x96,0x05,0x9A,0x07,0x12,0x80,0xE2,0xEB,0x27,0xB2,0x75,
    0x09,0x83,0x2C,0x1A,0x1B,0x6E,0x5A,0xA0,0x52,0x3B,0xD6,0xB3,0x29,0xE3,0x2F,0x84,
    0x53,0xD1,0x00,0xED,0x20,0xFC,0xB1,0x5B,0x6A,0xCB,0xBE,0x39,0x4A,0x4C,0x58,0xCF,
    0xD0,0xEF,0xAA,0xFB,0x43,0x4D,0x33,0x85,0x45,0xF9,0x02,0x7F,0x50,0x3C,0x9F,0xA8,
    0x51,0xA3,0x40,0x8F,0x92,0x9D,0x38,0xF5,0xBC,0xB6,0xDA,0x21,0x10,0xFF,0xF3,0xD2,
    0xCD,0x0C,0x13,0xEC,0x5F,0x97,0x44,0x17,0xC4,0xA7,0x7E,0x3D,0x64,0x5D,0x19,0x73,
    0x60,0x81,0x4F,0xDC,0x22,0x2A,0x90,0x88,0x46,0xEE,0xB8,0x14,0xDE,0x5E,0x0B,0xDB,
    0xE0,0x32,0x3A,0x0A,0x49,0x06,0x24,0x5C,0xC2,0xD3,0xAC,0x62,0x91,0x95,0xE4,0x79,
    0xE7,0xC8,0x37,0x6D,0x8D,0xD5,0x4E,0xA9,0x6C,0x56,0xF4,0xEA,0x65,0x7A,0xAE,0x08,
    0xBA,0x78,0x25,0x2E,0x1C,0xA6,0xB4,0xC6,0xE8,0xDD,0x74,0x1F,0x4B,0xBD,0x8B,0x8A,
    0x70,0x3E,0xB5,0x66,0x48,0x03,0xF6,0x0E,0x61,0x35,0x57,0xB9,0x86,0xC1,0x1D,0x9E,
    0xE1,0xF8,0x98,0x11,0x69,0xD9,0x8E,0x94,0x9B,0x1E,0x87,0xE9,0xCE,0x55,0x28,0xDF,
    0x8C,0xA1,0x89,0x0D,0xBF,0xE6,0x42,0x68,0x41,0x99,0x2D,0x0F,0xB0,0x54,0xBB,0x16,
)
_INV_SBOX = tuple(_SBOX.index(i) for i in range(256))
_RCON = (0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36)


def _xtime(a: int) -> int:
    return ((a << 1) ^ 0x1B) & 0xFF if (a & 0x80) else (a << 1)


def _mul(a: int, b: int) -> int:
    res = 0
    for _ in range(8):
        if b & 1:
            res ^= a
        a = _xtime(a)
        b >>= 1
    return res


def _key_expansion(key: bytes) -> list[list[int]]:
    w = [list(key[i:i + 4]) for i in range(0, 16, 4)]
    for i in range(4, 44):
        temp = list(w[i - 1])
        if i % 4 == 0:
            temp = [_SBOX[temp[1]] ^ _RCON[i // 4 - 1], _SBOX[temp[2]], _SBOX[temp[3]], _SBOX[temp[0]]]
        w.append([w[i - 4][j] ^ temp[j] for j in range(4)])
    return [sum(w[4 * r:4 * r + 4], []) for r in range(11)]


def _aes_encrypt_block(block: bytes, rk: list[list[int]]) -> bytes:
    s = [block[i] ^ rk[0][i] for i in range(16)]
    for r in range(1, 11):
        s = [_SBOX[b] for b in s]
        s = [
            s[0], s[5], s[10], s[15],
            s[4], s[9], s[14], s[3],
            s[8], s[13], s[2], s[7],
            s[12], s[1], s[6], s[11],
        ]
        if r < 10:
            ns = []
            for c in range(0, 16, 4):
                a0, a1, a2, a3 = s[c], s[c + 1], s[c + 2], s[c + 3]
                ns.extend([
                    _mul(2, a0) ^ _mul(3, a1) ^ a2 ^ a3,
                    a0 ^ _mul(2, a1) ^ _mul(3, a2) ^ a3,
                    a0 ^ a1 ^ _mul(2, a2) ^ _mul(3, a3),
                    _mul(3, a0) ^ a1 ^ a2 ^ _mul(2, a3),
                ])
            s = ns
        s = [s[i] ^ rk[r][i] for i in range(16)]
    return bytes(s)


def _aes_decrypt_block(block: bytes, rk: list[list[int]]) -> bytes:
    s = [block[i] ^ rk[10][i] for i in range(16)]
    for r in range(9, -1, -1):
        s = [
            s[0], s[13], s[10], s[7],
            s[4], s[1], s[14], s[11],
            s[8], s[5], s[2], s[15],
            s[12], s[9], s[6], s[3],
        ]
        s = [_INV_SBOX[b] for b in s]
        s = [s[i] ^ rk[r][i] for i in range(16)]
        if r > 0:
            ns = []
            for c in range(0, 16, 4):
                a0, a1, a2, a3 = s[c], s[c + 1], s[c + 2], s[c + 3]
                ns.extend([
                    _mul(14, a0) ^ _mul(11, a1) ^ _mul(13, a2) ^ _mul(9, a3),
                    _mul(9, a0) ^ _mul(14, a1) ^ _mul(11, a2) ^ _mul(13, a3),
                    _mul(13, a0) ^ _mul(9, a1) ^ _mul(14, a2) ^ _mul(11, a3),
                    _mul(11, a0) ^ _mul(13, a1) ^ _mul(9, a2) ^ _mul(14, a3),
                ])
            s = ns
    return bytes(s)


def aes_cbc_encrypt(data: bytes, key: bytes, iv: bytes) -> bytes:
    pad = 16 - (len(data) % 16)
    data = data + bytes([pad] * pad)
    rk = _key_expansion(key)
    prev = iv
    out = bytearray()
    for i in range(0, len(data), 16):
        xored = bytes(a ^ b for a, b in zip(data[i:i + 16], prev))
        enc = _aes_encrypt_block(xored, rk)
        out.extend(enc)
        prev = enc
    return bytes(out)


def aes_cbc_decrypt(data: bytes, key: bytes, iv: bytes) -> bytes:
    rk = _key_expansion(key)
    prev = iv
    out = bytearray()
    for i in range(0, len(data), 16):
        blk = data[i:i + 16]
        dec = _aes_decrypt_block(blk, rk)
        out.extend(bytes(a ^ b for a, b in zip(dec, prev)))
        prev = blk
    pad = out[-1]
    if 1 <= pad <= 16 and out.endswith(bytes([pad] * pad)):
        return bytes(out[:-pad])
    return bytes(out)


# --- miIO protocol ---
def miio_hello(ip: str, timeout: float = 2.0) -> dict | None:
    hello = bytes.fromhex("21310020ffffffffffffffffffffffffffffffffffffffffffffffffffffffff")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    if ip.endswith(".255") or ip == "255.255.255.255":
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    try:
        sock.sendto(hello, (ip, 54321))
        data, addr = sock.recvfrom(1024)
    except OSError:
        return None
    finally:
        sock.close()

    if len(data) < 32:
        return None
    did = data[8:12]
    stamp = struct.unpack(">I", data[12:16])[0]
    token_hex = data[16:32].hex()
    return {
        "ip": addr[0],
        "did": did,
        "did_hex": did.hex(),
        "stamp": stamp,
        "token": token_hex,
        "token_readable": token_hex not in ("0" * 32, "f" * 32),
    }


def miio_send(ip: str, token_hex: str, method: str, params=None, req_id: int = 1, timeout: float = 3.0) -> dict:
    hello = miio_hello(ip, timeout=timeout)
    if not hello:
        raise RuntimeError(f"No miIO hello response from {ip}:54321")

    token = bytes.fromhex(token_hex)
    key = hashlib.md5(token).digest()
    iv = hashlib.md5(key + token).digest()

    payload_obj = {"id": req_id, "method": method}
    if params is not None:
        payload_obj["params"] = params
    payload = json.dumps(payload_obj, separators=(",", ":")).encode("utf-8")
    encrypted = aes_cbc_encrypt(payload, key, iv)

    total_len = 32 + len(encrypted)
    hdr16 = struct.pack(">HHII", 0x2131, total_len, 0, int.from_bytes(hello["did"], "big"))
    # Re-pack properly with 4-byte unknown=0, 4-byte did, 4-byte stamp+1
    hdr16 = b"\x21\x31" + struct.pack(">HI", total_len, 0) + hello["did"] + struct.pack(">I", hello["stamp"] + 1)
    md5_chk = hashlib.md5(hdr16 + token + encrypted).digest()
    packet = hdr16 + md5_chk + encrypted

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    try:
        sock.sendto(packet, (ip, 54321))
        resp, _ = sock.recvfrom(4096)
    finally:
        sock.close()

    if len(resp) <= 32:
        return {"raw_len": len(resp)}
    decrypted = aes_cbc_decrypt(resp[32:], key, iv)
    return json.loads(decrypted.rstrip(b"\x00").decode("utf-8"))


def detect_wifi_gateways() -> list[str]:
    candidates: list[str] = []
    try:
        with open("/proc/net/route", encoding="utf-8") as f:
            for line in f.readlines()[1:]:
                parts = line.split()
                if len(parts) < 8 or not parts[0].startswith("wl"):
                    continue
                dest = socket.inet_ntoa(struct.pack("<L", int(parts[1], 16)))
                gw = socket.inet_ntoa(struct.pack("<L", int(parts[2], 16)))
                if gw != "0.0.0.0" and gw not in candidates:
                    candidates.append(gw)
                elif dest != "0.0.0.0":
                    guess = ".".join(dest.split(".")[:3]) + ".1"
                    if guess not in candidates:
                        candidates.append(guess)
    except OSError:
        pass
    for fallback in ("192.168.10.1", "192.168.4.1", "192.168.123.1"):
        if fallback not in candidates:
            candidates.append(fallback)
    return candidates


def check_tcp_55443(ip: str, timeout: float = 2.0) -> str:
    try:
        with socket.create_connection((ip, 55443), timeout=timeout) as s:
            s.settimeout(timeout)
            cmd = json.dumps({"id": 1, "method": "get_prop", "params": ["power", "bright", "ct", "color_mode"]}) + "\r\n"
            s.sendall(cmd.encode())
            return s.recv(1024).decode(errors="replace").strip()
    except OSError as exc:
        return f"ERR: {exc}"


def cmd_probe(args: argparse.Namespace) -> int:
    targets = [args.ip] if args.ip else [*detect_wifi_gateways(), "255.255.255.255"]
    found = None
    for target in targets:
        info = miio_hello(target)
        if info and info["token_readable"]:
            found = info
            break
        if info and found is None:
            found = info
    if not found:
        print("No miIO device answered on UDP 54321.")
        return 1
    print(f"Found device at {found['ip']} (did={found['did_hex']})")
    print(f"Token readable: {found['token_readable']} | token={found['token']}")
    if found["token_readable"]:
        try:
            print("miIO.info:", json.dumps(miio_send(found["ip"], found["token"], "miIO.info", []), indent=2))
        except Exception as exc:
            print(f"miIO.info failed: {exc}")
    return 0


def cmd_provision(args: argparse.Namespace) -> int:
    ap_ip = args.ip
    if not ap_ip:
        for cand in detect_wifi_gateways():
            res = miio_hello(cand)
            if res and res["token_readable"]:
                ap_ip = cand
                break
    if not ap_ip:
        print("Could not auto-detect Yeelight AP with readable token. Pass --ip explicitly.")
        return 1

    info = miio_hello(ap_ip)
    if not info or not info["token_readable"]:
        print(f"No readable token at {ap_ip}:54321")
        return 1

    token = info["token"]
    print(f"Captured AP token: {token} (did={info['did_hex']})")
    try:
        dev_info = miio_send(ap_ip, token, "miIO.info", []).get("result", {})
        print(f"Model={dev_info.get('model')} FW={dev_info.get('fw_ver')} MAC={dev_info.get('mac')}")
    except Exception as exc:
        print(f"Warning: miIO.info failed ({exc}), continuing...")

    env_defaults = load_env_defaults()
    ssid = (args.ssid or env_defaults.get("YEELIGHT_WIFI_SSID") or input("2.4GHz Wi-Fi SSID: ")).strip()
    if not ssid:
        print("SSID must not be empty.")
        return 1
    if args.password is not None:
        password = args.password
    elif env_defaults.get("YEELIGHT_WIFI_PASSWORD"):
        password = env_defaults["YEELIGHT_WIFI_PASSWORD"]
    else:
        password = getpass.getpass(f"Wi-Fi password for '{ssid}': ")

    # Enable LAN Control BEFORE switching from AP to home Wi-Fi:
    # 'set_developer_mode' is only a python-miio wrapper; the real device RPC is set_ps ["cfg_lan_ctrl", "1"].
    for attempt in range(1, 4):
        try:
            lan_res = miio_send(ap_ip, token, "set_ps", ["cfg_lan_ctrl", "1"], req_id=1 + attempt, timeout=2.5)
            print(f"AP set_ps ['cfg_lan_ctrl', '1'] (try {attempt}) response:", lan_res)
            if isinstance(lan_res, dict) and lan_res.get("result") == ["ok"]:
                break
        except Exception as exc:
            print(f"Warning: AP set_ps try {attempt} failed ({exc})")
    try:
        prop_res = miio_send(ap_ip, token, "get_prop", ["lan_ctrl"], req_id=10, timeout=2.5)
        print("AP get_prop ['lan_ctrl'] readback:", prop_res)
    except Exception as exc:
        print(f"Warning: AP get_prop ['lan_ctrl'] failed ({exc})")

    params = {"ssid": ssid, "passwd": password, "uid": args.uid}
    if args.tz:
        params["tz"] = args.tz
    if args.region:
        params["country_domain"] = args.region
    res = miio_send(ap_ip, token, "miIO.config_router", params, req_id=11)
    print("miIO.config_router response:", res)
    print("\nOnce connected to your home Wi-Fi, Home Assistant should auto-discover it via SSDP on port 55443.")
    print(f"If LAN Control is still off and internet was blocked, fallback:\n  python3 {sys.argv[0]} enable-lan --ip <NEW_LAN_IP> --token {token}")
    return 0


def cmd_enable_lan(args: argparse.Namespace) -> int:
    print(f"Sending set_ps ['cfg_lan_ctrl', '1'] to {args.ip}...")
    res = miio_send(args.ip, args.token, "set_ps", ["cfg_lan_ctrl", "1"], req_id=1)
    print("set_ps response:", res)
    try:
        print("get_prop ['lan_ctrl']:", miio_send(args.ip, args.token, "get_prop", ["lan_ctrl"], req_id=2))
    except Exception as exc:
        print(f"get_prop warning: {exc}")
    print("Testing TCP 55443:", check_tcp_55443(args.ip))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Zero-dependency Yeelight/miIO provisioner")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_probe = sub.add_parser("probe", help="Probe AP/LAN device for cleartext miIO token")
    p_probe.add_argument("--ip", help="Target IP (auto-detected if omitted)")
    p_probe.set_defaults(func=cmd_probe)

    p_prov = sub.add_parser("provision", help="Push Wi-Fi credentials via miIO.config_router")
    p_prov.add_argument("--ip", default=None, help="Bulb AP IP (auto-detected if omitted)")
    p_prov.add_argument("--ssid", default="", help="Target 2.4GHz Wi-Fi SSID (prompted if omitted)")
    p_prov.add_argument("--password", help="Wi-Fi password (prompted if omitted)")
    p_prov.add_argument("--uid", type=int, default=0, help="miIO uid (default: 0)")
    p_prov.add_argument("--tz", default="Europe/Berlin", help="Timezone for miIO.config_router (default: Europe/Berlin)")
    p_prov.add_argument("--region", default="", help="Optional country_domain (e.g. de, cn)")
    p_prov.set_defaults(func=cmd_provision)

    p_lan = sub.add_parser("enable-lan", help="Enable Yeelight LAN Control (port 55443)")
    p_lan.add_argument("--ip", required=True, help="Bulb IP in home LAN")
    p_lan.add_argument("--token", required=True, help="32-char miIO token")
    p_lan.set_defaults(func=cmd_enable_lan)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
