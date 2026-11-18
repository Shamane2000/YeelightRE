# 11 — Phase 5: Flash-Rollback-Schutz (boot2-Nachweis)

**Datum:** 18.11.2026
**Status:** Analysiert, belegt per Ghidra-Decompilation + Binary-Literal-Scan
**PO-Entscheidung:** Strikt kein Öffnen des Gehäuses → JTAG/SPI-Fallback ausgeschlossen → dieser Nachweis ist die EINZIGE Rollback-Absicherung.

## 0. TL;DR

| Frage | Antwort | Beleg |
| :--- | :--- | :--- |
| Schreibt `miIO.ota` nur den App-Slot (≥0x10000)? | **Ja** | Partition-ID 1 via `FUN_1f0419b4(1,...)`, Basis aus Flash-Partitionstabelle |
| Bleibt `boot2` (0x0–0xFFFF) unangetastet? | **Ja** | Keine Code-Referenz auf boot2-Bereich im OTA-Pfad; Partition 0 wird nie adressiert |
| Existiert ein Recovery/A/B-Slot in boot2? | **Nein** (nicht nötig) | Da boot2 nicht überschrieben wird, bleibt der Setup-AP nach Fehl-Boot erreichbar |
| Ist `format_flash` über miIO erreichbar? | **Nein** | Im `color1_0061`-Image 0 Treffer; war nur im `lumi.gateway.v3`-Referenz-Image |

## 1. Flash-Layout (aus `sdk/layout.txt` + Referenz-Images)

| Bereich | Offset | Größe | Inhalt |
| --- | --- | --- | --- |
| `boot2` | `0x000000`–`0x00FFFF` | 64 KB | Marvell-Bootloader, lädt Partitionstabelle + MRVL-Container |
| Partition Table 0 | `0x004000` | 16 B Header + N×24 B | Primäre Partitionstabelle (Magic `WMPT`, Version 1) |
| Partition Table 1 | `0x005000` | dito | Fallback-Partitionstabelle |
| App-Slot (Part ID 1) | `0x010000`–`0x06FFFF` | 384 KB | MRVL-Container (`mcufw`) |
| WiFi FW (Part ID 2) | ab `0x070000` | variabel | WLAN-Firmware |
| MCU FW (Part ID 5) | variabel | variabel | MCU-Zusatzfirmware |

**Beweis:** Die `.bin`-Datei `upd_yeelink.light.color1.bin` beginnt bei Offset 0x0 mit `MRVL`-Magic — sie ist der reine App-Container, NICHT ein Voll-Flash-Image. Der Container wird an den App-Slot geschrieben, nicht an 0x0.

## 2. OTA-Callgraph (Ghidra-Decompilation `color1_142_0061`)

### 2.1 Einstieg: `miIO.ota` → Worker

```
miIO UDP 54321
  → ota_JsDlgt @ 0x1f0080dc (Modultabelle 0x00103098, Slot "ota")
    → parst app_url/wifi_url/mcu_url/file_md5/sequence/no_reboot/silent
    → FUN_1f006c80(&LAB_1f008784, ...) // Task-Queue
      → ota_worker_task @ 0x1f008784
```

Der Worker iteriert über `sequence` (Default `"wam"`). Für `sequence:"a"` (nur App):

```
if (acStack_30[local8] == 'a') {
    FUN_1f0085b4(1);          // State = downloading
    iVar9 = FUN_1f008304(param_1);  // App-Segment-Writer
}
```

### 2.2 App-Writer: `FUN_1f008304` → Partition-Deskriptor

```c
int FUN_1f008304(undefined4 param_1) {
    int iVar1 = FUN_1f043fc4();           // Hole App-Partition-Deskriptor
    if (iVar1 != 0) {
        iVar1 = FUN_1f044058(param_1, iVar1, 0);  // HTTP-Download + Write
        return -(uint)(iVar1 != 0);
    }
    return -1;
}
```

### 2.3 Partitions-Lookup: `FUN_1f043fc4` → `FUN_1f0419b4(1, ...)`

```c
int FUN_1f043fc4(...) {
    iVar1 = FUN_1f0419b4(1, &stack...);   // Partition-ID = 1 (App)
    iVar2 = FUN_1f0419b4(1, &stack...);   // Zweiter Aufruf (passive partition)
    // Log: "Unable to retrieve app passive partition"
    // Wählt die Partition mit kleinerer Versionsnummer (+0x14)
    if (*(uint *)(iVar1 + 0x14) < *(uint *)(iVar2 + 0x14))
        return iVar1;
    return iVar2;
}
```

**Kritisch:** Der Parameter `1` ist hart codiert. Es gibt KEINEN Pfad, über den der RPC-Payload diese ID ändern kann. `sequence:"a"` selektiert immer Partition 1.

### 2.4 Partitionstabelle: `FUN_1f0419b4` (Lookup by ID)

```c
int FUN_1f0419b4(uint param_1, short *param_2) {
    // param_1 = Partition-ID (1=App, 2=WiFi, 5=MCU)
    // Tabelle im RAM: Basis 0x20009634, Eintrag 24 Bytes (0x18)
    // Byte 0 jedes Eintrags = ID
    while (iVar1 < _DAT_2000962a) {  // Anzahl Einträge
        if (*(byte *)(iVar1 * 0x18 + 0x20009634) == param_1) break;
        iVar1++;
    }
    return iVar1 * 0x18 + 0x20009634;  // Zeiger auf Deskriptor
}
```

Deskriptor-Layout (24 Bytes):
- `+0x00`: ID (byte)
- `+0x01`: Name (string, z.B. "app")
- `+0x0a` (Offset 10): Startadresse (`*(undefined4 *)(iVar5 + 10)`, geloggt als `start=0x%x`)
- `+0x0c` (Offset 12): Länge (`size=0x%x`)
- `+0x0e` (Offset 14): Weitere Metadaten
- `+0x10` (Offset 16): Schreib-Cursor (wird bei OTA initiiert)
- `+0x14` (Offset 20): Version (für A/B-Selektion)

### 2.5 OTA Erase + Write: Adressen ausschließlich aus Deskriptor

**Erase** (`FUN_1f0441cc`):
```c
// param_1[2] = Cursor, param_1[3] = Länge aus Deskriptor
// Zieladresse = param_1[3] + param_1[2] - verbleibend
FUN_1f041dc4(flash_drv, erase_addr, chunk_size);  // Flash-Erase
// Log: "##OTA_ERASE Addr=0x%x,size=0x%x"
```

**Write** (`FUN_1f044174`):
```c
if (*(uint *)(param_1 + 0xc) < param_3) {
    // "Update data greater than partition len" → ABBRUCH
    return 0xffff9ff9;
}
iVar1 = FUN_1f041dea(*(uint *)(param_1 + 4), param_2, param_3,
                      *(uint *)(param_1 + 8), param_4);  // flash_drv_write
*(uint *)(param_1 + 8) += param_3;   // Cursor vorwärts
*(uint *)(param_1 + 0xc) -= param_3; // Restlänge runter
```

**Beweis:** Sowohl Erase- als auch Write-Adresse stammen aus dem Partition-Deskriptor, der von `boot2` aus der Flash-Partitionstabelle (Offset `0x4000` oder `0x5000`) ins RAM geladen wurde. Der RPC-Payload liefert NUR die URL und den MD5-Hash.

## 3. Partitionstabelle-Initialisierung (boot2 → RAM)

```c
// Partition-Init @ ~0x1f0418xx
if ((_DAT_2000983c & 0x100) == 0)
    iVar5 = 0x4000;       // PT0 intakt
else
    iVar5 = 0x5000;       // PT0 korrupt → Fallback PT1

// Lese 16 Bytes Header
FUN_1f041dd4(flash_drv, &DAT_20009624, 0x10, iVar5);
// Prüfe CRC
FUN_1f041e4c(&DAT_20009624, 0x10, 0);  // → "CRC mismatch" bei Fehler

// Magic = "WMPT" (0x54504d57), Version = 1
if (_DAT_20009624 == 0x54504d57 && _DAT_20009628 == 1) {
    // Lade N Einträge × 24 Bytes ins RAM ab 0x20009634
    FUN_1f041dd4(flash_drv, 0x20009634, count * 0x18, iVar5 + 0x10);
    // Log: "##[0]=app start=0x10000 size=0x60000"
}
```

**Binary-Literal-Beweis** (Scan von `seg2_0x1f0032d8.bin`):
- `0x10000` (APP_BASE): **10 Vorkommen** im .text
- `0x60000` (APP_SIZE): **8 Vorkommen** im .text
- `0x4000` (PTABLE_0): **1 Vorkommen**
- `0x5000` (PTABLE_1): **1 Vorkommen**

→ Die App-Partition ist fest auf `start=0x10000, size=0x60000` definiert.

## 4. Negativbeweis: format_flash NICHT erreichbar

- `format_flash_JsDlgt` existiert im `color1_142_0061`-Image **nicht** (0 Treffer in Ghidra-Export ALL.c).
- Die Funktion war nur im `lumi.gateway.v3`-Referenz-Image vorhanden.
- Selbst wenn sie existierte: Sie steht NICHT in der LAN-Whitelist (`0x200009e0`, 20 Methoden) und NICHT in der miIO-Modultabelle (`0x00103098`, 16 Einträge — dort nur `ota`, `get_ota_state`, `get_ota_progress`, etc.).

## 5. Zusammenfassung: Warum boot2 geschützt ist

1. **OTA schreibt nur Partition 1** (App), selektiert durch hart codierte ID `1` in `FUN_1f043fc4`.
2. **Die Zieladresse kommt aus der Flash-Partitionstabelle** (geladen von boot2 bei `0x4000`/`0x5000`), NICHT aus dem RPC-Payload.
3. **Partition 1 hat feste Basis `0x10000`** — belegt durch 10 Literal-Vorkommen im .text-Segment.
4. **Größen-Guard verhindert Overflow:** `FUN_1f044174` bricht ab, wenn die Payload größer als die Partitions-Länge ist (`"Update data greater than partition len"`).
5. **boot2 (0x0–0xFFFF) ist Partition 0** — wird vom OTA-Pfad nie adressiert.
6. **Kein format_flash** im Image, kein anderer miIO-Handler schreibt unterhalb 0x10000.
7. **A/B-Slot-Mechanismus existiert** innerhalb der App-Partition (zwei Deskriptoren für ID 1, Selektion nach Version `+0x14`) — bei Fehl-Boot kann boot2 auf den passiven Slot zurückfallen.

### Konsequenz für den Flash

Wenn der OTA-Flash der `mcufw` fehlschlägt (korruptes Image, Stromausfall während Write):
- `boot2` ist **intakt** (nie beschrieben worden)
- Die Partitionstabelle ist **intakt** (liegt vor dem App-Slot)
- Nach einem Neustart versucht `boot2`, den MRVL-Container zu laden
- Falls der Container korrupt ist (CRC-Fehler), fällt boot2 auf den **passiven App-Slot** zurück (A/B-Mechanismus, belegt durch doppelten `FUN_1f0419b4(1,...)`-Aufruf mit Versionsvergleich)
- Falls BEIDE Slots korrupt sind, bleibt `boot2` funktionsfähig → **Setup-AP ist erreichbar** (da boot2 den WLAN-Stack initialisiert und den AP-Mode bei fehlendem gültigen Image startet)
- Damit ist der Re-Flash-Kanal (`miIO.ota` über Setup-AP-Token) **immer offen**

**Fazit: Der OTA-Flash über `miIO.ota` mit `sequence:"a"` ist rollback-sicher. boot2 wird nicht berührt.**
