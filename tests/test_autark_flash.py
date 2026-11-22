#!/usr/bin/env python3
"""Localhost-only loopback tests for tools/autark_flash.py (ZERO LAN traffic)."""
from __future__ import annotations

import hashlib
import json
import socket
import struct
import sys
import tempfile
import threading
import unittest
import urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import autark_flash as af  # noqa: E402


def _make_minimal_mrvl(payload_size: int = 18000) -> bytes:
    """Build a valid minimal 3-segment MRVL container passing verify_firmware_file."""
    seg1 = b"\x00\xf8\x01\x20\xd9\x32\x00\x1f" + b"\x00" * 248
    seg2 = b"\x00\xbf" * (payload_size // 2)
    seg3 = b"\x4d\x43\x57\x46"
    table_end = af.HDR_SIZE + 3 * af.SEG_SIZE
    off1 = 0x100
    off2 = off1 + len(seg1)
    off3 = off2 + len(seg2)

    buf = bytearray()
    buf.extend(struct.pack(af.HDR_FMT, b"MRVL", af.MAGIC2_REFERENCE, 1700000000, 3, 0x1F0032D9))
    buf.extend(struct.pack(af.SEG_FMT, 2, off1, len(seg1), 0x00100000, af._crc32_mrvl_fallback(seg1)))
    buf.extend(struct.pack(af.SEG_FMT, 2, off2, len(seg2), 0x1F0032D8, af._crc32_mrvl_fallback(seg2)))
    buf.extend(struct.pack(af.SEG_FMT, 2, off3, len(seg3), 0x20000040, af._crc32_mrvl_fallback(seg3)))
    buf.extend(b"\xff" * (off1 - table_end))
    buf.extend(seg1 + seg2 + seg3)
    return bytes(buf)


class MockMiioBulb:
    """Localhost 127.0.0.1:0 UDP miIO bulb simulator for wire & OTA tests."""

    def __init__(
        self,
        *,
        did_u64: int,
        token: bytes,
        info_reply: dict,
        tamper_reply_md5: bool = False,
        reject_with_3_gets: bool = False,
    ) -> None:
        self.did_u64 = did_u64
        self.token = token
        self.info_reply = info_reply
        self.tamper_reply_md5 = tamper_reply_md5
        self.reject_with_3_gets = reject_with_3_gets
        self.ota_calls: list[dict] = []
        self.ota_state = "idle"
        self.ota_progress = 0

        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.bind(("127.0.0.1", 0))
        self._sock.settimeout(0.1)
        self.port = self._sock.getsockname()[1]
        self._stop = False
        self._thread = threading.Thread(target=self._loop, daemon=True)

    def __enter__(self) -> "MockMiioBulb":
        self._thread.start()
        return self

    def __exit__(self, *args) -> None:
        self._stop = True
        self._thread.join(timeout=2.0)
        self._sock.close()

    def _run_http_download(self, app_url: str) -> None:
        attempts = 3 if self.reject_with_3_gets else 1
        try:
            if app_url.endswith("/dryrun_404.bin"):
                attempts = 1
            for _ in range(attempts):
                try:
                    with urllib.request.urlopen(app_url, timeout=3.0) as resp:
                        _ = resp.read()
                        if resp.status == 200 and not self.reject_with_3_gets:
                            self.ota_progress = 100
                            self.ota_state = "installed"
                            return
                except urllib.error.HTTPError as exc:
                    _ = exc.read()
                    code = exc.code
                    exc.close()
                    if code == 404:
                        self.ota_state = "idle"
                        return
            self.ota_state = "idle"
        except Exception:
            self.ota_state = "failed"

    def _loop(self) -> None:
        stamp = 42
        while not self._stop:
            try:
                data, addr = self._sock.recvfrom(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            if len(data) < 32 or data[:2] != b"\x21\x31":
                continue
            if len(data) == 32 and data[4:32] == b"\xff" * 28:
                hdr = struct.pack(">HHQI", 0x2131, 32, self.did_u64, stamp)
                self._sock.sendto(hdr + self.token, addr)
                continue

            body = data[32:]
            if af._md5(data[:16] + self.token + body) != data[16:32]:
                continue
            req = json.loads(af.aes_cbc_decrypt(body, self.token).decode("utf-8"))
            method = req.get("method")
            req_id = req.get("id", 1)
            if method == "miIO.info":
                reply_obj = {"id": req_id, "result": self.info_reply}
            elif method == "miIO.get_ota_state":
                reply_obj = {"id": req_id, "result": [self.ota_state]}
            elif method == "miIO.get_ota_progress":
                reply_obj = {"id": req_id, "result": [self.ota_progress]}
            elif method == "miIO.ota":
                params = req.get("params") or {}
                self.ota_calls.append(params)
                self.ota_state = "downloading"
                threading.Thread(target=self._run_http_download, args=(params["app_url"],), daemon=True).start()
                reply_obj = {"id": req_id, "result": ["ok"]}
            else:
                reply_obj = {"id": req_id, "error": {"code": -32601, "message": "unknown"}}

            enc = af.aes_cbc_encrypt(json.dumps(reply_obj, separators=(",", ":")).encode("utf-8"), self.token)
            hdr = struct.pack(">HHQI", 0x2131, 32 + len(enc), self.did_u64, stamp + 1)
            chk = bytearray(af._md5(hdr + self.token + enc))
            if self.tamper_reply_md5:
                chk[0] ^= 0xFF
            self._sock.sendto(hdr + bytes(chk) + enc, addr)


class TestAutarkFlash(unittest.TestCase):
    def test_cli_parser_help_formatting(self) -> None:
        parser = af.build_parser()
        _ = parser.format_help()
        for sub_name in ("verify-image", "inventory", "serve", "flash-local", "provision-mcufw"):
            ns = parser.parse_args(
                ["verify-image", "a.bin"] if sub_name == "verify-image" else
                ["inventory"] if sub_name == "inventory" else
                ["serve"] if sub_name == "serve" else
                ["flash-local", "--ip", "127.0.0.1", "--token", "00" * 16, "--firmware", "a.bin"] if sub_name == "flash-local" else
                ["provision-mcufw", "--wifi-ssid", "s", "--wifi-psk", "p"]
            )
            self.assertEqual(ns.cmd, sub_name)

    def test_miio_64bit_did_and_reply_md5_guard(self) -> None:
        token = bytes.fromhex("0f27f4ecba940fd63904abc1df7e2d1e")
        did64 = 0x0000000105EB8034
        info = {"model": "yeelink.light.color2", "hw_ver": "mw302", "fw_ver": "1.4.2_0027"}
        with MockMiioBulb(did_u64=did64, token=token, info_reply=info) as bulb:
            cli = af.MiioClient("127.0.0.1", token.hex(), port=bulb.port, timeout=2.0)
            res = cli.send("miIO.info", [])
            self.assertEqual(res["model"], "yeelink.light.color2")

        with MockMiioBulb(did_u64=did64, token=token, info_reply=info, tamper_reply_md5=True) as bad_bulb:
            cli = af.MiioClient("127.0.0.1", token.hex(), port=bad_bulb.port, timeout=2.0)
            with self.assertRaises(ValueError):
                cli.send("miIO.info", [])

    def test_fleet_classifier_matrix(self) -> None:
        c2_old = af.classify_bulb({"model": "yeelink.light.color2", "hw_ver": "mw302", "fw_ver": "1.4.2_0027", "miio_ver": "0.0.2"})
        self.assertEqual(c2_old["category"], "offline_direct")
        self.assertTrue(c2_old["flash_allowed"])
        self.assertEqual(c2_old["ota_channel"], "udp_54321_offline")

        c2_new = af.classify_bulb({"model": "yeelink.light.color2", "hw_ver": "mw302", "fw_ver": "2.0.6_0065", "miio_ver": "0.0.3"})
        self.assertEqual(c2_new["category"], "firewall_rpc_once")
        self.assertTrue(c2_new["flash_allowed"])
        self.assertEqual(c2_new["ota_channel"], "firewall_rpc_once")

        c1_old = af.classify_bulb({"model": "yeelink.light.color1", "hw_ver": "mw300", "fw_ver": "1.4.2_0061"})
        self.assertEqual(c1_old["category"], "needs_board_profile")
        self.assertFalse(c1_old["flash_allowed"])
        self.assertEqual(c1_old["ota_channel"], "udp_54321_offline")

        c4 = af.classify_bulb({"model": "yeelink.light.color4", "hw_ver": "esp32", "fw_ver": "2.0.6_0031"})
        self.assertEqual(c4["category"], "unsupported_soc")
        self.assertFalse(c4["flash_allowed"])
        self.assertEqual(c4["ota_channel"], "none")

    def test_mrvl_container_verifier(self) -> None:
        real_bin = ROOT / "sdk" / "apps" / "mcufw" / "build" / "mcufw_MRVL.bin"
        with tempfile.TemporaryDirectory() as td:
            td_path = Path(td)
            valid_path = real_bin if real_bin.is_file() else td_path / "valid.bin"
            if not real_bin.is_file():
                valid_path.write_bytes(_make_minimal_mrvl())
            info = af.verify_firmware_file(valid_path)
            self.assertGreaterEqual(info["size"], af.MIN_IMAGE_SIZE)
            self.assertLessEqual(info["size"], af.MAX_APP_SLOT_SIZE)

            bad_crc = bytearray(_make_minimal_mrvl())
            bad_crc[-1] ^= 0x55
            p_crc = td_path / "bad_crc.bin"
            p_crc.write_bytes(bytes(bad_crc))
            with self.assertRaises(ValueError):
                af.verify_firmware_file(p_crc)

            p_magic = td_path / "bad_magic.bin"
            p_magic.write_bytes(b"NOPE" + _make_minimal_mrvl()[4:])
            with self.assertRaises(ValueError):
                af.verify_firmware_file(p_magic)

            p_big = td_path / "oversize.bin"
            p_big.write_bytes(b"MRVL" + b"\x00" * (af.MAX_APP_SLOT_SIZE + 16))
            with self.assertRaises(ValueError):
                af.verify_firmware_file(p_big)

    def test_http11_ota_server_404_and_range(self) -> None:
        payload = b"0123456789ABCDEF" * 16
        with af.OtaHttpServer(host="127.0.0.1", port=0, firmware_bytes=payload, mode="dry-run-404") as srv:
            import http.client

            conn = http.client.HTTPConnection("127.0.0.1", srv.port, timeout=3.0)
            conn.request("GET", "/dryrun_404.bin")
            r404 = conn.getresponse()
            body404 = r404.read()
            self.assertEqual(r404.status, 404)
            self.assertEqual(r404.version, 11)
            self.assertEqual(body404, b"")
            conn.close()

            srv.set_mode("live", payload)
            conn = http.client.HTTPConnection("127.0.0.1", srv.port, timeout=3.0)
            conn.request("GET", "/fw.bin", headers={"Range": "bytes=0-63"})
            r206 = conn.getresponse()
            body206 = r206.read()
            self.assertEqual(r206.status, 206)
            self.assertEqual(r206.version, 11)
            self.assertEqual(body206, payload[:64])
            self.assertEqual(r206.getheader("Content-Range"), f"bytes 0-63/{len(payload)}")
            conn.close()

            conn = http.client.HTTPConnection("127.0.0.1", srv.port, timeout=3.0)
            conn.request("GET", "/fw.bin")
            r200 = conn.getresponse()
            body200 = r200.read()
            self.assertEqual(r200.status, 200)
            self.assertEqual(body200, payload)
            self.assertEqual(srv.complete_downloads, 1)
            conn.close()

    def test_flash_local_loopback_and_guards(self) -> None:
        token = bytes.fromhex("11223344556677889900aabbccddeeff")
        with tempfile.TemporaryDirectory() as td:
            fw_path = Path(td) / "mcufw_MRVL.bin"
            fw_path.write_bytes(_make_minimal_mrvl())

            # Safety guard blocks ESP32 color4 BEFORE sending miIO.ota
            info_c4 = {"model": "yeelink.light.color4", "hw_ver": "esp32", "fw_ver": "2.0.6_0031"}
            with MockMiioBulb(did_u64=0x1234, token=token, info_reply=info_c4) as b4:
                with self.assertRaises(RuntimeError):
                    af.run_flash_local(ip="127.0.0.1", port=b4.port, token=token.hex(), firmware_path=fw_path)
                self.assertEqual(b4.ota_calls, [])

            # Safety guard blocks 2.0.6_0065 local channel BEFORE sending miIO.ota
            info_206 = {"model": "yeelink.light.color2", "hw_ver": "mw302", "fw_ver": "2.0.6_0065", "miio_ver": "0.0.3"}
            with MockMiioBulb(did_u64=0x5678, token=token, info_reply=info_206) as b206:
                with self.assertRaises(RuntimeError):
                    af.run_flash_local(ip="127.0.0.1", port=b206.port, token=token.hex(), firmware_path=fw_path)
                self.assertEqual(b206.ota_calls, [])

            # Valid 1.4.2_0027 dry-run only -> 404 hit, no live download
            info_142 = {"model": "yeelink.light.color2", "hw_ver": "mw302", "fw_ver": "1.4.2_0027", "miio_ver": "0.0.2"}
            with MockMiioBulb(did_u64=0x05EB8034, token=token, info_reply=info_142) as bulb:
                res_dry = af.run_flash_local(
                    ip="127.0.0.1", port=bulb.port, token=token.hex(),
                    firmware_path=fw_path, serve_host="127.0.0.1", dry_run_only=True, timeout=5.0,
                )
                self.assertEqual(res_dry["status"], "DRY_RUN_404_OK")
                self.assertEqual(len(bulb.ota_calls), 1)
                self.assertTrue(bulb.ota_calls[0]["app_url"].endswith("/dryrun_404.bin"))

            # Full confirm-write -> 404 dry-run first, then 1 live GET -> FLASH_INSTALLED
            with MockMiioBulb(did_u64=0x05EB8034, token=token, info_reply=info_142) as bulb2:
                res_live = af.run_flash_local(
                    ip="127.0.0.1", port=bulb2.port, token=token.hex(),
                    firmware_path=fw_path, serve_host="127.0.0.1", confirm_write=True, timeout=5.0,
                )
                self.assertEqual(res_live["status"], "FLASH_INSTALLED")
                self.assertEqual(len(bulb2.ota_calls), 2)
                self.assertEqual(res_live["live_gets"], 1)
                self.assertEqual(res_live["complete_downloads"], 1)

            # 3-GET rejection loop detection (non-destructive timeout -> REJECTED_RETRY_LOOP)
            with MockMiioBulb(did_u64=0x05EB8034, token=token, info_reply=info_142, reject_with_3_gets=True) as bulb3:
                res_rej = af.run_flash_local(
                    ip="127.0.0.1", port=bulb3.port, token=token.hex(),
                    firmware_path=fw_path, serve_host="127.0.0.1", confirm_write=True, timeout=1.0,
                )
                self.assertEqual(res_rej["status"], "REJECTED_RETRY_LOOP")
                self.assertEqual(res_rej["complete_downloads"], 3)

    def test_provision_mcufw_loopback(self) -> None:
        state = {"fw": "0.8-phase4b.4", "wlan": "AP", "wifi_provisioned": 0}
        seen_posts: list[dict] = []

        class PortalHandler(BaseHTTPRequestHandler):
            def log_message(self, *args) -> None:
                return

            def _send(self, code: int, obj: dict) -> None:
                raw = json.dumps(obj).encode("utf-8")
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)

            def do_GET(self) -> None:
                if self.path == "/api/status":
                    self._send(200, state)
                else:
                    self._send(404, {"error": "not_found"})

            def do_POST(self) -> None:
                if self.path != "/api/config":
                    return self._send(404, {"error": "not_found"})
                length = int(self.headers.get("Content-Length", "0"))
                body = json.loads(self.rfile.read(length).decode("utf-8"))
                seen_posts.append(body)
                resp = {
                    "wifi_ssid": body["wifi_ssid"],
                    "wifi_psk": "***",
                    "wifi_provisioned": 1 if body.get("wifi_ssid") else 0,
                    "mqtt_host": body["mqtt_host"],
                    "mqtt_port": body["mqtt_port"],
                    "mqtt_enabled": body["mqtt_enabled"],
                    "pwm_freq_hz": body.get("pwm_freq_hz", 768),
                    "power_on_mode": body.get("power_on_mode", "last"),
                }
                self._send(200, resp)

        srv = HTTPServer(("127.0.0.1", 0), PortalHandler)
        t = threading.Thread(target=srv.serve_forever, daemon=True)
        t.start()
        try:
            url = f"http://127.0.0.1:{srv.server_address[1]}"
            res = af.run_provision_mcufw(
                base_url=url,
                wifi_ssid="HomeLan001",
                wifi_psk="SecretPass",
                mqtt_host="192.168.1.106",
                mqtt_port=1883,
                pwm_freq_hz=4000,
                power_on_mode="off",
            )
            self.assertEqual(res["config"]["wifi_provisioned"], 1)
            self.assertEqual(res["config"]["mqtt_host"], "192.168.1.106")
            self.assertEqual(seen_posts[0]["wifi_ssid"], "HomeLan001")
            self.assertEqual(seen_posts[0]["pwm_freq_hz"], 4000)
        finally:
            srv.shutdown()
            srv.server_close()
            t.join(timeout=2.0)


if __name__ == "__main__":
    unittest.main()
