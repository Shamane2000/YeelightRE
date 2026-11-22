#!/usr/bin/env python3
"""Autarkic cloud-free Yeelight fleet classifier, OTA server/flasher & mcufw provisioner.

Pure Python 3 stdlib (zero pip dependencies). Supports:
  - Marvell 88MW300/302 (color1/color2) offline local UDP 54321 miIO.ota (fw < 2.0.6)
  - Hard safety blocking against non-Marvell SoCs (e.g. color4 ESP32/Realtek)
  - Hard safety blocking / board-profile gating for non-color2 Marvell models
  - Mandatory HTTP/1.1 404 dry-run before any live OTA write
  - MRVL container verification (segment CRCs, bounds <= 384 KB)
  - Zero-app HTTP provisioning of mcufw >= 0.8-phase4b.4 (/api/status + /api/config)
"""
from __future__ import annotations

import argparse
import contextlib
import hashlib
import http.client
import io
import json
import os
import re
import socket
import struct
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

MIIO_PORT = 54321
MIIO_HELLO = bytes.fromhex("21310020" + "ff" * 28)
EMPTY_TOKENS = {b"\x00" * 16, b"\xff" * 16}

HDR_FMT = "<4sIIII"
HDR_SIZE = struct.calcsize(HDR_FMT)  # 20 bytes
SEG_FMT = "<IIIII"
SEG_SIZE = struct.calcsize(SEG_FMT)  # 20 bytes
MRVL_MAGIC = b"MRVL"
MAGIC2_REFERENCE = 0x2E9CF17B
SEG_TYPE_LOADABLE = 2
MIN_IMAGE_SIZE = 16 * 1024
MAX_APP_SLOT_SIZE = 384 * 1024  # 393216 bytes (0x60000)

MARVELL_MODELS = frozenset({
    "yeelink.light.color2",
    "yeelink.light.color1",
    "yeelink.light.color",
    "yeelink.light.ct2",
    "yeelink.light.mono1",
    "yeelink.light.ceiling1",
})
READY_BOARDS = frozenset({"yeelink.light.color2"})
DEFAULT_FLEET_STORE = Path.home() / ".config" / "yeelight-autark" / "fleet.json"


# ---------------------------------------------------------------------------
# Pure-Python AES-128-CBC + MD5 miIO Wire Codec (fixed 64-bit DID)
# ---------------------------------------------------------------------------

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
    w = [list(key[i : i + 4]) for i in range(0, 16, 4)]
    for i in range(4, 44):
        temp = list(w[i - 1])
        if i % 4 == 0:
            temp = [
                _SBOX[temp[1]] ^ _RCON[i // 4 - 1],
                _SBOX[temp[2]],
                _SBOX[temp[3]],
                _SBOX[temp[0]],
            ]
        w.append([w[i - 4][j] ^ temp[j] for j in range(4)])
    return [sum(w[4 * r : 4 * r + 4], []) for r in range(11)]


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


def _md5(data: bytes) -> bytes:
    return hashlib.md5(data).digest()


def _miio_key_iv(token: bytes) -> tuple[bytes, bytes]:
    key = _md5(token)
    iv = _md5(key + token)
    return key, iv


def aes_cbc_encrypt(data: bytes, token: bytes) -> bytes:
    key, iv = _miio_key_iv(token)
    pad = 16 - (len(data) % 16)
    padded = data + bytes([pad] * pad)
    rk = _key_expansion(key)
    prev = iv
    out = bytearray()
    for i in range(0, len(padded), 16):
        xored = bytes(a ^ b for a, b in zip(padded[i : i + 16], prev))
        enc = _aes_encrypt_block(xored, rk)
        out.extend(enc)
        prev = enc
    return bytes(out)


def aes_cbc_decrypt(data: bytes, token: bytes) -> bytes:
    if len(data) == 0 or len(data) % 16 != 0:
        raise ValueError(f"invalid ciphertext length {len(data)}")
    key, iv = _miio_key_iv(token)
    rk = _key_expansion(key)
    prev = iv
    out = bytearray()
    for i in range(0, len(data), 16):
        blk = data[i : i + 16]
        dec = _aes_decrypt_block(blk, rk)
        out.extend(bytes(a ^ b for a, b in zip(dec, prev)))
        prev = blk
    pad = out[-1]
    if 1 <= pad <= 16 and out.endswith(bytes([pad] * pad)):
        return bytes(out[:-pad]).rstrip(b"\x00")
    raise ValueError("invalid PKCS#7 padding in miIO payload")


class MiioRpcError(RuntimeError):
    """Raised when a bulb answers with a JSON-RPC error object."""

    def __init__(self, code: int | None, message: str) -> None:
        self.code = code
        self.message = message
        super().__init__(f"miIO error {code}: {message}")


def parse_hello(data: bytes) -> dict[str, Any] | None:
    """Validate and decode a 32-byte miIO.hello packet with full 64-bit DID."""
    if len(data) < 32 or data[:2] != b"\x21\x31":
        return None
    did_bytes = data[4:12]
    did_u64 = int.from_bytes(did_bytes, "big")
    did_hex8 = data[8:12].hex()
    stamp = struct.unpack(">I", data[12:16])[0]
    token_bytes = data[16:32]
    token_readable = token_bytes not in EMPTY_TOKENS
    return {
        "did_bytes": did_bytes,
        "did_u64": did_u64,
        "did_hex8": did_hex8,
        "stamp": stamp,
        "token_bytes": token_bytes,
        "token_hex": token_bytes.hex(),
        "token_readable": token_readable,
    }


def miio_hello(ip: str, port: int = MIIO_PORT, timeout: float = 2.0, broadcast: bool = False) -> dict[str, Any] | None:
    """Send miIO.hello to UDP (ip, port) and return decoded packet metadata."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(timeout)
        if broadcast or ip.endswith(".255") or ip == "255.255.255.255":
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        try:
            sock.sendto(MIIO_HELLO, (ip, port))
            data, addr = sock.recvfrom(4096)
        except OSError:
            return None
    parsed = parse_hello(data)
    if parsed is None:
        return None
    parsed["ip"] = addr[0]
    parsed["port"] = addr[1]
    return parsed


class MiioClient:
    """Thread-safe pure-stdlib miIO UDP RPC client using full 64-bit DID."""

    def __init__(self, ip: str, token: bytes | str, port: int = MIIO_PORT, timeout: float = 3.0) -> None:
        self.ip = ip
        self.port = port
        self.timeout = timeout
        if isinstance(token, str):
            clean = token.strip()
            if len(clean) != 32:
                raise ValueError(f"token hex must be 32 chars, got {len(clean)}")
            self.token = bytes.fromhex(clean)
        else:
            if len(token) != 16:
                raise ValueError(f"token bytes must be 16 bytes, got {len(token)}")
            self.token = token
        self._id = 1

    def send(self, method: str, params: Any = None, port: int | None = None) -> Any:
        target_port = self.port if port is None else port
        hello = miio_hello(self.ip, port=target_port, timeout=self.timeout)
        if not hello:
            raise TimeoutError(f"{self.ip}:{target_port}: no miIO.hello reply")

        req_id = self._id
        self._id += 1
        payload_obj: dict[str, Any] = {"id": req_id, "method": method}
        if params is not None:
            payload_obj["params"] = params
        else:
            payload_obj["params"] = []

        plaintext = json.dumps(payload_obj, separators=(",", ":")).encode("utf-8")
        enc = aes_cbc_encrypt(plaintext, self.token)
        header = struct.pack(">HHQI", 0x2131, 32 + len(enc), hello["did_u64"], (hello["stamp"] + 1) & 0xFFFFFFFF)
        digest = _md5(header + self.token + enc)
        packet = header + digest + enc

        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.settimeout(self.timeout)
            sock.sendto(packet, (self.ip, target_port))
            resp, _ = sock.recvfrom(4096)

        if len(resp) < 32 or resp[:2] != b"\x21\x31":
            raise ValueError(f"invalid miIO reply header (len={len(resp)})")
        expected_len = struct.unpack(">H", resp[2:4])[0]
        if expected_len > len(resp):
            raise ValueError(f"truncated miIO reply: header={expected_len} actual={len(resp)}")
        resp = resp[:expected_len]
        body = resp[32:]
        if _md5(resp[:16] + self.token + body) != resp[16:32]:
            raise ValueError("bad miIO reply MD5 checksum (wrong token or tampered packet)")
        if not body:
            return None

        reply = json.loads(aes_cbc_decrypt(body, self.token).decode("utf-8"))
        if "error" in reply and reply["error"] is not None:
            err = reply["error"]
            if isinstance(err, dict):
                raise MiioRpcError(err.get("code"), str(err.get("message", err)))
            raise MiioRpcError(None, str(err))
        return reply.get("result")


# ---------------------------------------------------------------------------
# Fleet Hardware & Firmware Classifier
# ---------------------------------------------------------------------------

def _parse_ver_tuple(ver: str | None) -> tuple[int, ...] | None:
    if not ver or not isinstance(ver, str):
        return None
    nums = [int(x) for x in re.findall(r"\d+", ver)]
    return tuple(nums) if nums else None


def classify_bulb(miio_info: dict[str, Any], allow_board_override: bool = False) -> dict[str, Any]:
    """Classify a Yeelight bulb by SoC, board model, and firmware OTA channel."""
    model = str(miio_info.get("model") or "").strip()
    fw_ver = str(miio_info.get("fw_ver") or "").strip()
    miio_ver = str(miio_info.get("miio_ver") or "").strip()
    hw_ver = str(miio_info.get("hw_ver") or "").strip()
    mac = str(miio_info.get("mac") or "").strip()

    base: dict[str, Any] = {
        "model": model,
        "fw_ver": fw_ver,
        "miio_ver": miio_ver,
        "hw_ver": hw_ver,
        "mac": mac,
    }

    if (not model and not hw_ver) or (hw_ver and hw_ver.lower() not in ("mw300", "mw302")) or (
        model and model not in MARVELL_MODELS
    ):
        return {
            **base,
            "category": "unsupported_soc",
            "flash_allowed": False,
            "ota_channel": "none",
            "reason": "Nicht-Marvell-SoC (z. B. ESP32/Realtek) — mcufw_MRVL.bin gesperrt!",
        }

    fw_tuple = _parse_ver_tuple(fw_ver)
    miio_tuple = _parse_ver_tuple(miio_ver)
    if fw_tuple is not None and fw_tuple < (2, 0, 6, 0) and (miio_tuple is None or miio_tuple < (0, 0, 3)):
        ota_channel = "udp_54321_offline"
    else:
        ota_channel = "firewall_rpc_once"

    if model not in READY_BOARDS and not allow_board_override:
        return {
            **base,
            "category": "needs_board_profile",
            "flash_allowed": False,
            "ota_channel": ota_channel,
            "reason": f"Marvell-SoC erkannt ({model or hw_ver}), aber aktuelles Image ist auf color2 (GPIO 0/1/4/5 + GPIO44) gepinnt.",
        }

    category = "offline_direct" if ota_channel == "udp_54321_offline" else "firewall_rpc_once"
    reason = (
        "100% offline UDP 54321 miIO.ota moeglich"
        if category == "offline_direct"
        else "TAG_KEY-Cloud-Trigger im OPNsense-Firewall-Kaefig noetig (kein App-Zwang)"
    )
    return {
        **base,
        "category": category,
        "flash_allowed": True,
        "ota_channel": ota_channel,
        "reason": reason,
    }


# ---------------------------------------------------------------------------
# MRVL Container Verifier (uses sibling verify_mrvl_container when available)
# ---------------------------------------------------------------------------

def _crc32_mrvl_fallback(data: bytes) -> int:
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xEDB88320 if (crc & 1) else (crc >> 1)
    return crc & 0xFFFFFFFF


def _try_import_sibling_verifier() -> Any | None:
    sib = Path(__file__).resolve().parent / "verify_mrvl_container.py"
    if not sib.is_file():
        return None
    try:
        tools_dir = str(sib.parent)
        if tools_dir not in sys.path:
            sys.path.insert(0, tools_dir)
        import verify_mrvl_container as vmc  # type: ignore

        return vmc
    except Exception:
        return None


def verify_firmware_file(path: Path | str) -> dict[str, Any]:
    """Validate an 88MW300/302 MRVL firmware image and return checksums + geometry."""
    p = Path(path)
    if not p.is_file():
        raise ValueError(f"Firmware-Datei nicht gefunden: {p}")
    data = p.read_bytes()
    size = len(data)
    if size < MIN_IMAGE_SIZE or size > MAX_APP_SLOT_SIZE:
        raise ValueError(
            f"Ungueltige Image-Groesse {size} B (erlaubt: {MIN_IMAGE_SIZE}..{MAX_APP_SLOT_SIZE} B)"
        )

    vmc = _try_import_sibling_verifier()
    if vmc is not None:
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                img = vmc.parse_container(str(p))
                rep = vmc.Report(quiet=True)
                vmc.verify_container(img, rep, MAX_APP_SLOT_SIZE)
        except SystemExit as exc:
            raise ValueError(str(exc)) from exc
        if rep.failed:
            raise ValueError("MRVL-Container-Check fehlgeschlagen: " + "; ".join(rep.failures))
        segs_out = [
            {"index": s["index"], "offset": s["offset"], "size": s["size"], "addr": s["addr"], "crc": s["crc"]}
            for s in img["segs"]
        ]
        entry = img["entry"]
    else:
        if len(data) < HDR_SIZE:
            raise ValueError("Container kuerzer als 20-B-Header")
        magic, magic2, _ts, seg_cnt, entry = struct.unpack_from(HDR_FMT, data, 0)
        if magic != MRVL_MAGIC:
            raise ValueError(f"Kein MRVL-Magic: {magic!r}")
        if magic2 != MAGIC2_REFERENCE:
            raise ValueError(f"Ungueltiges magic2: 0x{magic2:08x} (erwartet 0x{MAGIC2_REFERENCE:08x})")
        if not (1 <= seg_cnt <= 8):
            raise ValueError(f"Ungueltige Segmentanzahl: {seg_cnt}")
        table_end = HDR_SIZE + SEG_SIZE * seg_cnt
        if table_end > size:
            raise ValueError("Segment-Tabelle ueberschreitet Dateigroesse")

        segs_out = []
        prev_end = table_end
        for idx in range(seg_cnt):
            stype, offset, ssize, laddr, crc = struct.unpack_from(SEG_FMT, data, HDR_SIZE + SEG_SIZE * idx)
            if stype != SEG_TYPE_LOADABLE:
                raise ValueError(f"seg{idx+1}: falscher Typ {stype}")
            if ssize == 0 or offset < table_end or offset < prev_end or offset + ssize > size:
                raise ValueError(f"seg{idx+1}: ungueltige Bounds offset=0x{offset:x} size=0x{ssize:x}")
            body = data[offset : offset + ssize]
            calc = _crc32_mrvl_fallback(body)
            if calc != crc:
                raise ValueError(f"seg{idx+1}: CRC-Mismatch stored=0x{crc:08x} calc=0x{calc:08x}")
            segs_out.append({"index": idx + 1, "offset": offset, "size": ssize, "addr": laddr, "crc": crc})
            prev_end = offset + ssize

        if any(b != 0xFF for b in data[table_end : segs_out[0]["offset"]]):
            raise ValueError("Gap zwischen Tabelle und seg1 nicht 0xFF-gefuellt")
        if segs_out[-1]["offset"] + segs_out[-1]["size"] != size:
            raise ValueError("Ueberzaehlige Bytes nach letztem Segment")

    return {
        "path": str(p),
        "size": size,
        "md5": hashlib.md5(data).hexdigest(),
        "sha256": hashlib.sha256(data).hexdigest(),
        "segments": segs_out,
        "entry": entry,
    }


# ---------------------------------------------------------------------------
# HTTP/1.1 OTA & 404 Dry-Run Server
# ---------------------------------------------------------------------------

class OtaHttpServer:
    """Thread-safe HTTP/1.1 Range-capable OTA server with 404 dry-run mode."""

    def __init__(
        self,
        host: str = "127.0.0.1",
        port: int = 0,
        firmware_bytes: bytes = b"",
        mode: str = "dry-run-404",
    ) -> None:
        if mode not in ("dry-run-404", "live"):
            raise ValueError(f"unsupported mode: {mode}")
        self._lock = threading.Lock()
        self._mode = mode
        self._firmware = firmware_bytes
        self.requests_log: list[dict[str, Any]] = []
        self.complete_downloads = 0
        self._hit_event = threading.Event()
        parent = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"
            server_version = "nginx/1.24.0"
            sys_version = ""

            def log_message(self, fmt: str, *args: Any) -> None:
                return

            def _record(self, status_code: int, bytes_sent: int) -> None:
                entry = {
                    "method": self.command,
                    "path": self.path,
                    "client_ip": self.client_address[0],
                    "http_version": self.request_version,
                    "headers": {k.lower(): v for k, v in self.headers.items()},
                    "status_code": status_code,
                    "bytes_sent": bytes_sent,
                }
                with parent._lock:
                    parent.requests_log.append(entry)
                    parent._hit_event.set()

            def _handle_req(self, send_body: bool) -> None:
                mode_now, fw_now = parent.get_state()
                if mode_now == "dry-run-404":
                    self.send_response(404)
                    self.send_header("Content-Type", "text/plain")
                    self.send_header("Content-Length", "0")
                    self.send_header("Connection", "close")
                    self.end_headers()
                    self._record(404, 0)
                    self.close_connection = True
                    return

                size = len(fw_now)
                start, end, partial = 0, max(0, size - 1), False
                rng = self.headers.get("Range")
                if rng and size > 0:
                    m = re.match(r"bytes=(\d*)-(\d*)", rng.strip())
                    if m:
                        if m.group(1):
                            start = int(m.group(1))
                        if m.group(2):
                            end = min(int(m.group(2)), size - 1)
                        partial = True

                if size > 0 and (start >= size or start > end):
                    self.send_response(416)
                    self.send_header("Content-Range", f"bytes */{size}")
                    self.send_header("Content-Length", "0")
                    self.send_header("Connection", "close")
                    self.end_headers()
                    self._record(416, 0)
                    self.close_connection = True
                    return

                payload = fw_now[start : end + 1] if size > 0 else b""
                status = 206 if partial else 200
                self.send_response(status)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Accept-Ranges", "bytes")
                self.send_header("Content-Length", str(len(payload)))
                if partial:
                    self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
                self.send_header("Connection", "close")
                self.end_headers()

                sent = 0
                if send_body and payload:
                    try:
                        self.wfile.write(payload)
                        self.wfile.flush()
                        sent = len(payload)
                    except (BrokenPipeError, ConnectionResetError):
                        pass

                with parent._lock:
                    if send_body and size > 0 and sent == len(payload) and (not partial or end == size - 1):
                        parent.complete_downloads += 1
                self._record(status, sent)
                self.close_connection = True

            def do_HEAD(self) -> None:
                self._handle_req(send_body=False)

            def do_GET(self) -> None:
                self._handle_req(send_body=True)

        self._httpd = ThreadingHTTPServer((host, port), Handler)
        self.host, self.port = self._httpd.server_address[:2]
        self._thread: threading.Thread | None = None

    def get_state(self) -> tuple[str, bytes]:
        with self._lock:
            return self._mode, self._firmware

    def set_mode(self, mode: str, firmware_bytes: bytes | None = None) -> None:
        if mode not in ("dry-run-404", "live"):
            raise ValueError(f"unsupported mode: {mode}")
        with self._lock:
            self._mode = mode
            if firmware_bytes is not None:
                self._firmware = firmware_bytes

    def reset_events(self) -> None:
        with self._lock:
            self._hit_event.clear()

    def start(self) -> None:
        if self._thread is not None:
            return
        self._thread = threading.Thread(target=self._httpd.serve_forever, daemon=True)
        self._thread.start()

    def wait_for_request(self, timeout: float = 10.0) -> bool:
        return self._hit_event.wait(timeout)

    def stop(self) -> None:
        self._httpd.shutdown()
        self._httpd.server_close()
        if self._thread is not None:
            self._thread.join(timeout=2.0)
            self._thread = None

    def __enter__(self) -> "OtaHttpServer":
        self.start()
        return self

    def __exit__(self, *args: Any) -> None:
        self.stop()


# ---------------------------------------------------------------------------
# Helpers for Local IP, URL formatting, Wi-Fi AP (nmcli) & Fleet Store
# ---------------------------------------------------------------------------

def format_http_url(host: str, port: int, path: str) -> str:
    """Format URL, omitting :port automatically when port == 80 (stock 2.0.6 parser fix)."""
    clean_path = path if path.startswith("/") else f"/{path}"
    if port == 80:
        return f"http://{host}{clean_path}"
    return f"http://{host}:{port}{clean_path}"


def detect_route_src_ip(target_ip: str) -> str:
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect((target_ip, 54321))
            return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"


def _run(cmd: list[str], check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, capture_output=True, text=True, check=check)


def _detect_wifi_iface() -> str:
    r = _run(["nmcli", "-t", "-f", "DEVICE,TYPE", "dev", "status"])
    for line in r.stdout.splitlines():
        parts = line.split(":", 1)
        if len(parts) == 2 and parts[1].strip() == "wifi":
            return parts[0].strip()
    raise RuntimeError("Kein Wi-Fi-Interface ueber nmcli gefunden")


def _scan_yeelight_aps(iface: str) -> list[str]:
    _run(["sudo", "-n", "nmcli", "dev", "wifi", "rescan", "ifname", iface], check=False)
    time.sleep(2.5)
    r = _run(["sudo", "-n", "nmcli", "-t", "-f", "SSID", "dev", "wifi", "list", "ifname", iface])
    seen, out = set(), []
    for raw in r.stdout.splitlines():
        ssid = raw.strip().replace("\\:", ":")
        if re.match(r"^(yeelink-light-|miap)", ssid, re.I) and ssid not in seen:
            seen.add(ssid)
            out.append(ssid)
    return out


def _connect_ap_nmcli(iface: str, ssid: str, con_name: str = "yeelight-autark-tmp") -> str:
    _run(["sudo", "-n", "nmcli", "con", "delete", con_name], check=False)
    _run([
        "sudo", "-n", "nmcli", "con", "add",
        "type", "wifi", "ifname", iface, "con-name", con_name, "ssid", ssid,
        "ipv4.method", "auto", "ipv4.never-default", "yes", "ipv6.method", "ignore",
    ])
    _run(["sudo", "-n", "nmcli", "con", "up", con_name])
    time.sleep(1.5)
    r = _run(["ip", "-4", "route", "show", "dev", iface], check=False)
    m = re.search(r"default via (\d+\.\d+\.\d+\.\d+)", r.stdout)
    if m:
        return m.group(1)
    r2 = _run(["ip", "-4", "addr", "show", "dev", iface], check=False)
    m2 = re.search(r"inet (\d+\.\d+\.\d+)\.\d+/", r2.stdout)
    if m2:
        return f"{m2.group(1)}.1"
    return "192.168.10.1"


def _cleanup_ap_nmcli(con_name: str = "yeelight-autark-tmp") -> None:
    _run(["sudo", "-n", "nmcli", "con", "down", con_name], check=False)
    _run(["sudo", "-n", "nmcli", "con", "delete", con_name], check=False)


def _save_fleet(store_path: Path, fleet: dict[str, Any]) -> None:
    store_path.parent.mkdir(parents=True, exist_ok=True)
    store_path.write_text(json.dumps(fleet, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    store_path.chmod(0o600)


def _load_fleet(store_path: Path) -> dict[str, Any]:
    if not store_path.is_file():
        return {}
    return json.loads(store_path.read_text(encoding="utf-8"))


# ---------------------------------------------------------------------------
# Subcommand Workflows (callable directly from unit tests or CLI)
# ---------------------------------------------------------------------------

def run_flash_local(
    *,
    ip: str,
    token: str,
    firmware_path: Path,
    port: int = MIIO_PORT,
    serve_host: str | None = None,
    serve_port: int = 0,
    dry_run_only: bool = False,
    confirm_write: bool = False,
    skip_channel_guard: bool = False,
    allow_board_override: bool = False,
    timeout: float = 10.0,
) -> dict[str, Any]:
    """Run local UDP 54321 classification -> mandatory 404 dry-run -> optional live OTA."""
    client = MiioClient(ip=ip, token=token, port=port, timeout=min(timeout, 4.0))
    info = client.send("miIO.info", [])
    if not isinstance(info, dict):
        raise RuntimeError(f"unerwartete miIO.info-Antwort: {info!r}")

    cls = classify_bulb(info, allow_board_override=allow_board_override)
    if not cls["flash_allowed"]:
        raise RuntimeError(f"Safety-Block ({cls['category']}): {cls['reason']}")
    if cls["ota_channel"] != "udp_54321_offline" and not skip_channel_guard:
        raise RuntimeError(
            f"Kanal-Guard ({cls['ota_channel']}): Firmware >= 2.0.6 blockiert lokales UDP 54321 miIO.ota; "
            "nutze OPNsense-Firewall-Kaefig oder --skip-channel-guard im Labor"
        )

    fw_info = verify_firmware_file(firmware_path)
    fw_bytes = firmware_path.read_bytes()
    bind_host = serve_host or detect_route_src_ip(ip)

    with OtaHttpServer(host=bind_host, port=serve_port, firmware_bytes=fw_bytes, mode="dry-run-404") as srv:
        dry_url = format_http_url(bind_host, srv.port, "/dryrun_404.bin")
        srv.reset_events()
        ota_ack = client.send(
            "miIO.ota",
            {"app_url": dry_url, "file_md5": fw_info["md5"], "sequence": "a"},
        )
        hit = srv.wait_for_request(timeout=timeout)
        dry_reqs = [r for r in srv.requests_log if r["path"] == "/dryrun_404.bin"]
        if not hit or not dry_reqs:
            raise TimeoutError(f"Bulb hat 404-Dry-Run-URL {dry_url} nicht innerhalb {timeout}s angefragt")

        deadline = time.monotonic() + timeout
        ota_state = None
        while time.monotonic() < deadline:
            ota_state = client.send("miIO.get_ota_state", [])
            if ota_state == ["idle"] or ota_state == "idle":
                break
            time.sleep(0.15)
        if ota_state not in (["idle"], "idle"):
            raise RuntimeError(f"404-Dry-Run hinterliess unerwarteten OTA-State: {ota_state!r}")

        if dry_run_only or not confirm_write:
            return {
                "status": "DRY_RUN_404_OK",
                "classification": cls,
                "firmware": fw_info,
                "dry_run_url": dry_url,
                "dry_run_ota_ack": ota_ack,
                "dry_run_requests": len(dry_reqs),
                "ota_state": ota_state,
            }

        srv.set_mode("live", fw_bytes)
        srv.reset_events()
        live_url = format_http_url(bind_host, srv.port, f"/{firmware_path.name}")
        live_ack = client.send(
            "miIO.ota",
            {"app_url": live_url, "file_md5": fw_info["md5"], "sequence": "a"},
        )
        if not srv.wait_for_request(timeout=timeout):
            raise TimeoutError(f"Bulb hat Live-URL {live_url} nicht innerhalb {timeout}s angefragt")

        final_state = None
        final_prog = None
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            final_state = client.send("miIO.get_ota_state", [])
            final_prog = client.send("miIO.get_ota_progress", [])
            if final_state in (["installed"], "installed", ["failed"], "failed"):
                break
            time.sleep(0.15)

        live_gets = [r for r in srv.requests_log if r["path"] == f"/{firmware_path.name}" and r["method"] == "GET"]
        verdict = "FLASH_INSTALLED" if srv.complete_downloads == 1 and final_state in (["installed"], "installed") else (
            "REJECTED_RETRY_LOOP" if srv.complete_downloads >= 3 else "FLASH_TRIGGERED"
        )
        return {
            "status": verdict,
            "classification": cls,
            "firmware": fw_info,
            "live_url": live_url,
            "live_ota_ack": live_ack,
            "live_gets": len(live_gets),
            "complete_downloads": srv.complete_downloads,
            "ota_state": final_state,
            "ota_progress": final_prog,
        }


def _http_json(url: str, method: str = "GET", body: dict[str, Any] | None = None, timeout: float = 5.0) -> tuple[int, Any]:
    raw = json.dumps(body, separators=(",", ":")).encode("utf-8") if body is not None else None
    headers = {"Content-Type": "application/json"} if raw is not None else {}
    req = urllib.request.Request(url, data=raw, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            text = resp.read().decode("utf-8", errors="replace")
            return resp.status, json.loads(text) if text else {}
    except urllib.error.HTTPError as exc:
        text = exc.read().decode("utf-8", errors="replace")
        try:
            parsed = json.loads(text) if text else {}
        except json.JSONDecodeError:
            parsed = {"raw": text}
        return exc.code, parsed


def run_provision_mcufw(
    *,
    base_url: str,
    wifi_ssid: str,
    wifi_psk: str,
    mqtt_host: str = "192.168.1.106",
    mqtt_port: int = 1883,
    mqtt_user: str = "",
    mqtt_pass: str = "",
    mqtt_enabled: int = 1,
    pwm_freq_hz: int | None = None,
    power_on_mode: str | None = None,
    transition_ms: int | None = None,
    gamma_enable: int | None = None,
    timeout: float = 5.0,
) -> dict[str, Any]:
    """Onboard mcufw >= 0.8-phase4b.4 via HTTP /api/status and POST /api/config."""
    base = base_url.rstrip("/")
    st_code, st_json = _http_json(f"{base}/api/status", method="GET", timeout=timeout)
    if st_code != 200 or not isinstance(st_json, dict) or "fw" not in st_json:
        raise RuntimeError(f"GET /api/status fehlgeschlagen ({st_code}): {st_json!r}")

    cfg_payload: dict[str, Any] = {
        "wifi_ssid": wifi_ssid,
        "wifi_psk": wifi_psk,
        "mqtt_host": mqtt_host,
        "mqtt_port": int(mqtt_port),
        "mqtt_enabled": int(mqtt_enabled),
    }
    if mqtt_user:
        cfg_payload["mqtt_user"] = mqtt_user
    if mqtt_pass:
        cfg_payload["mqtt_pass"] = mqtt_pass
    if pwm_freq_hz is not None:
        cfg_payload["pwm_freq_hz"] = int(pwm_freq_hz)
    if power_on_mode is not None:
        cfg_payload["power_on_mode"] = power_on_mode
    if transition_ms is not None:
        cfg_payload["default_transition_ms"] = int(transition_ms)
    if gamma_enable is not None:
        cfg_payload["gamma_enable"] = int(gamma_enable)

    post_code, post_json = _http_json(f"{base}/api/config", method="POST", body=cfg_payload, timeout=timeout)
    if post_code != 200 or not isinstance(post_json, dict):
        raise RuntimeError(f"POST /api/config fehlgeschlagen ({post_code}): {post_json!r}")

    if post_json.get("wifi_provisioned") != 1:
        raise RuntimeError(f"Readback wifi_provisioned != 1: {post_json!r}")
    if post_json.get("mqtt_host") != mqtt_host or post_json.get("mqtt_port") != int(mqtt_port):
        raise RuntimeError(f"Readback mqtt mismatch: {post_json!r}")
    if post_json.get("mqtt_enabled") != int(mqtt_enabled):
        raise RuntimeError(f"Readback mqtt_enabled mismatch: {post_json!r}")

    return {"status": st_json, "config": post_json}


# ---------------------------------------------------------------------------
# CLI Entrypoints
# ---------------------------------------------------------------------------

def cmd_verify_image(args: argparse.Namespace) -> int:
    try:
        info = verify_firmware_file(Path(args.firmware))
    except ValueError as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    print(
        f"OK {info['path']}: {info['size']} B | segs={len(info['segments'])} | "
        f"entry=0x{info['entry']:08x} | md5={info['md5']} | sha256={info['sha256']}"
    )
    return 0


def _probe_single_ap(ip: str, port: int, timeout: float, allow_board_override: bool) -> dict[str, Any]:
    hello = miio_hello(ip, port=port, timeout=timeout)
    if not hello:
        raise RuntimeError(f"Kein miIO.hello von {ip}:{port}")
    if not hello["token_readable"]:
        raise RuntimeError(f"Device {ip} (did={hello['did_hex8']}) verbirgt Token ({hello['token_hex']})")
    info = MiioClient(ip, hello["token_bytes"], port=port, timeout=timeout).send("miIO.info", [])
    if not isinstance(info, dict):
        raise RuntimeError(f"Unerwartete miIO.info-Antwort: {info!r}")
    cls = classify_bulb(info, allow_board_override=allow_board_override)
    return {
        "did": hello["did_hex8"],
        "ap_ip": ip,
        "token": hello["token_hex"],
        "info": info,
        "classification": cls,
    }


def cmd_inventory(args: argparse.Namespace) -> int:
    store_path = Path(args.store)
    fleet = _load_fleet(store_path)
    records: list[dict[str, Any]] = []

    if args.ap_ip:
        records.append(_probe_single_ap(args.ap_ip, args.port, args.timeout, args.allow_board_override))
    else:
        iface = args.iface or _detect_wifi_iface()
        aps = _scan_yeelight_aps(iface)
        if not aps:
            print("Keine yeelink-light-* / miap* Setup-APs gefunden.")
            return 1
        for ssid in aps:
            con = "yeelight-autark-tmp"
            try:
                gw = _connect_ap_nmcli(iface, ssid, con_name=con)
                rec = _probe_single_ap(gw, args.port, args.timeout, args.allow_board_override)
                rec["ap_ssid"] = ssid
                records.append(rec)
            except Exception as exc:
                print(f"[WARN] {ssid}: {exc}", file=sys.stderr)
            finally:
                _cleanup_ap_nmcli(con_name=con)

    for rec in records:
        fleet[rec["did"]] = rec
    _save_fleet(store_path, fleet)

    print(f"{'DID':<10} {'Model':<22} {'HW':<7} {'FW':<12} {'Category':<20} {'Channel':<20} {'Token'}")
    for rec in records:
        c = rec["classification"]
        print(
            f"{rec['did']:<10} {c['model']:<22} {c['hw_ver'] or '-':<7} {c['fw_ver'] or '-':<12} "
            f"{c['category']:<20} {c['ota_channel']:<20} {rec['token']}"
        )
    print(f"\nGespeichert unter {store_path} ({len(fleet)} Geraete gesamt).")
    return 0


def cmd_serve(args: argparse.Namespace) -> int:
    fw_bytes = b""
    if args.mode == "live":
        if not args.firmware:
            print("FEHLER: --firmware ist fuer --mode live erforderlich", file=sys.stderr)
            return 2
        info = verify_firmware_file(Path(args.firmware))
        fw_bytes = Path(args.firmware).read_bytes()
        print(f"Verified {args.firmware}: {info['size']} B, md5={info['md5']}")
    with OtaHttpServer(host=args.host, port=args.port, firmware_bytes=fw_bytes, mode=args.mode) as srv:
        url = format_http_url(args.host, srv.port, "/mcufw_MRVL.bin" if args.mode == "live" else "/dryrun_404.bin")
        print(f"Serving ({args.mode}, HTTP/1.1) on {url} — Ctrl+C zum Beenden")
        try:
            while True:
                time.sleep(1.0)
        except KeyboardInterrupt:
            pass
    return 0


def cmd_flash_local(args: argparse.Namespace) -> int:
    res = run_flash_local(
        ip=args.ip,
        token=args.token,
        firmware_path=Path(args.firmware),
        port=args.port,
        serve_host=args.serve_host,
        serve_port=args.serve_port,
        dry_run_only=args.dry_run_only,
        confirm_write=args.confirm_write,
        skip_channel_guard=args.skip_channel_guard,
        allow_board_override=args.allow_board_override,
        timeout=args.timeout,
    )
    print(json.dumps(res, indent=2))
    return 0


def cmd_provision_mcufw(args: argparse.Namespace) -> int:
    con = "yeelight-mcufw-tmp"
    used_nmcli = False
    try:
        base_url = args.url
        if args.ap_ssid:
            iface = args.iface or _detect_wifi_iface()
            gw = _connect_ap_nmcli(iface, args.ap_ssid, con_name=con)
            used_nmcli = True
            if args.url == "http://192.168.10.1:8080":
                base_url = f"http://{gw}:8080"
        res = run_provision_mcufw(
            base_url=base_url,
            wifi_ssid=args.wifi_ssid,
            wifi_psk=args.wifi_psk,
            mqtt_host=args.mqtt_host,
            mqtt_port=args.mqtt_port,
            mqtt_user=args.mqtt_user,
            mqtt_pass=args.mqtt_pass,
            mqtt_enabled=args.mqtt_enabled,
            pwm_freq_hz=args.pwm_freq_hz,
            power_on_mode=args.power_on_mode,
            transition_ms=args.transition_ms,
            gamma_enable=args.gamma_enable,
            timeout=args.timeout,
        )
        print(json.dumps(res, indent=2))
        return 0
    finally:
        if used_nmcli:
            _cleanup_ap_nmcli(con_name=con)


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Autarkic Yeelight fleet flasher & mcufw provisioner")
    sub = p.add_subparsers(dest="cmd", required=True)

    pv = sub.add_parser("verify-image", help="Verify MRVL container CRCs & size <= 384 KB")
    pv.add_argument("firmware", help="Path to MRVL firmware .bin")
    pv.set_defaults(func=cmd_verify_image)

    pi = sub.add_parser("inventory", help="Scan Setup-APs or probe --ap-ip and classify bulbs")
    pi.add_argument("--ap-ip", help="Direct target AP IP (skips nmcli Wi-Fi scan)")
    pi.add_argument("--port", type=int, default=MIIO_PORT)
    pi.add_argument("--iface", help="Wi-Fi interface name (auto-detected if omitted)")
    pi.add_argument("--store", default=str(DEFAULT_FLEET_STORE))
    pi.add_argument("--timeout", type=float, default=3.0)
    pi.add_argument("--allow-board-override", action="store_true")
    pi.set_defaults(func=cmd_inventory)

    ps = sub.add_parser("serve", help="Run HTTP/1.1 OTA or 404 dry-run server")
    ps.add_argument("--host", default="127.0.0.1")
    ps.add_argument("--port", type=int, default=0)
    ps.add_argument("--mode", choices=["dry-run-404", "live"], default="dry-run-404")
    ps.add_argument("--firmware", help="Firmware .bin path (required for live mode)")
    ps.set_defaults(func=cmd_serve)

    pf = sub.add_parser("flash-local", help="100%% offline UDP 54321 dry-run -> OTA flash")
    pf.add_argument("--ip", required=True)
    pf.add_argument("--port", type=int, default=MIIO_PORT)
    pf.add_argument("--token", required=True, help="32-char hex miIO token")
    pf.add_argument("--firmware", required=True)
    pf.add_argument("--serve-host", default=None)
    pf.add_argument("--serve-port", type=int, default=0)
    pf.add_argument("--dry-run-only", action="store_true")
    pf.add_argument("--confirm-write", action="store_true")
    pf.add_argument("--skip-channel-guard", action="store_true")
    pf.add_argument("--allow-board-override", action="store_true")
    pf.add_argument("--timeout", type=float, default=10.0)
    pf.set_defaults(func=cmd_flash_local)

    pp = sub.add_parser("provision-mcufw", help="Provision mcufw HTTP :8080 portal without app")
    pp.add_argument("--url", default="http://192.168.10.1:8080")
    pp.add_argument("--iface", help="Optional Wi-Fi interface for nmcli AP join")
    pp.add_argument("--ap-ssid", help="Optional mcufw Setup-AP SSID to join via nmcli")
    pp.add_argument("--wifi-ssid", required=True)
    pp.add_argument("--wifi-psk", required=True)
    pp.add_argument("--mqtt-host", default="192.168.1.106")
    pp.add_argument("--mqtt-port", type=int, default=1883)
    pp.add_argument("--mqtt-user", default="")
    pp.add_argument("--mqtt-pass", default="")
    pp.add_argument("--mqtt-enabled", type=int, default=1)
    pp.add_argument("--pwm-freq-hz", type=int, default=None)
    pp.add_argument("--power-on-mode", choices=["last", "off", "on_default", "default"], default=None)
    pp.add_argument("--transition-ms", type=int, default=None)
    pp.add_argument("--gamma-enable", type=int, choices=[0, 1], default=None)
    pp.add_argument("--timeout", type=float, default=5.0)
    pp.set_defaults(func=cmd_provision_mcufw)
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
