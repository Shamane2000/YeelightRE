# Yeelight Custom Firmware Rebuild — Single Source of Truth

> **Status:** Phase 3.6 abgeschlossen (2026-11-18)
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
- **Determinismus:** ELF ist byte-identisch über Clean-Builds (SHA256 `72c2710a…`). Container-BIN variiert nur im Timestamp (`time(NULL)` in `axf2firmware`).
- **nm -u:** 0 undefinierte Symbole (alle Abhängigkeiten im Linker-Skript aufgelöst).

## §4 Phase-3.5b / 3.6 — Netzwerk-Runtime & Lamp-State (verifiziert 2026-11-18)

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

### 4.7 Phase-3.6 — Lamp-State-Mutation (`mcufw_state.c`, `mcufw_pwm.c`, verifiziert 2026-11-18)

**Ein State für beide Frontends.** `mcufw_state()` liefert den prozessglobalen
`mcufw_state_t`; LAN-RPC (TCP 55443) und miIO (UDP 54321) schreiben in
denselben Struct. Damit ist `toggle` kanalübergreifend konsistent und die
Hardware-Sicht entspricht immer dem State.

| Feld | Wertebereich | Default (Power-On) |
|------|--------------|--------------------|
| `power` | 0 / 1 | 0 (off) |
| `bright` | 1 .. 100 | 100 |
| `ct` | 1700 .. 6500 K | 4000 |
| `hue` / `sat` | 0 .. 359 / 0 .. 100 | 0 / 0 |
| `color_mode` | 2 = CT, 3 = HSV | 2 |
| `name` | max. 32 Zeichen, JSON-safe (`"`/`\` → `_`) | "" |
| `cf_active` / `is_default` | 0 / 1 | 0 / 0 |
| `mutations` | Zähler angewandter Änderungen | 0 |

**Clamping** in jedem Setter: `set_bright(150)` → 100, `set_bright(0)` → 1,
`set_ct_abx(1000)` → 1700, `set_ct_abx(9000)` → 6500, `set_hsv(h>359)` → 359,
`set_hsv(s>100)` → 100. `set_hsv` setzt `bright = v` (HSV-V), `set_ct_abx`
setzt `color_mode = 2`.

**PWM-Backend (`mcufw_pwm.c`)** — reine Abstraktion, **keine** GPT-Register-Writes
(die kommen mit Phase 4):
- `mcufw_pwm_set(bright, ct)` — Weißkanal; `bright == 0` ⇒ Mode OFF
- `mcufw_pwm_hsv(hue, sat, val)` — Farbkanal
- `mcufw_pwm_duty(bright)` — linear auf `MCUFW_PWM_PERIOD = 0xC350` (Vollausschlag,
  identisch zum Referenz-Counter), 50 % → 25000
- `mcufw_pwm_stats()` — letzter Mode/Bright/CT/Hue/Sat/Val, Aufrufzähler und
  `last_log` (z. B. `pwm ct bright=60 ct=1700 duty=30000`) für UART/Test

**LAN-RPC-Antworten (wire-kompatibel):** `result[0]` bleibt `"ok"`
(python-yeelight/HA prüfen nur dieses Element), der geklemmte Wert folgt als
`result[1]`:

| Methode | Antwort | State-Wirkung |
|---------|---------|---------------|
| `set_power ["on"\|"off"]` | `["ok",0\|1]` | power |
| `toggle` | `["ok",0\|1]` | power invertiert |
| `set_bright [n,…]` | `["ok",<bright>]` | bright (geklemmt) |
| `set_ct_abx [n,…]` | `["ok",<ct>]` | ct (geklemmt), mode=2 |
| `set_hsv [h,s,…]` | `["ok",h,s,<bright>]` | hue/sat/bright, mode=3 |
| `set_name ["x"]` | `["ok","x"]` | name (saniert) |
| `start_cf [n,action,expr]` | `["ok",0\|1]` | cf_active |
| `set_default` | `["ok"]` | is_default=1 |
| `get_prop ["power","bright","ct"]` | `["on",42,1700]` | liest State |

Ungültige Parameter → `{"error":{"code":3,"message":"invalid params"}}`.
`set_ps`/`get_ps` bleiben auf 55443 **code 2** — nur miIO darf sie.

**miIO (UDP 54321):** `set_ps ["on"|"off"]` → `["ok"]`, `get_ps` →
`[{power,bright,ct,hue,sat,color_mode,name}]`, `miIO.info` →
`{model, fw_ver:"0.6-phase3.6", power, bright, ct, hue, sat, color_mode, cf, name}`
(Live-State statt statischem String).

**Host-Test-Ergebnisse (`make -C sdk/apps/mcufw host-test`):**
```
=== TOTAL: 82 PASS, 0 FAIL ===
```
- Crypto + miIO Selftest: 13 · RPC-Whitelist: 13 · RPC-Dispatch: 3
- MQTT: 5 · Color Flow: 4
- **State-Model: 21** (Clamping, Toggle, Name-Truncate/Sanitize, CF/Default)
- **PWM-Backend: 10** (Duty 0/50/100, Mode CT/HSV/OFF, Aufrufzähler, Log)
- **RPC-State-Mutation: 8** (`["ok",100]`, `["ok",1700]`, Toggle, HSV, Name,
  start_cf, get_prop-Readback, code 3)
- **miIO-State-Mutation: 5** (set_ps on/off, get_ps, miIO.info, -32602)

**Vollverifikation:** `make -C sdk` → **41/41 PASS**, `nm -u mcufw.elf` → **0**,
ELF-SHA256 `72c2710a31f8679427e1cc3db0f625c9e08875d322df6c4a4de650d88b16aab1`
(2 Clean-Builds byte-identisch), Version `0.6-phase3.6`.
Zusätzliche nm-Guards im Verifier: `mcufw_state_set_power`, `mcufw_pwm_set`,
`mcufw_pwm_hsv`.
Commit: `6d5968c` (Code), dieser SSoT-Eintrag folgt als eigener `docs(phase3.6)`-Commit.

## §5 Offene Punkte / Folgephasen

| Phase | Inhalt | Status |
|-------|--------|--------|
| 3.6 | Lamp-State-Mutation (State-Model + PWM-Backend-Abstraktion) | **erledigt** (Commit `6d5968c`) |
| 4 | Echte PWM/GPT-Register-Writes + Persistent Config (Flash R/W) | offen |
| 5 | Echter AP-Join + MQTT-Broker-Connect (Flash-Gate) | offen |
| 6 | OTA-Download (app_url/file_md5/proc=dnld) | offen |

**Hinweis:** `mcufw_pwm_*` ist bewusst nur die Abstraktionsschicht — es wird in
Phase 3.6 **kein** GPT-Register geschrieben (Host-Test und Target teilen sich
denselben Code). Das Verdrahten auf `mcufw_lamp_set()` / GPT0-CMR0 pro Kanal
ist Teil von Phase 4. MQTT-Connect und AP-Join bleiben Phase 5.

## §6 Referenzen
- `docs/06_ota_delivery_path.md` — miIO-OTA-Protokoll
- `docs/10_wlan_miio.md` — WLAN/miIO-Integration
- `docs/03_research_lan_control.md` — LAN-Control-Spec
- `firmware/color1_142_0061/upd_yeelink.light.color1.bin` — Referenz-Firmware
