#!/usr/bin/env python3
"""Localhost-only loopback tests for tools/jailed_cloud_ota.py (ZERO internet/LAN traffic)."""
from __future__ import annotations

import base64
import json
import sys
import tempfile
import threading
import unittest
import urllib.parse
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import autark_flash as af  # noqa: E402
import jailed_cloud_ota as jc  # noqa: E402


class _MockXiaomiCloudServer:
    """Simulated /app/* ENCRYPT-RC4 RPC server running purely on 127.0.0.1:0."""

    def __init__(self, ssecurity_b64: str) -> None:
        self.ssecurity_b64 = ssecurity_b64
        self.calls: list[dict] = []
        self.ota_state = "idle"
        self.ota_progress = 0
        parent = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args) -> None:
                return

            def do_POST(self) -> None:
                parsed = urllib.parse.urlparse(self.path)
                qs = {k: v[0] for k, v in urllib.parse.parse_qs(parsed.query).items()}
                nonce = qs["_nonce"]
                s_nonce = jc.signed_nonce(parent.ssecurity_b64, nonce)

                # Verify outer signature
                enc_only = {k: qs[k] for k in ("data", "rc4_hash__") if k in qs}
                expected_sig = jc.generate_enc_signature(parsed.path, "POST", s_nonce, enc_only)
                if qs.get("signature") != expected_sig:
                    self.send_response(403)
                    self.end_headers()
                    return

                data_plain = jc.decrypt_rc4_bytes(s_nonce, qs["data"]).decode("utf-8")
                rc4_hash_plain = jc.decrypt_rc4_bytes(s_nonce, qs["rc4_hash__"]).decode("utf-8")
                inner_sig = jc.generate_enc_signature(parsed.path, "POST", s_nonce, {"data": data_plain})
                if rc4_hash_plain != inner_sig:
                    self.send_response(403)
                    self.end_headers()
                    return

                req_obj = json.loads(data_plain)
                parent.calls.append({"path": parsed.path, "body": req_obj})

                if parsed.path == "/app/user/get_bindkey":
                    resp_obj = {"code": 0, "result": {"bindkey": "testbindkey12345"}}
                elif parsed.path == "/app/home/rpc/99319860":
                    m = req_obj.get("method")
                    if m == "miIO.get_ota_state":
                        resp_obj = {"code": 0, "result": [parent.ota_state]}
                    elif m == "miIO.get_ota_progress":
                        resp_obj = {"code": 0, "result": [parent.ota_progress]}
                    elif m == "miIO.ota":
                        app_url = (req_obj.get("params") or {}).get("app_url", "")
                        if "dryrun_404" in app_url:
                            parent.ota_state = "idle"
                        else:
                            parent.ota_progress = 100
                            parent.ota_state = "installed"
                        resp_obj = {"code": 0, "result": ["ok"]}
                    else:
                        resp_obj = {"code": 0, "result": ["ok"]}
                else:
                    resp_obj = {"code": -1, "message": "unknown path"}

                enc_resp = jc.encrypt_rc4_b64(s_nonce, json.dumps(resp_obj, separators=(",", ":")))
                raw = enc_resp.encode("ascii")
                self.send_response(200)
                self.send_header("Content-Type", "text/plain")
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)

        self._httpd = HTTPServer(("127.0.0.1", 0), Handler)
        self.port = self._httpd.server_address[1]
        self._thread = threading.Thread(target=self._httpd.serve_forever, daemon=True)

    def __enter__(self) -> "_MockXiaomiCloudServer":
        self._thread.start()
        return self

    def __exit__(self, *args) -> None:
        self._httpd.shutdown()
        self._httpd.server_close()
        self._thread.join(timeout=2.0)


class TestJailedCloudOta(unittest.TestCase):
    def test_cli_parser(self) -> None:
        p = jc.build_parser()
        _ = p.format_help()
        self.assertEqual(p.parse_args(["login", "--username", "a@b.de"]).cmd, "login")
        self.assertEqual(p.parse_args(["pair-ap", "--wifi-ssid", "Home", "--wifi-psk", "123"]).cmd, "pair-ap")
        self.assertEqual(p.parse_args(["rpc-ota", "--did", "05eb8034", "--state"]).cmd, "rpc-ota")

    def test_rc4_with_1024_drop_and_did_normalization(self) -> None:
        ssec = base64.b64encode(b"0123456789ABCDEF").decode("ascii")
        nonce = base64.b64encode(b"ABCDEFGH1234").decode("ascii")
        sn = jc.signed_nonce(ssec, nonce)
        msg = '{"id":1,"method":"miIO.ota","params":{"app_url":"http://10.10.10.13/fw.bin"}}'
        c = jc.encrypt_rc4_b64(sn, msg)
        self.assertEqual(jc.decrypt_rc4_bytes(sn, c).decode("utf-8"), msg)

        self.assertEqual(jc.normalize_did("05eb8034"), "99319860")
        self.assertEqual(jc.normalize_did("0x05eb8034"), "99319860")
        self.assertEqual(jc.normalize_did("99319860"), "99319860")

    def test_rpc_ota_loopback_state_dryrun_and_live(self) -> None:
        ssec = base64.b64encode(b"SecretKeyForMock").decode("ascii")
        with _MockXiaomiCloudServer(ssec) as srv, tempfile.TemporaryDirectory() as td:
            sess_path = Path(td) / "mi_session.json"
            jc.save_session({
                "username": "bulbblub@ji5.de",
                "server": "de",
                "userId": "123456789",
                "ssecurity": ssec,
                "serviceToken": "mocktoken",
                "agent": "mock-agent",
                "device_id": "abcdef",
            }, sess_path)
            api_base = f"http://127.0.0.1:{srv.port}/app"

            res_st = jc.run_rpc_ota(
                did="05eb8034", state_only=True, session_file=sess_path, api_base_url=api_base, poll_interval=0.05
            )
            self.assertEqual(res_st["did"], "99319860")
            self.assertEqual(res_st["status"], "STATE_OK")

            res_dry = jc.run_rpc_ota(
                did="05eb8034",
                dry_run_url="http://10.10.10.13/dryrun_404.bin",
                session_file=sess_path,
                api_base_url=api_base,
                poll=3,
                poll_interval=0.05,
            )
            self.assertEqual(res_dry["status"], "DRY_RUN_RPC_DONE")
            self.assertEqual(res_dry["dry_run_final_state"]["result"], ["idle"])

            with self.assertRaises(RuntimeError):
                jc.run_rpc_ota(
                    did="05eb8034",
                    live_url="http://10.10.10.13/mcufw_MRVL.bin",
                    confirm_write=False,
                    session_file=sess_path,
                    api_base_url=api_base,
                )

            res_live = jc.run_rpc_ota(
                did="05eb8034",
                live_url="http://10.10.10.13/mcufw_MRVL.bin",
                confirm_write=True,
                session_file=sess_path,
                api_base_url=api_base,
                poll=3,
                poll_interval=0.05,
            )
            self.assertEqual(res_live["status"], "LIVE_RPC_DONE")
            self.assertEqual(res_live["live_final"]["state"]["result"], ["installed"])

    def test_pair_ap_loopback_with_mock_bulb(self) -> None:
        import socket
        import struct

        token = bytes.fromhex("dc6e80f9ebfce30b85a0ad5eeb422eea")
        did64 = 0x0000000005EB8034
        ssec = base64.b64encode(b"PairMockKey12345").decode("ascii")
        seen: list[tuple[str, object]] = []
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(("127.0.0.1", 0))
        sock.settimeout(0.1)
        port = sock.getsockname()[1]
        stop = False

        def loop() -> None:
            while not stop:
                try:
                    data, addr = sock.recvfrom(4096)
                except socket.timeout:
                    continue
                except OSError:
                    break
                if len(data) == 32 and data[4:32] == b"\xff" * 28:
                    sock.sendto(struct.pack(">HHQI", 0x2131, 32, did64, 100) + token, addr)
                    continue
                req = json.loads(af.aes_cbc_decrypt(data[32:], token).decode("utf-8"))
                m = req["method"]
                seen.append((m, req.get("params")))
                res = {"model": "yeelink.light.color2", "hw_ver": "mw302", "fw_ver": "2.0.6_0065", "miio_ver": "0.0.3"} if m == "miIO.info" else ["ok"]
                enc = af.aes_cbc_encrypt(json.dumps({"id": req["id"], "result": res}, separators=(",", ":")).encode("utf-8"), token)
                hdr = struct.pack(">HHQI", 0x2131, 32 + len(enc), did64, 101)
                sock.sendto(hdr + af._md5(hdr + token + enc) + enc, addr)

        th = threading.Thread(target=loop, daemon=True)
        th.start()
        try:
            with _MockXiaomiCloudServer(ssec) as cloud_srv, tempfile.TemporaryDirectory() as td:
                sess_path = Path(td) / "mi_session.json"
                jc.save_session({
                    "username": "bulbblub@ji5.de",
                    "server": "de",
                    "userId": "777888999",
                    "ssecurity": ssec,
                    "serviceToken": "mocktoken",
                    "agent": "mock-agent",
                    "device_id": "abcdef",
                }, sess_path)

                res = jc.run_pair_ap(
                    ap_ip="127.0.0.1",
                    port=port,
                    wifi_ssid="HomeLan001",
                    wifi_psk="WifiPass",
                    session_file=sess_path,
                    api_base_url=f"http://127.0.0.1:{cloud_srv.port}/app",
                )
                self.assertEqual(res["did_decimal"], "99319860")
                self.assertEqual(res["uid"], 777888999)
                self.assertTrue(res["used_bind_key"])
                self.assertEqual(seen[1], ("set_ps", ["cfg_lan_ctrl", "1"]))
                self.assertEqual(seen[2][0], "miIO.config_router")
                self.assertEqual(seen[2][1]["bind_key"], "testbindkey12345")
                self.assertEqual(seen[2][1]["country_domain"], "de")
        finally:
            stop = True
            th.join(timeout=2.0)
            sock.close()


if __name__ == "__main__":
    unittest.main()
