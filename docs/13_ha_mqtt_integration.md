# 13 — Home Assistant 2026.11 & MQTT 3.1.1 Integration (`mcufw 0.8-phase4b.4`)

Stand: **Phase 4b.4** (produktionsreifer lokaler MQTT-3.1.1-Client auf Basis von raw lwIP TCP + DNS, vollständige HA-2026.11 Auto-Discovery und bidirektionale Synchronisation mit LAN-RPC `55443`).

---

## 1. Netzwerk- & Broker-Konfiguration

- **Standard-Broker:** `192.168.1.106:1883` (`mqtt_enabled = 1`, `pwm_freq_hz = 768`, `power_on_mode = off`, `default_transition_ms = 0`).
- **Setup-AP / LAN HTTP-Portal (`:8080`):**
  - Status prüfen: `curl -s http://<BULB-IP>:8080/api/status`
  - Aktuelle Konfiguration lesen (Passwörter maskiert als `"***"`): `curl -s http://<BULB-IP>:8080/api/config`
  - Broker-Zugangsdaten und WLAN persistent setzen (`sys_psm` Partition, atomar CRC-32-geschützt):
    ```bash
    curl -X POST http://<BULB-IP>:8080/api/config \
      -H "Content-Type: application/json" \
      -d '{"wifi_ssid":"<SSID>","wifi_psk":"<PSK>","mqtt_host":"192.168.1.106","mqtt_port":1883,"mqtt_user":"<USER>","mqtt_pass":"<PASS>","mqtt_enabled":1}'
    ```
  - Ändern sich `mqtt_host`, `mqtt_port` oder `mqtt_user` im Betrieb, baut der MQTT-Client die Verbindung automatisch sauber neu auf.
  - Nach `DISCONNECT` / Broker-Ausfall erfolgt ein exponentieller Backoff (`2 s -> 4 s -> 8 s -> 16 s -> max. 30 s`); Keepalive `PINGREQ`/`PINGRESP` läuft alle `30 s` (`10 s` Response-Timeout).
  - **Last Will & Testament (LWT):** Beim `CONNECT` registriert die Lampe `yeelight/<did8>/availability = "offline"` (retained). Nach erfolgreichem `CONNACK` sendet sie `"online"` (retained).

---

## 2. Home Assistant `2026.11` Auto-Discovery-Entitäten

Alle Entitäten teilen ein gemeinsames Device-Objekt (`identifiers: ["yeelight_<did8>"]`, `model: "YLDP06YL"`, `manufacturer: "Yeelight (mcufw)"`, `sw_version: "0.8-phase4b.4"`, `configuration_url: "http://<ip>:8080/"`).
Die Haupt-Entität (`light`) nutzt `"name": null` nach HA-`2026.11`-Konvention, sodass der Entitätsname direkt den Gerätenamen übernimmt ohne doppeltes Präfix.

| HA-Komponente | Discovery-Topic | Command-Topic | State-Topic | Funktion / Bereich |
|---|---|---|---|---|
| `light` | `homeassistant/light/yeelight_<did8>/config` | `yeelight/<did8>/set` | `yeelight/<did8>/state` | HA JSON-Light (`ON`/`OFF`, Helligkeit `1..100`, Farbtemperatur `153..588` Mireds ≈ `6500..1700` K, `rgb`) |
| `update` | `homeassistant/update/yeelight_<did8>_fw/config` | `yeelight/<did8>/ota/install` | `yeelight/<did8>/ota/state` | HTTP Self-OTA (`installed_version`, `in_progress`, `status`) |
| `select` (`config`) | `homeassistant/select/yeelight_<did8>_poweron/config` | `yeelight/<did8>/config/power_on_mode/set` | `yeelight/<did8>/config/state` | Einschaltverhalten (`last`, `off`, `on_default`) |
| `number` (`config`) | `homeassistant/number/yeelight_<did8>_pwm_freq/config` | `yeelight/<did8>/config/pwm_freq_hz/set` | `yeelight/<did8>/config/state` | Anti-Flacker-PWM-Frequenz (`500..20000` Hz, Default `768`) |
| `number` (`config`) | `homeassistant/number/yeelight_<did8>_transition/config` | `yeelight/<did8>/config/transition_ms/set` | `yeelight/<did8>/config/state` | Standard-Übergangszeit (`0..5000` ms) |
| `button` (`config`) | `homeassistant/button/yeelight_<did8>_restart/config` | `yeelight/<did8>/config/restart` | — | Gerät neu starten (`PRESS`, verzögert für TCP-Flush) |
| `button` (`config`) | `homeassistant/button/yeelight_<did8>_rollback/config` | `yeelight/<did8>/config/rollback` | — | **Safe Rollback to Stock** (`ROLLBACK`) auf passiven WMPT-Slot |
| `sensor` (`diagnostic`) | `homeassistant/sensor/yeelight_<did8>_rssi/config` | — | `yeelight/<did8>/diag` | `rssi` (dBm) + Attribute `ip`, `active_slot`, `uptime_s`, `ota_status` |

---

## 3. Bidirektionale Synchronisation (LAN-RPC `55443` ↔ MQTT)

`mcufw_state()` ist die einzige Single-Source-of-Truth für den Lampenzustand:
- Eingehende **MQTT-Befehle** auf `yeelight/<did8>/set` mutieren `mcufw_state()`; die Single-Writer-Schleife `mcufw_pwm_service()` aktualisiert die GPT0-Register einmalig, und der nächste Tick publiziert sofort den neuen Zustand auf `yeelight/<did8>/state`.
- Eingehende **LAN-RPC-Befehle (TCP `55443`)** oder **miIO-Befehle (UDP `54321`)** erhöhen `mcufw_state()->mutations`; `mcufw_mqtt_client_tick()` erkennt die Änderung automatisch und synchronisiert `yeelight/<did8>/state` innerhalb eines Ticks (~`10 ms`) zurück an Home Assistant.
- Idle-Ticks ohne State-Änderung erzeugen **0** PWM-Register- und **0** Flash-Schreibzugriffe.

---

## 4. Self-OTA via Home Assistant & 1-Klick-Rollback

- **HTTP Self-OTA per MQTT:**
  Publish auf `yeelight/<did8>/ota/install`:
  ```json
  {"url": "http://192.168.1.196:8088/mcufw_MRVL.bin", "md5": "<32-hex-chars-optional>"}
  ```
  (Alternativ wird auch eine reine URL-Zeichenkette akzeptiert.)
  `mcufw_ota_queue()` stellt das Update in die Runtime-Warteschlange; vor dem ersten Flash-Write auf die passive WMPT-Partition werden HTTP `200`, nicht-chunked Länge `<= 384 KB`, `MRVL`-Magic-Header und (falls angegeben) Stream-MD5 geprüft. Bei Fehler bricht das Update ohne Slot-Wechsel ab.
- **1-Klick-Rollback auf Stock:**
  Der Button **Safe Rollback to Stock** (oder `POST http://<BULB-IP>:8080/api/rollback`) ruft `mcufw_ota_request_rollback()` auf, aktiviert per `rfget_get_passive_firmware()` + `part_set_active_partition()` die zuvor genutzte Partition (Stock `2.0.6_0065`) und startet den 88MW300 nach einem kurzen Flush-Delay sauber neu.

---

## 5. Phase-5-Übergang: `404`-Dry-Run vor erstem echten Flash

Vor dem ersten realen Flash der Schreibtischlampe (`192.168.1.115`) gilt weiterhin die Gate-Sequenz aus `docs/11_phase5_flash_rollback.md`:
1. Auf `jarvis` (`192.168.1.196`) läuft ein lokaler HTTP-Server auf Port `80` (oder explizitem Port), **ohne** gültige Binary unter `/dryrun-missing.bin`.
2. Ein einzelnes `miIO.ota` zeigt auf diese URL -> die Stock-Firmware antwortet mit HTTP `404`-Downloadfehler, ohne ihren Flash zu berühren.
3. Erst nach erfolgreichem `404`-Dry-Run-Nachweis wird die verifizierte `mcufw_MRVL.bin` bereitgestellt und ein Slot-Flash ausgelöst.
