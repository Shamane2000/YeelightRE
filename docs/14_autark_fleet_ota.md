# 14 — Autarke Flotten-OTA & Kryptographischer `TAG_KEY`-Befund (Option B)

**Datum:** 22.11.2026  
**Anlass:** Strategieentscheidung „Option B" (so autark wie möglich über einen gemischten Lampen-Schwung, ohne Yeelight-/Mi-App und ohne unkontrollierte Xiaomi-Cloud-Aktionen).  
**Werkzeug:** [`tools/autark_flash.py`](../tools/autark_flash.py) · **Unit-Tests:** [`tests/test_autark_flash.py`](../tests/test_autark_flash.py)

---

## 1. Harte Krypto-Grenze: `TAG_KEY` vs. `ot_token` (Warum Fake-`ot.io.mi.com` nicht signieren kann)

Die Ghidra-Dekompilierung von `color1_142_0061` (`export3/decompiled/ALL.c`) und `color2_142_0027` (`export_color2/ALL.c`) beweist in `FUN_1f00900c` → `FUN_1f03c090`:

1. **`ot_token` (`&DAT_20001ca8`):** 16-Byte-Session-Token, wird im Setup-AP durch `miIO.hello` im Klartext ausgegeben und in `*(param_1 + 0x100) + 0x14` **ausschließlich** für den lokalen UDP-54321-Pfad verwendet.
2. **`TAG_KEY` (`device.key`):** 16-Byte-Werksschlüssel aus OTP/Flash (`FUN_1f055718(buf, 0x18)` zusammen mit der 8-Byte-`TAG_DID`). Wird **niemals** per `miIO.hello` gesendet.
   - UDP `ot.io.mi.com:8053` (`FUN_1f03f1f8`, Offsets `param_1 + 0x24 / +0x34 / +0x44`)
   - TCP `ott.io.mi.com:80` (`FUN_1f03d154`, Offsets `param_1 + 0x412 / +0x422 / +0x432`)
   - Jedes Downlink-Paket vom Cloud-OT-Server wird in `FUN_1f03cb28` mit `MD5(hdr[0..15] || TAG_KEY || body)` geprüft und mit AES-128-CBC aus `TAG_KEY` entschlüsselt.

**Konsequenz:** Solange das Gehäuse ungeöffnet bleibt (kein SPI-Flash-/OTP-Dump), kennt ein lokaler Mock-/Fake-Server `ot.io.mi.com` das `TAG_KEY` der Birne nicht und kann keine gültigen `miIO.ota`-Pakete signieren (`docs/12_sta_ota_and_brick_audit.md` §2 entsprechend korrigiert).

---

## 2. Drei-Klassen-Flottenmatrix (App-frei & geschützt)

| Klasse | Erkennung (`miIO.info`) | Initial-OTA-Pfad | Tool-Verhalten in `autark_flash.py` |
| :--- | :--- | :--- | :--- |
| **1. `offline_direct`** | `hw_ver` ∈ `{mw300, mw302}`, `model == yeelink.light.color2`, `fw_ver < 2.0.6` (z. B. `1.4.2_xxxx`), `miio_ver < 0.0.3` | **100 % offline über UDP 54321**: `ot_token` überlebt `miIO.config_router` (`FUN_1f008e10(0)`) und `ota_JsDlgt` (`0x1f009802`) akzeptiert `miIO.ota` bei `sender_uid == 0`. **0 % Cloud, 0 % App.** | `flash-local` erlaubt (`--dry-run-only` → `--confirm-write`) |
| **2. `firewall_rpc_once`** | `hw_ver` ∈ `{mw300, mw302}`, `model == yeelink.light.color2`, `fw_ver >= 2.0.6` oder `miio_ver >= 0.0.3` (z. B. `.115` mit `2.0.6_0065`) | Stock verwirft LAN-Token beim AP→STA-Wechsel und blockt lokales UDP 54321 (`-30020`). Einmaliger CLI-Trigger im **OPNsense/AdGuard Firewall-Käfig** (kein App-Zwang), danach für immer 100 % offline auf `mcufw`. | `flash-local` bricht vor `miIO.ota` mit Kanal-Guard ab |
| **3a. `needs_board_profile`** | Marvell-Modell ≠ `color2` (`color1` `YLDP02YL`, `ct2`, `mono1`, `ceiling1`) | Abhängig von Firmwarestand; erfordert zuerst Pin-/Treiber-Profil statt `color2` (`GPIO 0/1/4/5 + GPIO 44`). | Hard-Block ohne `--allow-board-override` |
| **3b. `unsupported_soc`** | Nicht-Marvell (`hw_ver` ∉ `{mw300, mw302}` oder unbekanntes Modell, z. B. `yeelink.light.color4` / `YLDP13YL` = ESP32/Realtek) | **Kein `mcufw_MRVL.bin`!** Separate Plattform (z. B. ESPHome). | **Permanenter Hard-Block** vor jedem Flash |

---

## 3. Nutzung `tools/autark_flash.py`

Pure Python 3 Stdlib (keine `pip`-Abhängigkeiten). Bei jedem Flash wird **vorab** das Image auf `MRVL` + Segment-CRC-32 + `<= 384 KB` geprüft und zuerst ein nicht-destruktiver `HTTP/1.1` `404`-Dry-Run erzwungen:

```bash
# 1. Gebautes MRVL-Image prüfen
python3 tools/autark_flash.py verify-image sdk/apps/mcufw/build/mcufw_MRVL.bin

# 2. Gemischten Lampen-Schwung im Setup-AP ohne Heim-WLAN/Cloud inventarisieren
python3 tools/autark_flash.py inventory

# 3. 100% Offline-Flash (nur Klasse 1, z. B. 1.4.2_xxxx) — erst 404-Dry-Run, danach explizit --confirm-write
python3 tools/autark_flash.py flash-local --ip <BULB_IP> --token <HEX32> \
  --firmware sdk/apps/mcufw/build/mcufw_MRVL.bin --dry-run-only

# 4. Zero-App-Onboarding nach dem Flash auf mcufw 0.8-phase4b.4 (HTTP :8080)
python3 tools/autark_flash.py provision-mcufw --url http://192.168.10.1:8080 \
  --wifi-ssid "<WLAN>" --wifi-psk "<PSK>" --mqtt-host 192.168.1.106 --mqtt-port 1883
```

---

## 4. OPNsense / AdGuard Firewall-Käfig (für Klasse-2-Birnen wie `2.0.6_0065`)

Damit eine `2.0.6_xxxx`-Birne (`.115`) während des einmaligen 30-Sekunden-Cloud-RPC-Impulses **garantiert keine** Xiaomi-Stock-Firmware oder fremde Cloud-Kommandos nachladen kann:

1. **AdGuard Home / OPNsense DNS-Block** für alle Stock-Firmware- & Vendor-CDNs:
   - `*.mi-img.com` (u. a. `cdn.awsde0.fds.api.mi-img.com`, `cdn.cnbj1.fds.api.mi-img.com`)
   - `*.fds.api.mi-img.com`
   - `*.yeelight.com`
2. **Temporäre OPNsense-Regel** nur für die Ziel-IP (`192.168.1.115`) während des 30-Sekunden-Fensters:
   - Erlaubt ausschließlich ausgehend UDP/TCP zu `de.ot.io.mi.com:8053` / `de.ott.io.mi.com:80` für den initialen RPC-Relay.
   - LAN HTTP/1.1 `:80` URL (ohne `:port`, Caddy auf `jarvis:80` -> `192.168.1.196`) liefert zuerst `404` im Trockentest und erst danach einmalig `mcufw_MRVL.bin`.
3. Sobald `mcufw` bootet, verbindet es sich nur noch lokal mit Mosquitto (`192.168.1.106:1883`) — die Firewall-Ausnahme wird wieder entfernt.
