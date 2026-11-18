# Yeelight Custom Firmware Rebuild — Single Source of Truth

> **Status:** Phase 3.7 abgeschlossen (2026-11-18)
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

### 4.8 Phase-3.7 — `mcufw_pwm` auf echte GPT0-Kanäle (Register-Seam, verifiziert 2026-11-18)

**Register-Seam (ein benannter HAL-Baustein, zwei Link-Time-Backends).**
`mcufw_pwm_hal_write()` / `_read()` / `_name()` sind die einzige Stelle, an der
GPT0 angefasst wird; der Aufrufer enthält **kein** `#ifdef`:

| Backend | Datei | Mechanik |
|---|---|---|
| Target | `src/mcufw_pwm_hal_target.c` | `REG32(GPT0_BASE + off)` (`GPT0_BASE` 0x46070000) |
| Host | `src/mcufw_pwm_hal_host.c` | Wort-Array, 256 Words = 1 KiB (deckt 0x000..0x3FF, letzter Kanal CH5 endet 0x310) |

Der Stub führt zusätzlich zwei Invarianten: `bad_offset` (Zugriff außerhalb des
Fensters/unaligned) und Latch-Konsistenz (`latched_mask`/`latch_errors`).
Das Firmware-Image enthält **kein** `mcufw_pwm_hal_stub_*`-Symbol (nm-Guard im
Verifier) — der Stub kann nicht versehentlich mitgelinkt werden.
`mcufw_gpt.c` (Phase 3.3) schreibt seit 3.7 ebenfalls über diesen Seam.

**Kanal-Mapping + Register-Arithmetik** (aus 3.3 unverändert: edge-aligned PWM,
invertierte Polarität, Periode `0xC350`, also `CMR0 = 0xC350 − level`):

| Befehl | Level | CMR0 geschrieben | Kanäle |
|---|---|---|---|
| `mcufw_pwm_set(100, ct)` | 0xC350 | `0x0000` | CH0, CH1 (Weiß-Paar); CH4/CH5 dunkel (`0xC350`) |
| `mcufw_pwm_set(50, ct)` | 0x61A8 | `0x61A8` | CH0, CH1 |
| `mcufw_pwm_set(1, ct)` | 0x01F4 | `0xC15C` | CH0, CH1 |
| `mcufw_pwm_hsv(h, s, 80)` | 0x9C40 | `0x2710` | CH4, CH5 (Farb-Paar); Weiß-Paar dunkel |
| `mcufw_pwm_set(0, ct)` (off) | 0 | `0xC350` | alle vier Kanäle |

**Rundung: keine.** `0xC350 / 100 = 500` ist ganzzahlig, deshalb landet jedes
ganze Prozent exakt auf einem Count (1 % → 0x01F4, 50 % → 0x61A8). Der in der
Ticket-Beschreibung vermutete Wert `0xC347` für 1 % reproduziert nicht und ist
damit widerlegt (er entspräche ~0x0129 Counts) — der Test nagelt `0xC15C` fest.

**Latch.** Auf jeden CMR0-Write folgt `USER_REQ |= CHn_CMR_UPDT(ch)`, der neue
Match-Wert wird erst zur nächsten Zählerperiode wirksam (kein Tearing). Der
Host-Test prüft das als Invariante über alle vier Kanäle (Maske `0x33`) statt
nur „letzter Write".

**Power-Restore.** `set_power(0)` fährt alle vier Kanäle auf Level 0
(CMR0 = 0xC350) und merkt sich den letzten nicht-dunklen Befehl in
`restore_mode/restore_bright/restore_white/restore_color`; `set_power(1)` ruft
`mcufw_pwm_restore()` und stellt exakt dieselben Duty-Wörter wieder her
(bright 50 % → 0x61A8, hsv v=80 → 0x2710).

**Weiterhin Annahme, kein Beweis:** (a) die Aufteilung der vier Strings in
Weiß-Paar CH0/CH1 und Farb-Paar CH4/CH5 — der 3.3-Treiber schreibt auf alle
vier Kanäle denselben Level, die Paar-Zuordnung ist die 3.7-Hypothese und
braucht Silizium-Bestätigung am Gerät (Board-Zugriff, spätere Phase);
(b) keine per-Hue-Kanal-Mischtabelle im Referenz-Image — nur die HSV-V-
Komponente treibt das Duty-Wort, `hue`/`sat` werden für die spätere
Mischtabelle mitgeführt; (c) die echte GPT0-Taktfrequenz bleibt unbelegt
(SDK-`GPT_Init()`-Konfiguration nicht in `.data`) — im Treiberkopf als `TODO`
markiert, nicht als bewiesen dargestellt.

**Host-Test:** neue Sektion `pwm_register` mit **27** Checks (CMR0-Werte je
Befehl, Latch-Maske, Power-off + Restore, Ende-zu-Ende über
`mcufw_state_set_power/set_bright`, Fenster- und Latch-Invariante).
Gesamt **109 PASS, 0 FAIL** (vorher 82).

**Vollverifikation:** `make -C sdk` → **41/41 PASS**, `nm -u mcufw.elf` → **0**,
ELF-SHA256 `ffd06d7ee5799348221bdf18df521b5056be8881f61791ddef08c482078f320f`
(2 Clean-Builds byte-identisch, mehrfach reproduziert). Der Container `mcufw_MRVL.bin` ist bewusst
**nicht** byte-reproduzierbar: `axf2firmware` schreibt `time(NULL)`, es
differiert genau 1 Byte (Offset 9); im Verifier als erwartete Abweichung
dokumentiert. Flash-Budget 256740 B ≤ 384 KB (Slot-Limit).
Neue nm-Guards: `mcufw_pwm_hal_write` muss vorhanden sein, `mcufw_pwm_hal_stub_*`
darf nicht im Image sein.
Der Firmware-Version-String bleibt `0.6-phase3.6`: die nach außen sichtbare
Config-Oberfläche (miIO/LAN-RPC) ist unverändert; der Bump gehört zur ersten
flash-fähigen Phase. Commit: `40a8727` (Code).


### 4.9 Phase 4 — Persistent Config (NVRAM/Flash-Seam) + Power-on-Verhalten

- **mcufw_config-Modul:** 48-Byte-Block (Magic `MCFG` / Version 1 / CRC-32 MRVL-Variante init=0 no-final-xor). Felder: `power`, `bright`, `ct`, `hue`, `sat`, `color_mode`, `name` (sanitised via bestehender State-Grenze).
- **Link-Time-HAL-Seam** (`mcufw_config_hal_{read,write}`): Host-Backend = Datei/Byte-Array; Target-Backend waehlbar via `MCUFW_CONFIG_BACKEND=stub|flash`. Default `stub` = deklarierter No-Persist-Stub (ehrlich, kein stilles Flash-Tun). `flash`-Backend erst nach Sektor-Nachweis auf Bench-Traeger freizugeben (Phase 5).
- **Boot-Pfad:** `mcufw_config_boot(mcufw_state())` laedt persistente Config in den autoritativen Global-State **vor** dem ersten `mcufw_state_apply()`. Diagnose-/Clamp-Block arbeitet danach auf lokaler Kopie.
- **Save-Pfad:** Nur bei expliziter Mutation (`mutations != g_saved_mutations`). Idle-Ticks = **null** Writes. Identischer Save wird uebersprungen (kein Flash-Verschleiss).
- **Robustheit:** Bad CRC / Magic / Version -> definierte Defaults (`power=0, bright=100, ct=4000, CT-mode, name=""`) + genau **ein** UART-Log. Kein Brick, keine Retry-Schleife.
- **Host-Test `config_persist`:** 15 Checks (Round-Trip bit-exakt, CRC/Magic/Version-Korruption -> Defaults, Power-On-Duty 0x7530 @40%, Power-Off-Dark 0xC350, Idle-Zero-Writes, Mutation-One-Save, No-Rewrite-After-Save, Name-Sanitising, Identical-Skipped, CRC-Variant-Cross-Check gegen unabhaengig berechnete Konstante 0x2dfd2d88).
- **Makefile-Guard:** Stub-Backend wird via `$(OBJDUMP)` verifiziert (Toolchain-Variable seit Phase-4-Fix definiert) (keine `flash_drv_*`-Referenz im Config-HAL-Pfad). Vendor-Lib-Symbole im Image sind erlaubt, solange der Config-Pfad sie nicht referenziert.
- **Referenzzahlen Phase 4:** Host-Test **134 PASS / 0 FAIL**, ELF-Verifier **41/41 PASS**, `nm -u`=0, Commit `90eaf1f`.

## §5 Offene Punkte / Folgephasen

| Phase | Inhalt | Status |
|-------|--------|--------|
| 3.6 | Lamp-State-Mutation (State-Model + PWM-Backend-Abstraktion) | **erledigt** (Commit `6d5968c`) |
| 3.7 | `mcufw_pwm` → echte GPT0-Kanäle (Register-Seam + Host-Test `pwm_register`) | **erledigt** (Commit `40a8727`) |
| 4 | Persistent Config (Flash R/W) + Paar-Zuordnung/Mischtabelle am Silizium bestätigen | offen |
| 5 | Echter AP-Join + MQTT-Broker-Connect (Flash-Gate) | offen |
| 6 | OTA-Download (app_url/file_md5/proc=dnld) | offen |

**Hinweis:** Seit 3.7 schreibt `mcufw_pwm_*` echte GPT0-Register (CMR0 +
`USER_REQ`-Latch) — über den Seam `mcufw_pwm_hal_*`, den Host-Test und Target
teilen (Host: Stub-Array, Target: MMIO). Offen bleibt allein die
Silizium-Bestätigung der Kanal-Paar-Zuordnung (Weiß vs. Farbe) und der
GPT0-Taktfrequenz; beides braucht Board-Zugriff. Der Diagnose-Sweep
`mcufw_lamp_set()` im Target-Banner (3.3) bleibt bestehen und überschreibt
bewusst alle vier Kanäle — er ist der einzige Treiber, solange der AP-Join
(Phase 5) den State-Pfad noch nicht live speist.

## §6 Referenzen
- `docs/06_ota_delivery_path.md` — miIO-OTA-Protokoll
- `docs/10_wlan_miio.md` — WLAN/miIO-Integration
- `docs/03_research_lan_control.md` — LAN-Control-Spec
- `firmware/color1_142_0061/upd_yeelink.light.color1.bin` — Referenz-Firmware
