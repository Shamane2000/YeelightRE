# 07 — SDK-Build-Bringup (Phase 3.1): Minimal-`mcufw` + MRVL-Container

Stand: Phase 3.1 abgeschlossen. Beweis, dass der Build-Pfad für Custom-Firmware
steht: SDK-Basisprojekt → Cross-Compile → Link auf die Vendor-Ladeadressen →
`axf2firmware` → formatgültiger `MRVL`-Container, verifiziert gegen das
Referenz-Image `color1_0061`.

Reiner Build auf **devastator** (`192.168.1.154`). Kein Flash, kein
Gerätezugriff; Testträger `.115` unangetastet.

## 1. Toolchain

| Werkzeug | Version | Quelle |
| --- | --- | --- |
| `arm-none-eabi-gcc` | `14.2.1 20241119` (`15:14.2.rel1-1`, Debian) | Paket `gcc-arm-none-eabi` |
| `axf2firmware` | Vendor-Binary, `sha256=3b7de76cf8436aad321a57fc1dc715102e0b70a020d2f142b7dedb62295dbfe5` | `vendor/ez-connect-lite/sdk/tools/bin/Linux/axf2firmware` (ELF 32-bit i386, `ld-linux.so.2`) |
| SDK | EZ-Connect-Lite, Commit `99c05212f5853a1750c29b89f01d95041e3322ef` (2016-08-01) | `https://github.com/dhrishi/ez-connect-lite.git`, gepinnt in `sdk/sdk.lock` |
| Link-Skript | `sdk/apps/mcufw/mw300_min.ld` | abgeleitet von `build/toolchains/GNU/mw300-xip.ld` |

`make bootstrap` klont/checkoutet den Commit, prüft `axf2firmware` (Smoke-Test
über die Usage-Ausgabe — das Tool braucht **zwei** Argumente:
`axf2firmware <in.elf> <out.bin>`) und schreibt
`sdk/vendor/SDK_PROVENANCE.txt` mit Commit, Tool-SHA256 und GCC-Version.

## 2. Build-Steps

```bash
cd /home/jarvis/projects/yeelight-re
make -C sdk bootstrap        # SDK + Tools, einmalig (idempotent)
make -C sdk                  # elf -> container -> verify
make -C sdk verify-reference # Verifier gegen das Vendor-Referenzimage
make -C sdk verify-roundtrip # axf2firmware-Treue: Referenz-ELF neu packen
```

Frischer Arbeitsbaum (`make -C sdk clean`) plus die vier Schritte oben ist der
komplette reproduzierbare Pfad. `sdk/vendor/` ist git-ignoriert; die
Reproduzierbarkeit kommt aus `sdk/sdk.lock` + `bootstrap.sh`-Verifikation.

## 3. Ergebnis: ELF-Layout

```
Entry point address:               0x1f0032d9
LOAD 0x010000 0x00100000 0x00100000 0x03210 0x03210 R   0x10000   # .init  (SRAM0)
LOAD 0x0132d8 0x1f0032d8 0x1f0032d8 0x00268 0x00268 R E 0x10000   # .text  (XIP)
LOAD 0x020040 0x20000040 0x20000040 0x00004 0x00004 RW  0x10000   # .data  (SRAM1)
LOAD 0x010000 0x480c0000 0x480c0000 0x00000 0x00040 RW  0x10000   # .nvram (NOLOAD)

.init  12816 B @ 0x00100000   (davon genutzt: 256 B Vektortabelle, Rest Padding)
.text    616 B @ 0x1f0032d8
.data      4 B @ 0x20000040
.bss       0 B
```

Die drei **ladenden** Segmente liegen exakt auf den Adressen des Vendor-Images
`color1_0061` (`0x00100000` / `0x1f0032d8` / `0x20000040`), `e_entry` ist
`0x1f0032d9` (Thumb-Bit). Das `.nvram`-Segment ist `NOLOAD` (`p_filesz=0`) und
taucht im Container nicht auf.

Zwei Linker-Konstruktionen sind dafür verantwortlich:

1. `.text (_flashc_mem_start + _text_offset + _init_size)` mit
   `_flashc_mem_start=0x1f000000`, `_text_offset=0xc8` (20-B-Header + 3×20-B
   Segmenttabelle) und `_init_size=0x3210` (Vendor-`.init`-Belegung).
2. Auffüllen des eigenen `.init` **innerhalb** der Output-Sektion auf
   `_init_size`, sonst würde `.text` bei einem kleinen Image deutlich früher
   landen. Drei `ASSERT()`s im Skript erzwingen
   `SIZEOF(.init)==0x3210`, `ADDR(.text)==0x1f0032d8`, `ADDR(.data)==0x20000040`.

## 4. Ergebnis: MRVL-Container

```
$ ls -l sdk/apps/mcufw/build/mcufw_MRVL.bin
-rw-rw-r-- 1 jarvis jarvis 13636 ... mcufw_MRVL.bin        # 0x3544

sha256=72fa05094ae0a033967d849e98d8b63980b87591022dee8111bf848d6192e95d

magic=MRVL  magic2=0x2e9cf17b  segs=3  entry=0x1f0032d9
seg1 type=2 off=0x0000c8 size=0x3210 addr=0x00100000
seg2 type=2 off=0x0032d8 size=0x0268 addr=0x1f0032d8
seg3 type=2 off=0x03540 size=0x0004 addr=0x20000040
```

Vollständiges Container-Layout (Header, Segmenttabelle, CRC-Verfahren,
Flash-Slot-Grenzen) steht in `sdk/layout.txt`.

**Wichtig für Phase 3.2:** Ohne mindestens eine initialisierte globale Variable
löscht der Linker `.data`, und `axf2firmware` erzeugt nur **zwei** Segmente.
`sdk/apps/mcufw/src/main.c` hält deshalb `mcufw_boot_marker` (4 B, `"MCUF"`),
damit das dritte Segment — wie beim Vendor-Image — real existiert.

## 5. Verifikation (echte Ausgaben)

```
$ make -C sdk                          # Build + Verify gegen Referenz
PASS sdk/apps/mcufw/build/mcufw_MRVL.bin: 41/41 Checks OK      (rc=0)

$ make -C sdk verify-reference         # Verifier-Selbsttest am Vendor-Image
PASS firmware/color1_142_0061/upd_yeelink.light.color1.bin: 35/35 Checks OK (rc=0)

$ make -C sdk verify-roundtrip         # axf2firmware-Treue
PASS build/ref_roundtrip.bin: 38/38 Checks OK                  (rc=0)
```

Der neue Verifier `tools/verify_mrvl_container.py` prüft je Container:

| Gruppe | Checks |
| --- | --- |
| Header | `magic == "MRVL"`, `magic2 == 0x2e9cf17b`, `1 ≤ seg_cnt ≤ 8`, Tabelle passt in die Datei |
| Segmente | `type == 2`, `size > 0`, Offset nach der Tabelle, Linkreihenfolge ohne Überlappung, Segment vollständig in der Datei, Body-Länge == deklarierte Größe, **CRC-32 je Segment** |
| Struktur | Lücke Tabelle→erstes Segment ist `0xFF`-gefüllt, Datei endet exakt am letzten Segment |
| Entry | Thumb-Bit gesetzt, Entry zeigt in ein ladbares Segment |
| Memory-Map | Segment @`0x00100000` vorhanden, App-Segment im Flash-Fenster `0x1f000000..0x1fffffff`, Ladeadressen eindeutig |
| Slot | Container ≤ App-Slot `0x60000` (384 KB) |
| Referenz | Segmentanzahl, Ladeadressen, Segmenttypen, Entry-Konvention identisch zu `color1_0061` |

CRC-Verfahren (empirisch an allen drei Referenzsegmenten bestätigt):
reflektiertes CRC-32 mit Tabelle `0xEDB88320`, **Initialwert 0, kein finales
XOR**. `zlib.crc32()` ist falsch (invertiert vor/nach).

Determinismus: zwei Cleans+Builts liefern byte-identische Container bis auf die
4 Timestamp-Bytes (`--determinism-against`, `PASS`). Das ELF selbst ist
deterministisch (kein `__DATE__`/`__TIME__`, `-fno-ident`).

## 6. Befunde / begründete Abweichungen

1. **Timestamp im Container ist nicht reproduzierbar.** `axf2firmware` schreibt
   `time(NULL)` nach `0x08–0x0b`. Verifier und Doku behandeln das Feld als
   erwartete Abweichung; alles andere muss byte-identisch sein.
2. **`e_entry` des Referenz-ELF-Artefakts weicht vom ausgelieferten Container
   ab.** `color1_0061.elf` hat `e_entry=0x100001`, der ausgelieferte Container
   `0x1f0032d9`. Beweis: `axf2firmware` auf dieses ELF angewandt erzeugt eine
   Datei, die bis auf **8 Bytes** (`0x08–0x0b` Timestamp, `0x10–0x13` Entry)
   identisch mit dem Vendor-Image ist — Segmenttabelle inkl. aller CRCs und alle
   Rohdaten stimmen exakt. Das ELF ist also inhaltlich treu, nur sein
   Entry-Symbol zeigt auf `0x00100000` (Beginn `.init`) statt auf
   `0x1f0032d8`. `mcufw` folgt der **ausgelieferten** Konvention
   (`Reset_IRQHandler` als erstes Objekt in `.text`).
3. **`libwmsdk.a` wird nicht gelinkt.** Vendor-Startups erwarten
   Kconfig-generierte Objekte und RTOS-Globals; `mcufw` ist eigenständig und
   nutzt nur dokumentierte Register. Die SDK-Bibliothek liegt für Phase 3.2
   (WLAN/miIO) bereit.
4. **Clock-Tree wird nicht programmiert.** `MCUFW_UART_CLK=40 MHz` ist eine
   Annahme für den UART-Teiler (boot2 hinterlässt den Clock-Tree); eine falsche
   Annahme verschiebt nur die Baudrate, nicht die Container-Gültigkeit. Live-
   Verifikation erst mit Hardware-Zugriff (Phase 3.2).
5. **LED-Pin ist vorläufig GPIO40** (aus dem `mw302_rd`-Boardfile). Der reale
   Yeelight-Pin kommt aus dem Pinmux des Original-Images (Phase 3.2).
6. **Vektortabelle 64 Einträge** (16 Core + 48 IRQ). Nicht belegte IRQs und alle
   IRQs > 47 landen auf `Default_IRQHandler` (Endlosschleife) statt in Müll.

## 7. Offene Punkte für Phase 3.2

- UART-Baudrate/UART-Clock gegen echtes Gerät prüfen (`MCUFW_UART_CLK`).
- Yeelight-Pinmux aus `color1_0061` übernehmen (LED, Taster, UART-Pins am
  Yeelight-Board, nicht am `mw302_rd`-Referenzboard).
- `libwmsdk.a` linken (Vendor-Startup + Kconfig-Objekte) für WLAN/miIO-Stack.
- Boot2-Vertrag verifizieren (Kopiert boot2 wirklich alle drei Segmente an ihre
  Ladeadressen? Erwartung aus dem Container-Format, noch nicht am Gerät
  bestätigt).
