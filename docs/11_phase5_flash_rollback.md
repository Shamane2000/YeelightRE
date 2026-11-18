# 11 — Phase 5: Flash-Rollback-Schutz (`boot2` + `WMPT` A/B-Pflichtnachweis)

**Datum:** 18.11.2026 (korrigiert nach AGY Anti-Brick-Audit am 18.11.2026)  
**Status:** Analysiert und **kritisch korrigiert** — der ursprüngliche Claim *„kein A/B-Recovery nötig, da `boot2` Setup-AP startet“* war **FALSCH** und hätte `.115` bei Fehl-Flash dauerhaft gebrickt!  
**PO-Entscheidung:** Strikt kein Öffnen des Gehäuses (`yeelink.light.color2`, `192.168.1.115`) → JTAG/SPI-Fallback ausgeschlossen → **Early-Boot A/B-Slot-Rollback in `mcufw` ist unbedingtes Gate vor jedem Flash.**

## 0. TL;DR (Korrigiert)

| Frage | Antwort | Beleg |
| :--- | :--- | :--- |
| Schreibt `miIO.ota` nur einen passiven App-Slot (≥`0x10000`)? | **Ja** | Partition-ID `1` via `FUN_1f043fc4` → `FUN_1f0419b4(1, ...)`, wählt Slot mit kleinerer `gen_level` (`+0x14`) |
| Bleibt `boot2` (`0x0`–`0xFFFF`) unangetastet? | **Ja** | Partition `0` wird vom OTA-Pfad nie adressiert; `FUN_1f044174` prüft Slot-Länge |
| Startet `boot2` einen WiFi-/Setup-AP-Recovery bei Fehl-Boot? | **NEIN (FATALER IRRTUM)** | `boot2` hat **keinen WLAN-Stack und keinen Setup-AP**. Sobald `mcufw` gültiges `MRVL`-CRC + höhere `WMPT`-Version hat, bootet `boot2` bei jedem Kaltstart bedingungslos `mcufw`! |
| Existiert ein echter A/B-Schutzslot für Stock-FW `2.0.6_0065`? | **Ja — in der `WMPT`-Partitionstabelle** | `WMPT` (`0x4000`/`0x5000`) hält zwei App-Deskriptoren (Active/Passive, ID `1`). Stock-FW bleibt im anderen Slot erhalten |
| Welche Rollback-Logik ist vor Phase-5-Flash zwingend? | **5× Fast-Power-Cycle Early-Boot Rollback** | Bevor `mcufw` WLAN/App startet, prüft es einen persistierten Boot-Counter. Nach 5× Aus/Ein ohne erfolgreichen Boot: `part_set_active_partition(passive)` + Reboot → Stock `2.0.6_0065`! |
| Ist `format_flash` über miIO erreichbar? | **Nein** | Im `color1_0061`-Image 0 Treffer |

---

## 1. Flash-Layout (`sdk/layout.txt` + Referenz-Images)

| Bereich | Offset | Größe | Inhalt |
| --- | --- | --- | --- |
| `boot2` | `0x000000`–`0x00FFFF` | 64 KB | Minimaler Marvell-SPI-Bootloader (`WMPT` lesen, `MRVL`-Container laden, Entry anspringen — **kein WiFi**) |
| Partition Table 0 | `0x004000` | 16 B Header + N×24 B | Primäre Partitionstabelle (Magic `WMPT`, Version 1) |
| Partition Table 1 | `0x005000` | dito | Redundante Partitionstabelle |
| App-Slot A/B (Part ID 1) | ab `0x010000` | je `0x60000` (384 KB) | Zwei abwechselnd beschreibbare `app`-Slots (Active vs. Passive nach `gen_level` `+0x14`) |
| WiFi FW (Part ID 2) | Layout-abhängig | variabel | `SD878x`-WLAN-Firmware |
| PSM / NVRAM / MFG | Layout-abhängig | variabel | Persistent Storage (`psm-v2`), Factory-Daten |

**Beweis:** `upd_yeelink.light.color1.bin` und `upd_yeelink.light.color2.bin` beginnen bei Offset `0x0` mit `"MRVL"` — sie enthalten ausschließlich einen App-Container für einen der beiden ID-`1`-Slots, niemals `boot2` oder `WMPT`.

---

## 2. OTA-Callgraph (Ghidra-Decompilation `color1_142_0061`)

### 2.1 Einstieg: `miIO.ota` → Worker

```
miIO UDP 54321
  → ota_JsDlgt @ 0x1f0080dc (Modultabelle 0x00103098, Slot "ota")
    → parst app_url / wifi_url / mcu_url / file_md5 / sequence / no_reboot / silent
    → FUN_1f006c80(&LAB_1f008784, ...) // Task-Queue
      → ota_worker_task @ 0x1f008784
```

Für `sequence:"a"` (nur App):

```c
if (acStack_30[local8] == 'a') {
    FUN_1f0085b4(1);          // State = downloading
    iVar9 = FUN_1f008304(param_1);  // App-Segment-Writer
}
```

### 2.2 App-Writer & A/B-Selektion (`FUN_1f008304` → `FUN_1f043fc4` → `FUN_1f0419b4(1, ...)`)

```c
int FUN_1f043fc4(...) {
    iVar1 = FUN_1f0419b4(1, &stack...);   // Erster App-Deskriptor (ID = 1)
    iVar2 = FUN_1f0419b4(1, &stack...);   // Zweiter App-Deskriptor (ID = 1)
    // Wählt die Partition mit kleinerer gen_level (+0x14) als Ziel (passive slot)
    if (*(uint *)(iVar1 + 0x14) < *(uint *)(iVar2 + 0x14))
        return iVar1;
    return iVar2;
}
```

Deskriptor-Layout (`0x20009634`, Stride `0x18` = 24 Bytes):
- `+0x00`: Partition-ID (`1` = App, `2` = WiFi FW, `5` = MCU/other)
- `+0x01`: Name (`"app"`, etc.)
- `+0x0a` / `0x0c`: Startadresse / Länge (`start=0x... size=0x60000`)
- `+0x14`: Generations-/Versionsnummer (`gen_level`, entscheidet für `boot2` welches Image aktiv gebootet wird)

### 2.3 OTA Erase + Write (`FUN_1f0441cc`, `FUN_1f044174`)

- Beide operieren ausschließlich relativ zum Deskriptor (Start + Größe).
- Bei Überschreitung bricht `FUN_1f044174` mit `"Update data greater than partition len"` (`0xffff9ff9`) ab.
- Nach Download + CRC/Hash-Prüfung erhöht der Updater `gen_level` des passiven Slots über die des aktiven und schreibt die neue `WMPT` (`part_set_active_partition`). Der bisherige Stock-Slot bleibt byte-unverändert im Flash!

---

## 3. Warum ohne eigenes A/B-Rollback in `mcufw` akute Brick-Gefahr besteht

Die Annahme in einer früheren Version dieses Dokuments lautete: *„Wenn `mcufw` nicht sauber läuft, startet `boot2` einfach einen Setup-AP.“*  
**Das ist technisch falsch:**

1. `boot2` prüft vor dem Sprung nur die `WMPT`-Tabelle (`0x4000`/`0x5000`) und die **Segment-CRC-32 + Magic `"MRVL"`** des gewählten App-Slots.
2. Ist unser `mcufw_MRVL.bin` korrekt gebaut (CRC gültig!) und durch `miIO.ota` aktiv geschaltet (`gen_level` erhöht), lädt `boot2` beim Einschalten **immer** `mcufw`.
3. Bleibt `mcufw` dann im `Default_IRQHandler` hängen, verbindet nicht mit dem Heim-WLAN, startet keinen HTTP/OTA-Server oder verliert seine Konfiguration, wird `boot2` **niemals** von selbst auf Stock `2.0.6_0065` zurückschalten — die Birne wäre ohne Hardware-Programmer permanent unerreichbar.

### Pflicht-Mechanismus in `mcufw` (Phase 4b.1)

Da der SDK-Snapshot `libwmsdk.a` bereits die realen Partition-APIs bereitstellt (`part_init`, `part_get_active_partition`, `part_get_passive_partition`, `part_set_active_partition`, `flash_drv_*`), gilt vor **jedem** Flash auf `.115`:

1. **Early-Boot Boot-Loop-Zähler (vor Scheduler/WLAN-Init, ganz am Anfang von `main()`):**
   - Ein dedizierter kleiner Flash-/PSM-Marker (oder zwei abwechselnde 4-KB-Sektoren oberhalb der genutzten `WMPT`-Region — Sektor-Belegung zuvor verifizieren!) inkrementiert bei jedem Kaltstart vor jeglicher komplexer Init `boot_fail_count`.
   - Erreicht `boot_fail_count >= 5` (auslösbar durch **5× schnelles Aus-/Einschalten am Lichtschalter** im <10s-Fenster):
     - Counter zurücksetzen,
     - per `part_init()` + `part_get_passive_partition()` + `part_set_active_partition()` den **anderen `app`-Slot (= Stock `2.0.6_0065`)** wieder aktiv schalten,
     - Reset auslösen → Birne bootet garantiert wieder mit originaler Xiaomi/Yeelight-Firmware.
2. **Stabilitäts-Commit („Commit Good Boot“):**
   - Erst nachdem WLAN assoziiert, DHCP-IP bezogen **und** der lokale OTA-/Recovery-Endpunkt mindestens `15 s` stabil gelaufen ist, wird `boot_fail_count = 0` zurückgeschrieben.
3. **Host- + Target-Nachweis:**
   - Der komplette Boot-Counter-/Slot-Swap-Zustandsautomat wird gegen ein Host-Mock der `WMPT`-Partitionstabelle + Flash-HAL deterministisch getestet (`>90 %` Pfadabdeckung, inkl. Power-Loss/Corrupt-Sector-Tests), bevor ein Flash erlaubt wird.
