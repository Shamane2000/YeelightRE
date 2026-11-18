#!/usr/bin/env python3
"""OTA Flash Tool for Yeelight 88MW300/302 via miIO.ota (UDP 54321).

Uses the proven AES-128-CBC miIO implementation from yeelight_standalone.py.
Safety: sequence="a" writes ONLY to App partition (ID 1, base 0x10000+).
        boot2 (0x0-0xFFFF) is NEVER touched (see docs/11_phase5_flash_rollback.md).

Usage:
    python3 ota_flash.py --host 192.168.1.115 --token <TOKEN> \
        --app-url http://192.168.1.x:8080/mcufw.bin \
        --md5 <MD5> [--dry-run] [--sequence a] [--no-reboot]
"""
import argparse
import hashlib
import json
import socket
import struct
import sys
import time
import urllib.request

# --- Minimal pure-Python AES-128-CBC (from yeelight_standalone.py) ---
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

def _xtime(a: int) -> int:
    return ((a << 1) ^ 0x1B) & 0xFF if a & 0x80 else (a << 1) & 0xFF

def _mul(a: int, b: int) -> int:
    r = 0
    while b:
        if b & 1: r ^= a
        a = _xtime(a)
        b >>= 1
    return r

def _key_expansion(key: bytes):
    Nk, Nr = 4, 10
    w = [list(key[i*4:(i+1)*4]) for i in range(Nk)]
    rcon = 1
    for i in range(Nk, 4*(Nr+1)):
        t = list(w[i-1])
        if i % Nk == 0:
            t = t[1:] + t[:1]
            t = [_SBOX[b] for b in t]
            t[0] ^= rcon
            rcon = _xtime(rcon)
        w.append([w[i-Nk][j] ^ t[j] for j in range(4)])
    return [w[i*4:(i+1)*4] for i in range(Nr+1)]

def _aes_encrypt_block(block: bytes, rk) -> bytes:
    state = [list(block[i:i+4]) for i in range(0, 16, 4)]
    state = [[state[c][r] for c in range(4)] for r in range(4)]
    state = [[state[r][c] ^ rk[0][r][c] for c in range(4)] for r in range(4)]
    for rnd in range(1, 10):
        state = [[_SBOX[state[r][c]] for c in range(4)] for r in range(4)]
        state[1] = state[1][1:] + state[1][:1]
        state[2] = state[2][2:] + state[2][:2]
        state[3] = state[3][3:] + state[3][:3]
        new = [[0]*4 for _ in range(4)]
        for r in range(4):
            row = state[r]
            new[r][0] = _mul(row[0],2)^_mul(row[1],3)^row[2]^row[3]
            new[r][1] = row[0]^_mul(row[1],2)^_mul(row[2],3)^row[3]
            new[r][2] = row[0]^row[1]^_mul(row[2],2)^_mul(row[3],3)
            new[r][3] = _mul(row[0],3)^row[1]^row[2]^_mul(row[3],2)
        state = [[new[r][c] ^ rk[rnd][r][c] for c in range(4)] for r in range(4)]
    state = [[_SBOX[state[r][c]] for c in range(4)] for r in range(4)]
    state[1] = state[1][1:] + state[1][:1]
    state[2] = state[2][2:] + state[2][:2]
    state[3] = state[3][3:] + state[3][:3]
    state = [[state[r][c] ^ rk[10][r][c] for c in range(4)] for r in range(4)]
    out = bytearray(16)
    for r in range(4):
        for c in range(4): out[c*4+r] = state[r][c]
    return bytes(out)

def _aes_decrypt_block(block: bytes, rk) -> bytes:
    state = [list(block[i:i+4]) for i in range(0, 16, 4)]
    state = [[state[c][r] for c in range(4)] for r in range(4)]
    state = [[state[r][c] ^ rk[10][r][c] for c in range(4)] for r in range(4)]
    for rnd in range(9, 0, -1):
        state[3] = state[3][-1:] + state[3][:-1]
        state[2] = state[2][-2:] + state[2][:-2]
        state[1] = state[1][-3:] + state[1][:-3]
        state = [[_INV_SBOX[state[r][c]] for c in range(4)] for r in range(4)]
        new = [[0]*4 for _ in range(4)]
        for r in range(4):
            row = state[r]
            new[r][0] = _mul(row[0],0xe)^_mul(row[1],0xb)^_mul(row[2],0xd)^_mul(row[3],0x9)
            new[r][1] = _mul(row[0],0x9)^_mul(row[1],0xe)^_mul(row[2],0xb)^_mul(row[3],0xd)
            new[r][2] = _mul(row[0],0xd)^_mul(row[1],0x9)^_mul(row[2],0xe)^_mul(row[3],0xb)
            new[r][3] = _mul(row[0],0xb)^_mul(row[1],0xd)^_mul(row[2],0x9)^_mul(row[3],0xe)
        state = [[new[r][c] ^ rk[rnd][r][c] for c in range(4)] for r in range(4)]
    state[3] = state[3][-1:] + state[3][:-1]
    state[2] = state[2][-2:] + state[2][:-2]
    state[1] = state[1][-3:] + state[1][:-3]
    state = [[_INV_SBOX[state[r][c]] for c in range(4)] for r in range(4)]
    state = [[state[r][c] ^ rk[0][r][c] for c in range(4)] for r in range(4)]
    out = bytearray(16)
    for r in range(4):
        for c in range(4): out[c*4+r] = state[r][c]
    return bytes(out)

def aes_cbc_encrypt(data: bytes, key: bytes, iv: bytes) -> bytes:
    rk = _key_expansion(key)
    pad_len = 16 - len(data) % 16
    data += bytes([pad_len]) * pad_len
    out = bytearray()
    prev = iv
    for i in range(0, len(data), 16):
        blk = bytes(a ^ b for a, b in zip(data[i:i+16], prev))
        enc = _aes_encrypt_block(blk, rk)
        out.extend(enc)
        prev = enc
    return bytes(out)

def aes_cbc_decrypt(data: bytes, key: bytes, iv: bytes) -> bytes:
    rk = _key_expansion(key)
    out = bytearray()
    prev = iv
    for i in range(0, len(data), 16):
        dec = _aes_decrypt_block(data[i:i+16], rk)
        out.extend(bytes(a ^ b for a, b in zip(dec, prev)))
        prev = data[i:i+16]
    pad = out[-1]
    if 1 <= pad <= 16 and all(b == pad for b in out[-pad:]):
        out = out[:-pad]
    return bytes(out)

# --- miIO protocol ---
def miio_hello(ip: str, timeout: float = 2.0):
    hello = bytes.fromhex("21310020ffffffffffffffffffffffffffffffffffffffffffffffffffffffff")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    try:
        sock.sendto(hello, (ip, 54321))
        data, addr = sock.recvfrom(1024)
    except OSError:
        return None
    finally:
        sock.close()
    if len(data) < 32: return None
    did = data[8:12]
    stamp = struct.unpack(">I", data[12:16])[0]
    return {"did": did, "stamp": stamp}

def miio_send(ip: str, token_hex: str, method: str, params=None, req_id: int = 1, timeout: float = 3.0) -> dict:
    hello = miio_hello(ip, timeout=timeout)
    if not hello:
        raise RuntimeError(f"No miIO hello from {ip}:54321")
    token = bytes.fromhex(token_hex)
    key = hashlib.md5(token).digest()
    iv = hashlib.md5(key + token).digest()
    payload_obj = {"id": req_id, "method": method}
    if params is not None: payload_obj["params"] = params
    payload = json.dumps(payload_obj, separators=(",", ":")).encode("utf-8")
    encrypted = aes_cbc_encrypt(payload, key, iv)
    total_len = 32 + len(encrypted)
    hdr = b"\x21\x31" + struct.pack(">HI", total_len, 0) + hello["did"] + struct.pack(">I", hello["stamp"] + 1)
    md5_chk = hashlib.md5(hdr + token + encrypted).digest()
    packet = hdr + md5_chk + encrypted
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    try:
        sock.sendto(packet, (ip, 54321))
        resp, _ = sock.recvfrom(4096)
    finally:
        sock.close()
    if len(resp) <= 32: return {"raw_len": len(resp)}
    decrypted = aes_cbc_decrypt(resp[32:], key, iv)
    return json.loads(decrypted.rstrip(b"\x00").decode("utf-8"))

# --- OTA Logic ---
def verify_container_md5(url: str, expected_md5: str) -> bool:
    print(f"[*] Downloading {url} for MD5 check...")
    req = urllib.request.urlopen(url, timeout=30)
    data = req.read()
    actual = hashlib.md5(data).hexdigest()
    ok = actual.lower() == expected_md5.lower()
    print(f"    Expected: {expected_md5}")
    print(f"    Actual:   {actual}")
    print(f"    Match:    {'YES' if ok else 'NO'}")
    return ok

def main():
    parser = argparse.ArgumentParser(description="Yeelight OTA Flash via miIO.ota")
    parser.add_argument("--host", required=True)
    parser.add_argument("--token", required=True)
    parser.add_argument("--app-url", required=True)
    parser.add_argument("--md5", required=True)
    parser.add_argument("--sequence", default="a")
    parser.add_argument("--no-reboot", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    if len(bytes.fromhex(args.token)) != 16:
        print("ERROR: Token must be 32 hex chars"); sys.exit(1)

    if not verify_container_md5(args.app_url, args.md5):
        print("ERROR: MD5 mismatch"); sys.exit(2)

    print(f"\n[*] Testing miIO connectivity to {args.host}...")
    try:
        info = miio_send(args.host, args.token, "miIO.info", {}, req_id=999)
        print(f"    Response: {json.dumps(info)}")
    except Exception as e:
        print(f"ERROR: Connectivity failed: {e}"); sys.exit(3)

    if args.dry_run:
        print("\n[OK] Dry-run complete."); sys.exit(0)

    params = {"app_url": args.app_url, "file_md5": args.md5, "sequence": args.sequence}
    if args.no_reboot: params["no_reboot"] = True

    print(f"\n[*] Sending miIO.ota...")
    for attempt in range(1, 4):
        try:
            resp = miio_send(args.host, args.token, "miIO.ota", params, req_id=1)
            print(f"    Attempt {attempt}: {json.dumps(resp)}")
            if "result" in resp or resp.get("code") == 0:
                print("\n[OK] OTA accepted."); break
            time.sleep(2)
        except Exception as e:
            print(f"    Attempt {attempt} failed: {e}")
            if attempt == 3: print("ERROR: Retries exhausted."); sys.exit(4)
            time.sleep(2)

    print("\n[*] Monitoring progress...")
    for _ in range(60):
        time.sleep(5)
        try:
            state = miio_send(args.host, args.token, "miIO.get_ota_state", {}, req_id=2)
            s = state.get("result", [None])[0] if isinstance(state.get("result"), list) else state.get("result")
            print(f"    State: {s}")
            if s in ("done", "success", 2): print("\n[OK] OTA done!"); sys.exit(0)
            if s in ("failed", "error", -1): print(f"\nERROR: OTA failed"); sys.exit(5)
        except socket.timeout: print("    (timeout, device may have restarted)"); break
        except Exception as e: print(f"    Poll error: {e}")

if __name__ == "__main__":
    main()
