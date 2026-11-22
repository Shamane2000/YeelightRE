#!/usr/bin/env python3
"""Zero-dependency Xiaomi Cloud throwaway login, AP binder & jailed RPC OTA trigger.

Pure Python 3 stdlib (zero pip dependencies). Uses sibling autark_flash.py for
miIO UDP 54321 transport and Marvell/color2 hardware safety guards.

Workflow for firmware >= 2.0.6 inside an OPNsense/AdGuard Firewall Cage:
  1. login   : Frank authenticates once interactively with a throwaway account (e.g. bulbblub@ji5.de)
  2. pair-ap : Zero-app Setup-AP Wi-Fi provisioning + uid/bind_key binding + cfg_lan_ctrl=1
  3. rpc-ota : Relays a single miIO.ota instruction pointing at Mordor LAN HTTP/1.1 :80
               (first --dry-run-url 404 verification, then --live-url --confirm-write)
"""
from __future__ import annotations

import argparse
import base64
import getpass
import hashlib
import http.cookiejar
import json
import os
import random
import re
import string
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any

TOOLS_DIR = str(Path(__file__).resolve().parent)
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

import autark_flash as af  # noqa: E402

DEFAULT_CACHE_DIR = Path.home() / ".cache" / "yeelight-autark"
DEFAULT_SESSION_FILE = DEFAULT_CACHE_DIR / "mi_session.json"
DEFAULT_CAPTCHA_FILE = DEFAULT_CACHE_DIR / "captcha.jpg"
DEFAULT_TZ = "Europe/Berlin"
DEFAULT_COUNTRY = "de"


# ---------------------------------------------------------------------------
# Pure-Python RC4 (with 1024-byte drop) + Xiaomi MIOT ENCRYPT-RC4 Crypto
# ---------------------------------------------------------------------------

def rc4_crypt(key: bytes, data: bytes) -> bytes:
    """Standard RC4 skipping the first 1024 keystream bytes (MIOT ENCRYPT-RC4)."""
    if not key:
        raise ValueError("RC4 key must not be empty")
    s = list(range(256))
    j = 0
    klen = len(key)
    for i in range(256):
        j = (j + s[i] + key[i % klen]) & 0xFF
        s[i], s[j] = s[j], s[i]

    i = 0
    j = 0
    out = bytearray(len(data))
    for idx in range(1024 + len(data)):
        i = (i + 1) & 0xFF
        j = (j + s[i]) & 0xFF
        s[i], s[j] = s[j], s[i]
        k = s[(s[i] + s[j]) & 0xFF]
        if idx >= 1024:
            out[idx - 1024] = data[idx - 1024] ^ k
    return bytes(out)


def signed_nonce(ssecurity_b64: str, nonce_b64: str) -> str:
    digest = hashlib.sha256(base64.b64decode(ssecurity_b64) + base64.b64decode(nonce_b64)).digest()
    return base64.b64encode(digest).decode("ascii")


def generate_nonce(millis: int | None = None) -> str:
    ms = round(time.time() * 1000) if millis is None else millis
    raw = os.urandom(8) + (int(ms / 60000)).to_bytes(4, byteorder="big")
    return base64.b64encode(raw).decode("ascii")


def encrypt_rc4_b64(password_b64: str, plaintext: str) -> str:
    raw = rc4_crypt(base64.b64decode(password_b64), plaintext.encode("utf-8"))
    return base64.b64encode(raw).decode("ascii")


def decrypt_rc4_bytes(password_b64: str, cipher_b64: str) -> bytes:
    return rc4_crypt(base64.b64decode(password_b64), base64.b64decode(cipher_b64))


def generate_enc_signature(url: str, method: str, s_nonce: str, params: dict[str, str]) -> str:
    parsed_path = urllib.parse.urlparse(url).path
    url_path = parsed_path.replace("/app/", "/", 1) if "/app/" in parsed_path else parsed_path
    sig_parts = [method.upper(), url_path]
    for k, v in params.items():
        sig_parts.append(f"{k}={v}")
    sig_parts.append(s_nonce)
    sig_str = "&".join(sig_parts)
    return base64.b64encode(hashlib.sha1(sig_str.encode("utf-8")).digest()).decode("ascii")


def generate_enc_params(
    url: str,
    method: str,
    s_nonce: str,
    nonce: str,
    params: dict[str, str],
    ssecurity: str,
) -> dict[str, str]:
    fields: dict[str, str] = {k: str(v) for k, v in params.items()}
    fields["rc4_hash__"] = generate_enc_signature(url, method, s_nonce, fields)
    for k in list(fields.keys()):
        fields[k] = encrypt_rc4_b64(s_nonce, fields[k])
    fields["signature"] = generate_enc_signature(url, method, s_nonce, fields)
    fields["ssecurity"] = ssecurity
    fields["_nonce"] = nonce
    return fields


def normalize_did(raw_did: str | int) -> str:
    """Convert hex DID (e.g. '05eb8034' or '0x05eb8034') or decimal string to Mi-Cloud decimal DID."""
    s = str(raw_did).strip()
    if not s:
        raise ValueError("DID darf nicht leer sein")
    if s.lower().startswith("0x"):
        return str(int(s, 16))
    if re.fullmatch(r"[0-9]+", s):
        return str(int(s, 10))
    if re.fullmatch(r"[0-9a-fA-F]+", s):
        return str(int(s, 16))
    raise ValueError(f"Ungueltiges DID-Format: {raw_did!r}")


# ---------------------------------------------------------------------------
# Stdlib HTTP Cookie Client (without external requests dependency)
# ---------------------------------------------------------------------------

class _NoRedirectHandler(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req: Any, fp: Any, code: int, msg: str, headers: Any, newurl: str) -> None:
        return None


class _RawHttpResponse:
    def __init__(self, status: int, url: str, headers: Any, body: bytes) -> None:
        self.status_code = status
        self.url = url
        self.headers = headers
        self.content = body
        self.text = body.decode("utf-8", errors="replace")

    def json(self) -> Any:
        return json.loads(self.text)


class _StdSession:
    def __init__(self) -> None:
        self.jar = http.cookiejar.CookieJar()
        self._follow_opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(self.jar))
        self._no_redir_opener = urllib.request.build_opener(
            urllib.request.HTTPCookieProcessor(self.jar),
            _NoRedirectHandler(),
        )

    def set_cookie(self, name: str, value: str, domain: str) -> None:
        ck = http.cookiejar.Cookie(
            version=0, name=name, value=value, port=None, port_specified=False,
            domain=domain, domain_specified=bool(domain), domain_initial_dot=domain.startswith("."),
            path="/", path_specified=True, secure=True, expires=None, discard=True,
            comment=None, comment_url=None, rest={}, rfc2109=False,
        )
        self.jar.set_cookie(ck)

    def get_cookie(self, name: str) -> str | None:
        for ck in self.jar:
            if ck.name == name:
                return ck.value
        return None

    def request(
        self,
        method: str,
        url: str,
        *,
        params: dict[str, str] | None = None,
        data: dict[str, str] | bytes | None = None,
        headers: dict[str, str] | None = None,
        cookies: dict[str, str] | None = None,
        allow_redirects: bool = True,
        timeout: float = 15.0,
    ) -> _RawHttpResponse:
        target = url
        if params:
            sep = "&" if "?" in url else "?"
            target = f"{url}{sep}{urllib.parse.urlencode(params)}"
        body: bytes | None = None
        if isinstance(data, dict):
            body = urllib.parse.urlencode(data).encode("utf-8")
        elif isinstance(data, bytes):
            body = data
        elif method.upper() == "POST":
            body = b""

        hdrs = dict(headers or {})
        if cookies:
            cookie_str = "; ".join(f"{k}={v}" for k, v in cookies.items())
            hdrs["Cookie"] = f"{hdrs['Cookie']}; {cookie_str}" if "Cookie" in hdrs else cookie_str

        req = urllib.request.Request(target, data=body, headers=hdrs, method=method.upper())
        opener = self._follow_opener if allow_redirects else self._no_redir_opener
        try:
            with opener.open(req, timeout=timeout) as resp:
                return _RawHttpResponse(resp.status, resp.geturl(), resp.headers, resp.read())
        except urllib.error.HTTPError as exc:
            raw = exc.read()
            code = exc.code
            hdr = exc.headers
            geturl = exc.geturl()
            exc.close()
            return _RawHttpResponse(code, geturl, hdr, raw)


# ---------------------------------------------------------------------------
# Xiaomi Cloud Session & Login / RPC
# ---------------------------------------------------------------------------

def _gen_agent() -> str:
    agent_id = "".join(random.choice("ABCDE") for _ in range(13))
    rnd = "".join(random.choice(string.ascii_lowercase) for _ in range(18))
    return f"{rnd}-{agent_id} APP/com.xiaomi.mihome APPV/10.5.201"


def _gen_device_id() -> str:
    return "".join(random.choice(string.ascii_lowercase) for _ in range(6))


def _strip_mi_json(text: str) -> dict[str, Any]:
    return json.loads(text.replace("&&&START&&&", "", 1))


def load_session(session_file: Path = DEFAULT_SESSION_FILE) -> dict[str, Any]:
    if not session_file.is_file():
        raise FileNotFoundError(
            f"Keine Xiaomi-Session unter {session_file} gefunden. Bitte zuerst 'login' ausfuehren."
        )
    data = json.loads(session_file.read_text(encoding="utf-8"))
    for k in ("userId", "ssecurity", "serviceToken", "agent", "device_id"):
        if not data.get(k):
            raise ValueError(f"Unvollstaendige Session in {session_file}: Feld {k!r} fehlt")
    return data


def save_session(data: dict[str, Any], session_file: Path = DEFAULT_SESSION_FILE) -> None:
    session_file.parent.mkdir(parents=True, exist_ok=True)
    session_file.parent.chmod(0o700)
    session_file.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    session_file.chmod(0o600)


class XiaomiCloudClient:
    """Stdlib Mi-Cloud ENCRYPT-RC4 RPC client (zero external deps)."""

    def __init__(
        self,
        *,
        user_id: str | int,
        ssecurity: str,
        service_token: str,
        agent: str,
        device_id: str,
        server: str = DEFAULT_COUNTRY,
        api_base_url: str | None = None,
        timeout: float = 15.0,
    ) -> None:
        self.user_id = str(user_id)
        self.ssecurity = ssecurity
        self.service_token = service_token
        self.agent = agent
        self.device_id = device_id
        self.server = server
        self.api_base_url = api_base_url.rstrip("/") if api_base_url else self.default_api_base(server)
        self.timeout = timeout
        self._http = _StdSession()
        self._rpc_id = 1

    @staticmethod
    def default_api_base(server: str) -> str:
        prefix = "" if server == "cn" else f"{server}."
        return f"https://{prefix}api.io.mi.com/app"

    @classmethod
    def from_session_file(
        cls,
        session_file: Path = DEFAULT_SESSION_FILE,
        server: str | None = None,
        api_base_url: str | None = None,
    ) -> "XiaomiCloudClient":
        sess = load_session(session_file)
        srv = server or str(sess.get("server") or DEFAULT_COUNTRY)
        return cls(
            user_id=sess["userId"],
            ssecurity=sess["ssecurity"],
            service_token=sess["serviceToken"],
            agent=sess["agent"],
            device_id=sess["device_id"],
            server=srv,
            api_base_url=api_base_url,
        )

    def execute_encrypted(self, path: str, params: dict[str, str]) -> dict[str, Any]:
        url = f"{self.api_base_url}{path if path.startswith('/') else '/' + path}"
        nonce = generate_nonce()
        s_nonce = signed_nonce(self.ssecurity, nonce)
        enc_fields = generate_enc_params(url, "POST", s_nonce, nonce, params, self.ssecurity)
        headers = {
            "Accept-Encoding": "identity",
            "User-Agent": self.agent,
            "Content-Type": "application/x-www-form-urlencoded",
            "x-xiaomi-protocal-flag-cli": "PROTOCAL-HTTP2",
            "MIOT-ENCRYPT-ALGORITHM": "ENCRYPT-RC4",
        }
        cookies = {
            "userId": self.user_id,
            "yetAnotherServiceToken": self.service_token,
            "serviceToken": self.service_token,
            "locale": "de_DE",
            "timezone": "GMT+01:00",
            "is_daylight": "0",
            "dst_offset": "3600000",
            "channel": "MI_APP_STORE",
        }
        resp = self._http.request(
            "POST",
            url,
            params=enc_fields,
            headers=headers,
            cookies=cookies,
            allow_redirects=True,
            timeout=self.timeout,
        )
        if resp.status_code != 200:
            raise RuntimeError(f"Mi-Cloud HTTP {resp.status_code} auf {path}: {resp.text[:300]}")
        decoded = decrypt_rc4_bytes(s_nonce, resp.text.strip())
        return json.loads(decoded.decode("utf-8"))

    def get_bindkey(self) -> str:
        res = self.execute_encrypted("/user/get_bindkey", {"data": "{}"})
        result = res.get("result") or {}
        key = result.get("bindkey") or result.get("bind_key") or ""
        if not key:
            raise RuntimeError(f"get_bindkey unerwartete Antwort: {res!r}")
        return str(key)

    def find_device_by_ip(self, target_ip: str) -> dict[str, Any] | None:
        homes: list[tuple[Any, Any]] = []
        h = self.execute_encrypted(
            "/v2/homeroom/gethome",
            {"data": '{"fg":true,"fetch_share":true,"fetch_share_dev":true,"limit":300,"app_ver":7}'},
        )
        for item in (h.get("result") or {}).get("homelist") or []:
            homes.append((item["id"], self.user_id))
        cnt = self.execute_encrypted("/v2/user/get_device_cnt", {"data": '{"fetch_own":true,"fetch_share":true}'})
        for item in ((cnt.get("result") or {}).get("share") or {}).get("share_family") or []:
            homes.append((item["home_id"], item["home_owner"]))

        for home_id, owner_id in homes:
            payload = {
                "home_owner": owner_id,
                "home_id": home_id,
                "limit": 200,
                "get_split_device": True,
                "support_smart_home": True,
            }
            devs = self.execute_encrypted("/v2/home/home_device_list", {"data": json.dumps(payload)})
            for d in (devs.get("result") or {}).get("device_info") or []:
                if d.get("localip") == target_ip:
                    return d
        return None

    def rpc(self, did: str | int, method: str, params: Any = None) -> Any:
        dec_did = normalize_did(did)
        req_id = self._rpc_id
        self._rpc_id += 1
        rpc_obj = {
            "id": req_id,
            "method": method,
            "params": [] if params is None else params,
        }
        return self.execute_encrypted(f"/home/rpc/{dec_did}", {"data": json.dumps(rpc_obj, separators=(",", ":"))})


def interactive_login(
    *,
    username: str,
    password: str,
    server: str = DEFAULT_COUNTRY,
    session_file: Path = DEFAULT_SESSION_FILE,
    captcha_file: Path = DEFAULT_CAPTCHA_FILE,
) -> dict[str, Any]:
    """Authenticate a Xiaomi account locally using pure stdlib (supports captcha & email 2FA)."""
    http_sess = _StdSession()
    agent = _gen_agent()
    device_id = _gen_device_id()
    for dom in ("mi.com", "xiaomi.com"):
        http_sess.set_cookie("sdkVersion", "accountsdk-18.8.15", dom)
        http_sess.set_cookie("deviceId", device_id, dom)
        http_sess.set_cookie("userId", username, dom)

    headers = {
        "User-Agent": agent,
        "Content-Type": "application/x-www-form-urlencoded",
    }

    r1 = http_sess.request(
        "GET",
        "https://account.xiaomi.com/pass/serviceLogin?sid=xiaomiio&_json=true",
        headers=headers,
    )
    j1 = _strip_mi_json(r1.text)
    sign = j1.get("_sign")
    ssecurity = j1.get("ssecurity")
    user_id = j1.get("userId")
    location = j1.get("location")
    service_token: str | None = None

    if not ssecurity:
        if not sign:
            raise RuntimeError(f"serviceLogin fehlgeschlagen: {j1!r}")
        fields = {
            "sid": "xiaomiio",
            "hash": hashlib.md5(password.encode("utf-8")).hexdigest().upper(),
            "callback": "https://sts.api.io.mi.com/sts",
            "qs": "%3Fsid%3Dxiaomiio%26_json%3Dtrue",
            "user": username,
            "_sign": sign,
            "_json": "true",
        }
        r2 = http_sess.request(
            "POST",
            "https://account.xiaomi.com/pass/serviceLoginAuth2",
            params=fields,
            headers=headers,
            allow_redirects=False,
        )
        j2 = _strip_mi_json(r2.text)

        if j2.get("captchaUrl"):
            cap_url = str(j2["captchaUrl"])
            if cap_url.startswith("/"):
                cap_url = "https://account.xiaomi.com" + cap_url
            cap_resp = http_sess.request("GET", cap_url, headers={"User-Agent": agent})
            captcha_file.parent.mkdir(parents=True, exist_ok=True)
            captcha_file.write_bytes(cap_resp.content)
            print(f"\nCaptcha-Bild gespeichert unter: {captcha_file}", flush=True)
            fields["captCode"] = input("Captcha eingeben (Gross-/Kleinschreibung beachten): ").strip()
            r2 = http_sess.request(
                "POST",
                "https://account.xiaomi.com/pass/serviceLoginAuth2",
                params=fields,
                headers=headers,
                allow_redirects=False,
            )
            j2 = _strip_mi_json(r2.text)
            if j2.get("code") == 87001:
                raise RuntimeError("Ungueltiges Captcha.")

        if j2.get("ssecurity") and len(str(j2["ssecurity"])) > 4:
            ssecurity = j2["ssecurity"]
            user_id = j2.get("userId")
            location = j2.get("location")
        elif j2.get("notificationUrl"):
            notif_url = str(j2["notificationUrl"])
            r_start = http_sess.request("GET", notif_url, headers=headers)
            ctx = urllib.parse.parse_qs(urllib.parse.urlparse(r_start.url).query).get("context") or \
                  urllib.parse.parse_qs(urllib.parse.urlparse(notif_url).query).get("context")
            if not ctx:
                raise RuntimeError("2FA context nicht in notificationUrl gefunden")
            context = ctx[0]

            http_sess.request(
                "GET",
                "https://account.xiaomi.com/identity/list",
                params={"sid": "xiaomiio", "context": context, "_locale": "en_US"},
                headers=headers,
            )
            http_sess.request(
                "POST",
                "https://account.xiaomi.com/identity/auth/sendEmailTicket",
                params={"_dc": str(int(time.time() * 1000)), "sid": "xiaomiio", "context": context, "mask": "0", "_locale": "en_US"},
                data={"retry": "0", "icode": "", "_json": "true", "ick": http_sess.get_cookie("ick") or ""},
                headers=headers,
            )
            code = input(f"\nE-Mail-2FA-Code fuer {username}: ").strip()
            r_ver = http_sess.request(
                "POST",
                "https://account.xiaomi.com/identity/auth/verifyEmail",
                params={"_flag": "8", "_json": "true", "sid": "xiaomiio", "context": context, "mask": "0", "_locale": "en_US"},
                data={"_flag": "8", "ticket": code, "trust": "false", "_json": "true", "ick": http_sess.get_cookie("ick") or ""},
                headers=headers,
            )
            if r_ver.status_code != 200:
                raise RuntimeError(f"verifyEmail HTTP {r_ver.status_code}: {r_ver.text[:250]}")

            finish_loc = None
            try:
                finish_loc = r_ver.json().get("location")
            except Exception:
                finish_loc = r_ver.headers.get("Location")
            if not finish_loc:
                r_chk = http_sess.request(
                    "GET",
                    "https://account.xiaomi.com/identity/result/check",
                    params={"sid": "xiaomiio", "context": context, "_locale": "en_US"},
                    headers=headers,
                    allow_redirects=False,
                )
                finish_loc = r_chk.headers.get("Location")

            end_url = finish_loc
            if finish_loc and "identity/result/check" in finish_loc:
                r_chk2 = http_sess.request("GET", finish_loc, headers=headers, allow_redirects=False)
                end_url = r_chk2.headers.get("Location")
            if not end_url:
                raise RuntimeError("Konnte Auth2/end URL nach verifyEmail nicht bestimmen")

            r_end = http_sess.request("GET", end_url, headers=headers, allow_redirects=False)
            if r_end.status_code == 200 and "Xiaomi Account - Tips" in r_end.text:
                r_end = http_sess.request("GET", end_url, headers=headers, allow_redirects=False)

            ext_prag = r_end.headers.get("extension-pragma")
            if ext_prag:
                ep_json = json.loads(ext_prag)
                ssecurity = ep_json.get("ssecurity")

            sts_url = r_end.headers.get("Location")
            if not sts_url:
                m = re.search(r"https://sts\.api\.io\.mi\.com/sts[^\"'\s]+", r_end.text)
                if m:
                    sts_url = m.group(0)
            if not sts_url or not ssecurity:
                raise RuntimeError("2FA abgeschlossen, aber ssecurity oder STS-Redirect fehlte")

            r_sts = http_sess.request("GET", sts_url, headers=headers, allow_redirects=True)
            if r_sts.status_code != 200:
                raise RuntimeError(f"STS-Step fehlgeschlagen ({r_sts.status_code})")
            service_token = http_sess.get_cookie("serviceToken")
            user_id = http_sess.get_cookie("userId") or user_id
        else:
            raise RuntimeError(f"Login fehlgeschlagen: {j2!r}")

    if location and not service_token:
        r3 = http_sess.request("GET", location, headers=headers, allow_redirects=True)
        if r3.status_code != 200:
            raise RuntimeError(f"Step-3 serviceToken fehlgeschlagen ({r3.status_code})")
        service_token = http_sess.get_cookie("serviceToken")
        user_id = http_sess.get_cookie("userId") or user_id

    if not (user_id and ssecurity and service_token):
        raise RuntimeError("Session unvollstaendig nach Login (userId/ssecurity/serviceToken fehlt)")

    session_data = {
        "username": username,
        "server": server,
        "userId": str(user_id),
        "ssecurity": str(ssecurity),
        "serviceToken": str(service_token),
        "agent": agent,
        "device_id": device_id,
    }
    save_session(session_data, session_file)
    return session_data


# ---------------------------------------------------------------------------
# Subcommand Workflows: pair-ap & rpc-ota
# ---------------------------------------------------------------------------

def run_pair_ap(
    *,
    ap_ip: str = "192.168.10.1",
    port: int = af.MIIO_PORT,
    wifi_ssid: str,
    wifi_psk: str,
    uid: int | None = None,
    bind_key: str | None = None,
    no_bind_key: bool = False,
    tz: str = DEFAULT_TZ,
    country_domain: str = DEFAULT_COUNTRY,
    allow_board_override: bool = False,
    session_file: Path = DEFAULT_SESSION_FILE,
    api_base_url: str | None = None,
    timeout: float = 4.0,
) -> dict[str, Any]:
    """Configure a bulb in Setup-AP mode (zero app) with uid, optional bind_key & cfg_lan_ctrl=1."""
    hello = af.miio_hello(ap_ip, port=port, timeout=timeout)
    if not hello or not hello["token_readable"]:
        raise RuntimeError(f"Kein Setup-AP-Token von {ap_ip}:{port}: {hello!r}")

    client = af.MiioClient(ap_ip, hello["token_bytes"], port=port, timeout=timeout)
    info = client.send("miIO.info", [])
    if not isinstance(info, dict):
        raise RuntimeError(f"Unerwartete miIO.info-Antwort: {info!r}")

    cls = af.classify_bulb(info, allow_board_override=allow_board_override)
    if not cls["flash_allowed"]:
        raise RuntimeError(f"Hardware-Guard ({cls['category']}): {cls['reason']}")

    resolved_uid = uid
    resolved_bind_key = bind_key
    if resolved_uid is None or (not no_bind_key and resolved_bind_key is None):
        if session_file.is_file():
            cloud = XiaomiCloudClient.from_session_file(session_file, server=country_domain, api_base_url=api_base_url)
            if resolved_uid is None:
                resolved_uid = int(cloud.user_id)
            if not no_bind_key and resolved_bind_key is None:
                resolved_bind_key = cloud.get_bindkey()

    if resolved_uid is None:
        resolved_uid = 0

    lan_res = client.send("set_ps", ["cfg_lan_ctrl", "1"])

    cfg_params: dict[str, Any] = {
        "ssid": wifi_ssid,
        "passwd": wifi_psk,
        "uid": int(resolved_uid),
        "country_domain": country_domain[:2],
        "tz": tz,
    }
    if not no_bind_key and resolved_bind_key:
        cfg_params["bind_key"] = resolved_bind_key

    try:
        cfg_res = client.send("miIO.config_router", cfg_params)
    except TimeoutError:
        cfg_res = "timeout_switching_to_sta"

    return {
        "did_hex8": hello["did_hex8"],
        "did_decimal": normalize_did(hello["did_hex8"]),
        "ap_token": hello["token_hex"],
        "classification": cls,
        "uid": int(resolved_uid),
        "used_bind_key": bool(cfg_params.get("bind_key")),
        "lan_ctrl_res": lan_res,
        "config_router_res": cfg_res,
    }


def run_rpc_ota(
    *,
    did: str | None = None,
    ip: str | None = None,
    server: str = DEFAULT_COUNTRY,
    state_only: bool = False,
    dry_run_url: str | None = None,
    live_url: str | None = None,
    md5: str | None = None,
    confirm_write: bool = False,
    poll: int = 20,
    poll_interval: float = 3.0,
    session_file: Path = DEFAULT_SESSION_FILE,
    api_base_url: str | None = None,
) -> dict[str, Any]:
    """Relay miIO.ota / state queries over a single jailed Mi-Cloud RPC call."""
    if not did and not ip:
        raise ValueError("Entweder --did oder --ip angeben")

    cloud = XiaomiCloudClient.from_session_file(session_file, server=server, api_base_url=api_base_url)
    target_did = normalize_did(did) if did else None
    if not target_did and ip:
        dev = cloud.find_device_by_ip(ip)
        if not dev or not dev.get("did"):
            raise RuntimeError(f"Kein Geraet mit IP {ip} im Konto auf Server {server!r} gefunden")
        target_did = str(dev["did"])

    assert target_did is not None
    state_before = cloud.rpc(target_did, "miIO.get_ota_state", [])
    prog_before = cloud.rpc(target_did, "miIO.get_ota_progress", [])
    out: dict[str, Any] = {
        "did": target_did,
        "state_before": state_before,
        "progress_before": prog_before,
    }
    if state_only:
        out["status"] = "STATE_OK"
        return out

    if dry_run_url:
        ack = cloud.rpc(target_did, "miIO.ota", {"app_url": dry_run_url})
        out["dry_run_ack"] = ack
        final_dry = None
        for _ in range(max(1, poll)):
            st = cloud.rpc(target_did, "miIO.get_ota_state", [])
            final_dry = st
            if st.get("result") in (["idle"], "idle"):
                break
            time.sleep(poll_interval)
        out["dry_run_final_state"] = final_dry
        if not confirm_write or not live_url:
            out["status"] = "DRY_RUN_RPC_DONE"
            return out

    if live_url:
        if not confirm_write:
            raise RuntimeError("Live-OTA benoetigt explizites --confirm-write (nach erfolgreichem 404-Dry-Run)")
        live_ack = cloud.rpc(target_did, "miIO.ota", {"app_url": live_url})
        out["live_ack"] = live_ack
        retried_full = False
        final_live = None
        for idx in range(max(1, poll)):
            st = cloud.rpc(target_did, "miIO.get_ota_state", [])
            pr = cloud.rpc(target_did, "miIO.get_ota_progress", [])
            final_live = {"state": st, "progress": pr}
            res_val = st.get("result")
            if res_val in (["installed"], "installed", ["failed"], "failed"):
                break
            if idx >= 1 and res_val in (["idle"], "idle") and md5 and not retried_full:
                retried_full = True
                out["live_retry_full_ack"] = cloud.rpc(
                    target_did,
                    "miIO.ota",
                    {"mode": "normal", "install": "1", "app_url": live_url, "file_md5": md5, "proc": "dnld install"},
                )
            time.sleep(poll_interval)
        out["live_final"] = final_live
        out["status"] = "LIVE_RPC_DONE"
        return out

    out["status"] = "NO_ACTION"
    return out


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def cmd_login(args: argparse.Namespace) -> int:
    user = (args.username or input("Xiaomi Account (E-Mail): ")).strip()
    pw = getpass.getpass(f"Xiaomi Passwort fuer {user} (bleibt lokal): ")
    sess = interactive_login(
        username=user,
        password=pw,
        server=args.server,
        session_file=Path(args.session_file),
        captcha_file=Path(args.captcha_file),
    )
    print(f"Login OK: userId={sess['userId']} server={sess['server']} -> {args.session_file}")
    try:
        cloud = XiaomiCloudClient.from_session_file(Path(args.session_file), server=args.server)
        bk = cloud.get_bindkey()
        print(f"Test bind_key abgerufen ({len(bk)} Zeichen): {bk[:6]}...")
    except Exception as exc:
        print(f"Hinweis get_bindkey: {exc}")
    return 0


def cmd_pair_ap(args: argparse.Namespace) -> int:
    psk = args.wifi_psk if args.wifi_psk is not None else getpass.getpass(f"WLAN-Passwort fuer '{args.wifi_ssid}': ")
    res = run_pair_ap(
        ap_ip=args.ap_ip,
        port=args.port,
        wifi_ssid=args.wifi_ssid,
        wifi_psk=psk,
        uid=args.uid,
        bind_key=args.bind_key,
        no_bind_key=args.no_bind_key,
        tz=args.tz,
        country_domain=args.country_domain,
        allow_board_override=args.allow_board_override,
        session_file=Path(args.session_file),
    )
    print(json.dumps(res, indent=2))
    return 0


def cmd_rpc_ota(args: argparse.Namespace) -> int:
    res = run_rpc_ota(
        did=args.did,
        ip=args.ip,
        server=args.server,
        state_only=args.state,
        dry_run_url=args.dry_run_url,
        live_url=args.live_url,
        md5=args.md5,
        confirm_write=args.confirm_write,
        poll=args.poll,
        poll_interval=args.poll_interval,
        session_file=Path(args.session_file),
    )
    print(json.dumps(res, indent=2))
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Zero-dependency throwaway Xiaomi login, AP binder & jailed RPC OTA")
    sub = p.add_subparsers(dest="cmd", required=True)

    pl = sub.add_parser("login", help="Interactive local Xiaomi login with captcha & email-2FA")
    pl.add_argument("--username", help="Xiaomi email / userId")
    pl.add_argument("--server", default=DEFAULT_COUNTRY, choices=["cn", "de", "us", "ru", "tw", "sg", "in", "i2"])
    pl.add_argument("--session-file", default=str(DEFAULT_SESSION_FILE))
    pl.add_argument("--captcha-file", default=str(DEFAULT_CAPTCHA_FILE))
    pl.set_defaults(func=cmd_login)

    pp = sub.add_parser("pair-ap", help="Couple bulb Setup-AP to home Wi-Fi with uid/bind_key (zero app)")
    pp.add_argument("--ap-ip", default="192.168.10.1")
    pp.add_argument("--port", type=int, default=af.MIIO_PORT)
    pp.add_argument("--wifi-ssid", required=True)
    pp.add_argument("--wifi-psk", default=None)
    pp.add_argument("--uid", type=int, default=None)
    pp.add_argument("--bind-key", default=None)
    pp.add_argument("--no-bind-key", action="store_true")
    pp.add_argument("--tz", default=DEFAULT_TZ)
    pp.add_argument("--country-domain", default=DEFAULT_COUNTRY)
    pp.add_argument("--allow-board-override", action="store_true")
    pp.add_argument("--session-file", default=str(DEFAULT_SESSION_FILE))
    pp.set_defaults(func=cmd_pair_ap)

    pr = sub.add_parser("rpc-ota", help="Send jailed miIO.ota RPC pointing to Mordor LAN HTTP :80")
    pr.add_argument("--did", help="Device ID (hex like 05eb8034 or decimal like 99319860)")
    pr.add_argument("--ip", help="Device LAN IP (used for account device lookup when --did omitted)")
    pr.add_argument("--server", default=DEFAULT_COUNTRY)
    pr.add_argument("--state", action="store_true", help="Only query get_ota_state & get_ota_progress")
    pr.add_argument("--dry-run-url", help="Non-destructive 404 URL (e.g. http://10.10.10.13/dryrun_404.bin)")
    pr.add_argument("--live-url", help="Live firmware URL (e.g. http://10.10.10.13/mcufw_MRVL.bin)")
    pr.add_argument("--md5", help="Firmware MD5 hex for fallback payload")
    pr.add_argument("--confirm-write", action="store_true")
    pr.add_argument("--poll", type=int, default=20)
    pr.add_argument("--poll-interval", type=float, default=3.0)
    pr.add_argument("--session-file", default=str(DEFAULT_SESSION_FILE))
    pr.set_defaults(func=cmd_rpc_ota)
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
