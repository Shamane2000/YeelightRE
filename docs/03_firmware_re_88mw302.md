# 03 — Firmware-RE: Marvell 88MW302 (`upd_lumi.gateway.v3`)

Statische Analyse der Referenzfirmware auf `devastator` mit **Ghidra 12.1.4**
headless (Projekt `ghidra_workspace/ylre`, ARM Cortex-M4, Thumb-2, LE).
Vollständige Notizen, Dekompilate und Reproduktionsbefehle:
[`analysis/88mw302_miio_re.md`](../analysis/88mw302_miio_re.md).

## Image-Layout

| Bereich | VMA | File-Offset |
| :--- | :--- | :--- |
| `.text` | `0x00100000` | `0x10000` |
| `.text3` | `0x20000040` | `0xa0040` |
| `.text2` (miIO-Logik) | `0x1f003718` | `0x13718` |
| RPC-Methodentabelle | `0x103198` (151 × 8 B bis `0x103650`) | — |

Modul-Deskriptor `"miIO"` @ `0x1f0067f8` (registriert von `FUN_1f0065f8`):
`{name, 0x103198, 0x103220, 17 Slots, …, 0x103220, 0x103650, 134 Slots}`.

## Belegte Kernadressen

| Methode / Objekt | Adresse | Kernaussage |
| :--- | :--- | :--- |
| `miIO.config_router` | `0x1f0089f0` | parst ssid/passwd, persistiert `ot_config.uid` (`FUN_1f00617c`), antwortet sofort, plant Folgeaktion nach 2000 ms (`FUN_1f006e6c(&LAB_1f008cb8,0,2000,0)`) |
| `event.router_changed`-Emitter | `0x1f008cf4` | JSON-Event, Payload je Flag (`0xf0f0f0f0` = „ssid or passwd changed", `0xbe49` = ssid, `0x5a87` = passwd); **4 × 3000 ms Retry** bis Ack-Flag `0x20001bcc` |
| `miIO.ota` | `0x1f0080dc` | Magic-Check `int16 == 0xd431`, Params `file_md5/app_url/wifi_url/mcu_url/sequence/no_reboot/silent`, **Sofort-Ack**, Worker-Queue `&LAB_1f008784` |
| `miIO.get_ota_state` | `0x1f008068` | `{"result":[%s]}` aus Statustabelle `0x2000035c` |
| `miIO.get_ota_progress` | `0x1f0086c4` | `{"result":%d}` aus `FUN_1f00850c` |
| `report_ota_state` | `0x1f0084a0` | Property `ota_state` + Commit |
| Token laden/erzeugen | `0x1f008e10` | s. u. |
| Boot-Init | `0x1f00900c` | `[MIIO-NORMAL-BOOT]` / `[MIIO-TEST-STARTUP-PASS]`, ruft `FUN_1f008e10(0)` |
| Property-Dispatch | `0x1f01342c` | `psm_erase` → `FUN_1f046efc(0,0)`; `token_did` → `0x1f010cd4`; `debug_mode`; `reboot` |
| Nachbar-Token-Setter | `0x1f035b40` | UART-Log `"##Tkn set=%llu,%s,%s"` (**Token im Klartext auf Debug-UART**) |
| CLI `psm-get/-set/-delete/-erase/-dump` | `0x1f046fa4` / `0x1f047180` / `0x1f04711c` / `0x1f046efc` / `0x1f047258` | `psm-get ot_config.ot_token` gibt den Token aus |

## Token-Rotation

`FUN_1f008e10(force)`:

1. `psm_open("ot_config")`, `psm_get("ot_token", buf, 0x23)`.
2. Treffer & `force == 0` → 32 Hex-Zeichen nach `0x20001ca8` (16 B) dekodieren, fertig.
3. Sonst neu: `MD5( sprintf("%u", *(u32*)0x2000b7f4) + "TAG_KEY" + 16×0x00 + "TAG_MAC")`
   (`FUN_1f049f9c` → `FUN_1f06a4a8/6a4d4/6a52e`) → 16-Byte-Token → als 32 Hex-Zeichen
   in `ot_config.ot_token` persistiert.

**Entropie nur 32 Bit** (Tick-Zähler `0x2000b7f4`) — sicherheitsrelevanter Befund.

**Rotationskette (AP→STA / Re-Provisioning):** Es gibt keinen direkten Aufruf mit
`force=1`. Rotation erfolgt über `psm_erase` (Property-Handler `0x1f01342c` →
`FUN_1f046efc(0,0)`) bzw. `miIO.restore` (`0x1f0058cc`): PSM-Wipe ⇒ Reboot ⇒
`FUN_1f008e10(0)` findet keinen Token ⇒ Neuerzeugung ⇒ alter App-Token ungültig.
Cloud-Sync der DID danach via `_sync.neighborDevInfo` (`FUN_1f010cd4`).

## OTA-Prüfung (byte-exakt belegt)

1. Magic `0xd431` im Request-Header (`*(int16*)(msg+0x10)`), Bypass-Flag `0x20001bb0`.
2. `file_md5` (0x21 B via `FUN_1f004e6c`) → `FUN_1f043d90` → `memcpy(0x20009866, buf, 0x21)`.
   **Blindkopie ohne Normalisierung** ⇒ nur Lowercase-Hex (32 Zeichen) besteht die Prüfung.
3. Antwort `ok` **vor** dem Download (`FUN_1f03c2fc`); Arbeit geht an den Worker
   `ota_worker_task_1f008784` @ `0x1f008784` (518 B, Queue via `FUN_1f006c80`) →
   je Ziel `FUN_1f008304` / `FUN_1f00835a` / `FUN_1f008414` (App / WiFi / MCU) →
   `FUN_1f044058` (HTTP 200 + Content-Length) → `FUN_1f043450` → `FUN_1f04326c`.
4. Container-Vorprüfung: Header `"SBHF"` (`0x48464253`) oder `"MRVL"`+`0x2e9cf17b`,
   sonst `"File data is not a firmware file"` (-24589). **Kein XZ** im OTA-Pfad.
5. Stream-Schreiben in ≤0x100-Byte-Blöcken nach Flash; Fortschritt `0x20001124 = written*100/total`.
6. **MD5-Verifikation:** `FUN_1f04326c` → `FUN_1f043e40(flash_addr, written)` →
   `FUN_1f043cdc`: Read-back der geschriebenen Bytes (512-B-Blöcke) aus dem Flash,
   MD5 mit eigener Implementierung (`FUN_1f043b7c`/`ba8`/`c20`, IV
   `0x67452301`/`0xefcdab89`/`0x98badcfe`/`0x10325476`), Digest → 32× `"%02x"` →
   `memcmp(hex, 0x20009866, 32)`. Hashes Eingabe = die Datei-Bytes inkl. 0x14-Byte-Header.
7. Zusätzlich (nur MRVL-Container): Hardware-CRC32 je Segment (Typ 2) gegen Sollwert im
   Container-Header. Reine `"SBHF"`-Images überspringen 6./7. (Bootloader prüft).
8. Mismatch-Pfad: `0xffff9ff4` (-24588) → `"Firmware verification failed with code %d"` →
   `"Firmware update failed"` → Worker: OTA-State 5 `"failed"`, Antwort `0xffffffff`, **kein Reboot**.
   Erfolg: State 3 `"installing"` + Reboot nach 6 s bzw. State 4 `"installed"` bei `no_reboot`.
9. Fehlerantworten: `"invalied"` (-1), `"no mem"` (-5) via `FUN_1f03c1f4`.

Reproduktion: `analysis/ota_worker_task_1f008784.c` (via
`re_scripts/DecompAt.java`) und `analysis/88mw302_miio_re.md` Abschnitt 4.1/4.2.

## Negativbefund `-9999 user ack timeout` / `set_ps`

```
grep -ac "user ack"       elf = 0   (auch im .bin: 0)
grep -ac -- "-9999"       elf = 0   (auch im .bin: 0)
grep -ac "set_ps"         elf = 0
grep -ac "developer_mode" elf = 1   (Property-Dispatch 0x1f01342c)
```

`-9999 user ack timeout` stammt **nicht** aus diesem Firmware-Image, sondern aus
der Client-/Cloud-Seite des miIO-Tunnels. Der einzige Ack-Timeout im Image ist der
4 × 3 s-Retry im `event.router_changed`-Emitter.
