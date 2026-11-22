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

## 4. Zero-Jarvis-Architektur & OPNsense/AdGuard Firewall-Käfig (für `2.0.6_0065`)

Gemäß Homelab-Regelwerk (`hermes-homelab` §3) bleibt **Jarvis (`192.168.1.196`) komplett unberührt** (kein OTA-Server, keine Caddy-Änderung, keine Wegwerf-Skripte oder Xiaomi-Credentials):

| Rolle | Host | Aufgabe |
| :--- | :--- | :--- |
| **Lokaler Steuer-Laptop** | `Borg-Zorin` (`192.168.1.187`, `~/Projekte/Antigravity/yeelight-flash/`) | `jailed_cloud_ota.py login/pair-ap/rpc-ota` mit Wegwerf-Account (`bulbblub@ji5.de`) — Zugangsdaten verlassen nie den Laptop |
| **HTTP/1.1 Port-80 OTA** | `Mordor` (`10.10.10.13:80`, ephemeres `docker run --rm`) | Serviert `404`-Dry-Run und anschließend einmalig `mcufw_MRVL.bin` über `http://10.10.10.13/...` (ohne `:port`) |
| **Firewall-Käfig & Sniffer** | `OPNsense` (`192.168.1.1`, AdGuard Home + `tcpdump`) | Blockiert `*.mi-img.com`, `*.fds.api.mi-img.com`, `*.yeelight.com` hart; schneidet Vorher-/Nachher-Traffic von `.115` mit |

Ablauf (`docs` + `tools/jailed_cloud_ota.py`):
1. `python3 jailed_cloud_ota.py login --username bulbblub@ji5.de` (interaktiv am Laptop).
2. Optional Setup-AP: `python3 jailed_cloud_ota.py pair-ap --wifi-ssid <WLAN> --wifi-psk <PSK>` (setzt `cfg_lan_ctrl=1` + `uid` + `bind_key` ohne Handy-App).
3. OPNsense-CDN-Block + Mordor Port-80 `404`-Container aktivieren, `rpc-ota --did 05eb8034 --dry-run-url http://10.10.10.13/dryrun_404.bin` prüfen.
4. Auf Mordor Live-Image umschalten → `rpc-ota --did 05eb8034 --live-url http://10.10.10.13/mcufw_MRVL.bin --md5 4f355acb69d0a73e09b8b26e696176be --confirm-write`.
5. Sobald `mcufw` bootet: Vorher-/Nachher-Capture auf OPNsense zeigt `0` ausgehende Cloud-Pakete; Container & Käfig-Ausnahme werden entfernt.
