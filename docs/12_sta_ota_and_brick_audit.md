# 12 — Anti-Brick-Audit (`mcufw` Phase 4), Live-STA-Probe `.115` & Phase-4b-Roadmap

**Datum:** 18.11.2026  
**Anlass:** Übernahme und Sicherheitsaudit nach Blockade von Phase 5 (`miIO.ota → -30020 "OTd not available."` im Setup-AP).  
**Wichtigstes Ergebnis:** Der `-30020`-Abbruch im Setup-AP hat die Testbirne `.115` (**Schlafzimmer Schreibtischlampe**, `yeelink.light.color2`, `2.0.6_0065`, ungeöffnet/OTA-only) vor einem **sofortigen, irreversiblen Brick** bewahrt.

---

## 1. Warum das bisherige `sdk/apps/mcufw`-Image (`.bin`) `.115` sofort gebrickt hätte

Eine direkte Code-Prüfung von `sdk/apps/mcufw/{main.c,src/*}` (Stand Phase 4, Commit `8504201`) ergab **6 fatale Blocker**:

| # | Modul / Stelle | Ist-Zustand im gebauten `mcufw_MRVL.bin` | Konsequenz auf echter Hardware (`.115`) |
| :--- | :--- | :--- | :--- |
| **1** | **Kein A/B-Rollback** (`docs/11` v1) | Irrtümliche Annahme, `boot2` starte bei Fehler einen Setup-AP. Keinerlei `part_init` / `part_set_active_partition` Code vorhanden | Sobald `mcufw` gültiges `MRVL`-CRC hat, bootet `boot2` für immer `mcufw`. Kein Rückweg zu Stock `2.0.6_0065` ohne Aufbrechen |
| **2** | **Kein WLAN-Join / `SD878x`-Load** (`mcufw_wlan.c`) | `mcufw_wlan.c` nimmt nur Symboladressen (`anchors[]`) von `mlan_init.o`, ruft `wm_wlan_connect()` / `wlan_init()` **nie** auf (`"no assoc, no cloud"` in `main.c`) | Lampe verbindet nach Boot mit keinem WLAN und öffnet keinen AP → komplett netzwerklos |
| **3** | **FreeRTOS nie gestartet & IRQs auf Endlosschleife** (`main.c`, `startup.c`) | `main.c` ruft `mcufw_rtos_start()` (`vTaskStartScheduler()`) nie auf; `startup.c` hängt `SVCall`, `PendSV`, `SysTick` und alle 48 HW-IRQs (inkl. SDIO/WLAN/Timer) in `Default_IRQHandler` (`for(;;);`) | Sobald FreeRTOS-Tasks, WLAN-SDIO oder ein HW-IRQ feuern, sofortiger Hang |
| **4** | **Kein Self-OTA-Updater** (`mcufw_miio.c`) | `miIO.ota` wird geparst und mit `["ok"]` quittiert — **kein** HTTP-Download (`httpc`), **kein** `rfget_update_*` / `part_*` Flash-Write | Selbst wenn WLAN käme, könnte man `mcufw` niemals weiter updaten |
| **5** | **`color2` (`YLDP06YL`) Hardware-Treiber unvollständig** (`mcufw_pwm.c`, `main.c`) | Auf Basis `color1_0061` gebaut; `mcufw_pwm.c` ignoriert `ct`/`hue`/`sat` und treibt `(CH0,CH1)` und `(CH4,CH5)` jeweils nur paarweise identisch. `GPIO 44` wird blind auf `1` gesetzt statt sauber gesteuert | Falsche/unvollständige Farb-/CCT-Ausgabe für RGBW-Lampe `color2` |
| **6** | **Kein echter MQTT-Client** (`mcufw_mqtt.c`) | Baut nur JSON-Discovery-/State-Strings im RAM; kein TCP-MQTT-Connect/Publish/Subscribe | Keine Home-Assistant-Steuerung per MQTT |

**Sofortmaßnahme auf `jarvis`:** `/home/jarvis/projects/yeelight-re-flash/mcufw_MRVL.bin` gelöscht und Verzeichnis nach `/home/jarvis/projects/yeelight-re-flash.QUARANTINED_DO_NOT_FLASH` verschoben. Kanban-Ticket `t_7a5f1d16` gestoppt/blockiert; `t_72a569d0` (Phase 5 Flash) bleibt strikt `blocked`.

---

## 2. Live-Probe `.115` (18.11.2026, Read-Only — kein Flash, kein `miIO.ota`)

Live-Ablauf am echten Testträger (`yeelink.light.color2`, `did=05eb8034`, `mac=7C:49:EB:B7:6E:16`, `fw_ver=2.0.6_0065`, `miio_ver=0.0.3`, `hw_ver=mw302`, `wifi_fw_ver=SD878x-14.76.36.p103-702.1.0-WM`):

1. **Setup-AP (`192.168.10.1` via `wlx003676546389`):**
   - `miIO.hello` → `token = dc6e80f9ebfce30b85a0ad5eeb422eea` (`token_readable: True`, `stamp: 9560`)
   - `miIO.info` (verschlüsselt mit `dc6e80f9...`) → `{model: "yeelink.light.color2", fw_ver: "2.0.6_0065", miio_ver: "0.0.3", hw_ver: "mw302", mmfree: 242072}`
   - `set_ps ["cfg_lan_ctrl", "1"]` → `["ok"]`, `get_prop ["lan_ctrl"]` → `["1"]`
   - `miIO.config_router` (`ssid: "HomeLan001"`, `uid: 0`, `tz: "Europe/Berlin"`) → `["ok"]`
2. **STA-Modus im Heim-LAN (`192.168.1.115`):**
   - **TCP 55443 (Yeelight LAN Control):** `{"id":1,"result":["on","100","3500","2"]}` — Home Assistant (`light.schlafzimmer_schreibtischlampe`) sofort wieder online (`on`, Brightness 255, 3500 K).
   - **UDP 54321 `miIO.hello`:** liefert `did=05eb8034`, `stamp=35`, `token=ffffffffffffffffffffffffffffffff` (`token_readable: False`).
   - **UDP 54321 `miIO.info` / `miIO.get_ota_state` / `miIO.get_ota_progress` mit AP-Token `dc6e80f9...`:** **alle drei liefen in `timed out`!**

### Klärender Befund für `t_7a5f1d16` & `fw 2.0.6_0065` (`miio_ver 0.0.3`)

- **Beantwortung `t_7a5f1d16` (1–4):**
  1. Im Firmware-`2.0.6_0065`-Image (`miio_ver 0.0.3`, im Unterschied zum früher analysierten `1.4.2_0027`/`0061`-Zweig!) überlebt der Setup-AP-Token **nicht** nutzbar einen `miIO.config_router`-Wechsel mit `uid: 0` auf lokales UDP `54321` — alle Anfragen mit dem kurz zuvor ausgelesenen AP-Token `dc6e80f9...` wurden im STA-Modus still verworfen (`timed out`), und `hello` liefert `ff...ff`.
  2. Zudem sperrt Yeelight/Xiaomi ab Firmware `2.0.6_xxxx` (`miio_ver 0.0.3`, dokumentiert in `syssi/esphome-yeelight-ceiling-light/docs/flashing-over-the-network.md`) **`miIO.ota` auf dem lokalen UDP-54321-Kanal generell** mit `-30020` (`"OTd not available."` / `"service not available."`) und akzeptiert `miIO.ota` nur noch über den **OTd-/Cloud-RPC-Kanal**.
  3. **Bekannte Delivery-Regeln für `2.0.6_0065` in Phase 5** (erst relevant, wenn Phase 4b fertig ist!):
     - Firmware-HTTP-Server zwingend mit **`HTTP/1.1`** (Python `BaseHTTPRequestHandler` mit `protocol_version = "HTTP/1.1"` + exaktem `Content-Length`) und **auf Port 80 ohne `:port` in `app_url`** betreiben (Bug im `2.0.6`-URL-Parser schlägt sonst `"<ip>:8080"` per DNS nach!).
     - OTA-Trigger später in Phase 5 entweder via **Xiaomi-Cloud-RPC-Relay** (`syssi/esphome-yeelight-ceiling-light/tools/cloud_ota.py` gegen `de.api.io.mi.com/app/home/rpc/99319860` mit lokal gehosteter HTTP/1.1 Port-80 URL — nur Trigger über Cloud, Firmware bleibt im LAN) oder via isoliertem CLI-RPC-Trigger im OPNsense/AdGuard Firewall-Käfig (`docs/14_autark_fleet_ota.md`). **Krypto-Korrektur (22.11.2026, Ghidra `FUN_1f00900c`/`FUN_1f03c090`/`FUN_1f03d154`/`FUN_1f03f1f8`/`FUN_1f03cb28`):** Ein lokaler Fake-`ot.io.mi.com`-OTd-Server kann bei ungeöffneten `2.0.6_xxxx`-Birnen **keine** gültigen Cloud-Frames signieren, da `ot.io.mi.com`/`ott.io.mi.com` ausschließlich den aus Werks-OTP gelesenen `TAG_KEY` (`FUN_1f055718`, nie im `miIO.hello`) verwenden — nicht `ot_token`.

---

## 3. Verifizierte `color2` (`YLDP06YL`, `1.4.2_0027`) LED-Hardware-Fakten

Aus `firmware/color2_142_0027/color2_142_0027.elf` + Ghidra `ALL.c` (`FUN_1f00f1c4`, `FUN_1f00f2a8`, `FUN_1f010dbc`, `FUN_1f0112a0`, `FUN_1f0144d8`, `FUN_1f0119d0`):

- **4× Hardware-PWM auf GPT0 (`0x46070000`):**
  Array `*0x200009dc` (`0x20000a34`, `*0x20000408 = 4` Einträge × 24 B):
  - `ch[0]`: **GPIO 0**, GPT0 `CH0`, PinMux Fn `1`
  - `ch[1]`: **GPIO 1**, GPT0 `CH1`, PinMux Fn `1`
  - `ch[2]`: **GPIO 4**, GPT0 `CH4`, PinMux Fn `1`
  - `ch[3]`: **GPIO 5**, GPT0 `CH5`, PinMux Fn `1`
- **Periode & Polarität:** `50000` (`0xC350`), active-low (`CMR0 = 50000 - duty`, `FUN_1f00f2a8`).
- **LED-Power-Enable (`GPIO 44` / `0x2c`):**
  `FUN_1f00f1c4` konfiguriert GPIO `44` als Output `0` (aus) und erst `FUN_1f00f2a8` schaltet GPIO `44` auf `1` (ein), sobald LEDs angesteuert werden.
- **RGBW-Treiber-Vtable (`FUN_1f0144d8`):**
  Slot `+0x50` (`0x1f0119d0`) skaliert die 4 Kanalwerte (RGB + W) über `255.0` × Kalibrierungsfaktor × `50000 + 0.49` direkt in die 4 GPT0-Kanal-Slots (`0x20000a34 + 0x14 + i*0x18`). Vor dem ersten echten Hardware-Flash ist in `mcufw_pwm` echte RGB+W/CT-Mischung über alle 4 getrennten Kanäle (statt paarweisem Dummy) vorzusehen.

---

## 4. Verbindlicher Phase-4b-Fahrplan vor Phase 5 (Anti-Brick-Checkliste)

`t_72a569d0` (Phase 5 OTA-Flash auf `.115`) darf **erst entblockt werden**, wenn alle folgenden Teilschritte gebaut, reviewt und — soweit ohne Zielhardware möglich — im Host-/QEMU-Harness oder statisch gegen `libwmsdk.a` bewiesen sind:

1. **Phase 4b.1 — Early-Boot 5× Power-Cycle Rollback + `color2` 4-Kanal-PWM & `GPIO 44` Enable:**
   - Nutzt echte WMSDK-Partition-APIs (`part_init`, `part_get_passive_partition`, `part_set_active_partition`, `flash_drv_*`) — kein blindes Booten ohne Fallback auf Stock `2.0.6_0065`.
   - Boot-Fail-Counter (schaltet nach `5` unbestätigten Boots auf Passive-Slot um, bestätigt nach stabilem Lauf `boot_ok`), vollständig per Host-Tests (inkl. korrupter Sektoren, Power-Loss-Simulation) verifiziert.
   - `mcufw_pwm` trennt alle 4 GPT0-Kanäle (`CH0/CH1/CH4/CH5`) inkl. CT-Mix, RGB-zu-RGBW-Umrechnung und `GPIO 44` Enable.
2. **Phase 4b.2 — Echter FreeRTOS-Scheduler, NVIC-Vektoren & `88MW302` WLAN-Bringup:**
   - `startup.c` bindet echte FreeRTOS-Handler (`vPortSVCHandler`, `xPortPendSVHandler`, `xPortSysTickHandler`) und leitet Hardware-IRQs (SDIO/DMA/Timer/UART/GPIO) an die realen Vendor-/SDK-Handler statt `Default_IRQHandler`.
   - `mcufw_wlan.c` + `mcufw_net_lwip.c` integrieren echte `wm_wlan_connect()` / `app_sta_*` / `app_uap_*`-Initialisierung (inkl. Fallback auf eigenen `mcufw`-Setup-AP, falls Heim-WLAN nicht erreichbar ist, sowie NVRAM/PSM-Credentials).
   - `make -C sdk` verifiziert, dass `nm -u = 0` bleibt und alle echten RTOS/WLAN-Einstiegspunkte statt inert-Stubs aktiv verdrahtet sind.
3. **Phase 4b.3 — Integrierter Self-OTA (`HTTP` + `rfget_*/part_*`) + HTTP-Recovery:**
   - Eigenes `mcufw` bringt eingebauten HTTP-Upgrade-/Self-OTA-Pfad (`rfget_update_begin`/`_data`/`_complete` + `part_set_active_partition` oder `miIO.ota`-Handler mit echtem HTTP-Client) mit, damit Folge-Updates nach dem ersten Flash trivial über LAN laufen.
4. **Phase 4b.4 — Natives MQTT + HA Auto-Discovery (über echtes lwIP/TCP):**
   - Echte MQTT-3.1.1-TCP-Client-Verbindung über lwIP + Home Assistant Discovery + LAN-JSON-RPC `55443` kompatibel.
5. **Phase 5 Preflight (Trockentest an `.115` vor Real-Flash):**
   - HTTP/1.1 Server auf Port `80` (ohne `:port` in URL) mit `404`-Antwort gegen gewählten OTA-Trigger (`cloud_ota.py` oder Fake-OTd) testen: `.115` holt `GET /nonexistent_404.bin` ab und kehrt ohne Flash-Schreibvorgang zu `idle` zurück → erst dann echtes Flash-Go durch PO.
