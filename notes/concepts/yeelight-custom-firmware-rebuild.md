# Yeelight Custom Firmware Rebuild — Single Source of Truth

> **Status:** Phase 3.5b abgeschlossen (2026-11-18)
> **Repo:** `Shamane2000/yeelight-re` · Branch `main`
> **Build-Host:** devastator (`192.168.1.154`, `/home/jarvis/projects/yeelight-re`)

---

## §1 Ziel
Vollständiger, deterministischer Neuaufbau der Yeelight color1-Firmware (MW300/88MW302) mit eigenem MCU-Stack (`mcufw`), der das Original-Binärformat (`upd_yeelink.light.color1.bin`) byte-kompatibel ersetzt.

## §2 Hardware- & SDK-Basis
- **MCU:** Marvell MW300 (ARM Cortex-M4F, Thumb-2)
- **SDK:** Marvell EZ-Connect SDK v3.4 (lwIP 1.4.1, FreeRTOS)
- **Flash-Layout:** boot2 @ 0x00100000, App-XIP @ 0x1f0032d8, RAM-DATA @ 0x20000040
- **Container-Format:** Magic `0x2e9cf17b`, 3 Segmente, CRC-32 (reflected, init 0), Entry-Point mit Thumb-Bit

## §3 Build-Pipeline
```
make -C sdk              # Full-Build → mcufw_MRVL.bin + Verifier
make -C sdk/apps/mcufw host-test   # Host-Tests (gcc, nativ)
```
- **Determinismus:** ELF ist byte-identisch über Clean-Builds (SHA256 `d3a0c71f…`). Container-BIN variiert nur im Timestamp (`time(NULL)` in `axf2firmware`).
- **nm -u:** 0 undefinierte Symbole (alle Abhängigkeiten im Linker-Skript aufgelöst).

## §4 Phase-3.5b — Netzwerk-Runtime (verifiziert 2026-11-18)

### 4.1 miIO UDP 54321 (`mcufw_miio.c`)
- **Header:** 32 Byte, Little-Endian, Magic `0x2131`
- **Hello-Handshake:** 32-Byte-Datagramm → Reply mit Device-Token (ungepaart: `ff…ff`)
- **AES-Session:** Key = MD5(token), IV = MD5(key || token), CBC-Padding
- **Dispatch:** `hello`, `miIO.ota` (Stub-OK), `miIO.info` (Modell/FW-Version), unbekannte Methoden → JSON-RPC Error `-32601`
- **Replay-Guard:** Stale Hellos (Stamp ≤ letzter) werden verworfen

### 4.2 LAN JSON-RPC TCP 55443 (`mcufw_lan_rpc.c`)
- **Whitelist (exakt `color1_0061`):**
  - **Erlaubt:** `set_power`, `toggle`, `set_bright`, `set_ct_abx`, `set_hsv`, `set_name`, `get_prop`, `start_cf`, `cron_add|del|get`, `set_default`
  - **Blockiert (code:2):** `set_ps`, `get_ps`, `cfg_lan_ctrl`, `ota`
- **Dispatch-Skeleton:** Valide Methoden → Stub-OK; blockierte → `{"error":{"code":2,"message":"method not supported"}}`

### 4.3 MQTT Payload-Builder (`mcufw_mqtt.c`)
- **Discovery-Topic:** `homeassistant/light/<did>/config`
- **State/Command-Topics:** `yeelight/<did>/state`, `yeelight/<did>/set`
- **Payload-Felder:** `brightness:true`, `color_temp:true`, `unique_id:"yeelight_<did>"`, Power/Bright/CT-Serialisierung

### 4.4 Color Flow (`mcufw_cf.c`)
- **Parser:** Action/Repeat/Expression-Tupel aus JSON-Array
- **Start/Stop:** Validierung Action ∈ {0,1,2}, Reject bei ungültigen Werten

### 4.5 Host-Test-Ergebnisse
```
=== TOTAL: 38 PASS, 0 FAIL ===
```
- Crypto + miIO Selftest: 13/13
- RPC-Whitelist: 13/13 (4 blocked + 9 allowed)
- RPC-Dispatch: 3/3
- MQTT-Payloads: 5/5
- Color Flow: 4/4

### 4.6 Vollverifikation
- `make -C sdk` → **41/41 PASS** (Verifier)
- `nm -u mcufw.elf` → **0** undefinierte Symbole
- ELF-SHA256: `d3a0c71f910ac9a01fbe96b5d89c8a5eb95193b97788646524168aa2fa7bc83c` (2 Clean-Builds identisch)
- Commit: `0a644ac` (Phase 3.5a) → wird durch Phase-3.5b-Commit aktualisiert

## §5 Offene Punkte / Folgephasen

| Phase | Inhalt | Status |
|-------|--------|--------|
| 3.6 | Lamp-State-Mutation (GPIO/PWM-Backend) | offen |
| 4 | Persistent Config (Flash-Read/Write) | offen |
| 5 | Echter AP-Join + MQTT-Broker-Connect (Flash-Gate) | offen |
| 6 | OTA-Download (app_url/file_md5/proc=dnld) | offen |

**Hinweis:** MQTT-Connect und AP-Join sind **nicht** Teil von Phase 3.5 — hier nur reine Serialisierungslogik (host-testbar). Echte Netzwerkanbindung erfordert Flash-Zugriff und ist erst ab Phase 5 freigegeben.

## §6 Referenzen
- `docs/06_ota_delivery_path.md` — miIO-OTA-Protokoll
- `docs/10_wlan_miio.md` — WLAN/miIO-Integration
- `docs/03_research_lan_control.md` — LAN-Control-Spec
- `firmware/color1_142_0061/upd_yeelink.light.color1.bin` — Referenz-Firmware
