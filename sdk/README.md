# sdk/ — Toolchain-Bringup fuer Custom-Firmware (88MW300/88MW302)

Reproduzierbares Basisprojekt: baut eine minimale `mcufw` (SoC-Init + UART +
LED-Blink, **ohne** WLAN/miIO/Cloud) und packt sie mit dem Vendor-Tool
`axf2firmware` in einen `MRVL`-Container im Layout des Referenz-Images
`color1_0061`.

Alles laeuft auf **devastator** (`192.168.1.154`). Kein Flash, kein
Geraetezugriff.

## Schnellstart

```bash
make bootstrap   # klont EZ-Connect-Lite auf den gepinnten Commit, prueft Tools
make             # baut mcufw.elf -> mcufw_MRVL.bin und verifiziert gegen Referenz
make verify      # nur der Verifier (PASS/FAIL + Check-Zaehler)
make clean
```

Erwartete Ausgabe am Ende:

```
PASS build/mcufw_MRVL.bin: NN/NN Checks OK
```

## Verzeichnisse

```
sdk/
  sdk.lock                 gepinnter SDK-Commit + Referenzpfade (von bootstrap.sh gelesen)
  bootstrap.sh             SDK klonen/pruefen, axf2firmware + Toolchain verifizieren
  Makefile                 Build-Einstieg (bootstrap/elf/container/verify/clean)
  layout.txt               Flash- + MRVL-Container-Layout (SSoT-Ableitung)
  apps/mcufw/
    Makefile               Cross-Compile + Link + axf2firmware
    mw300_min.ld           Link-Skript, abgeleitet von vendor mw300-xip.ld
    src/mcufw_regs.h       minimale Register-Definitionen (mit Provenienz)
    src/startup.c          Vektortabelle, VTOR, .data/.bss, Sprung in main()
    src/main.c             UART0-Konsole, LED-Blink, kein Cloud-Code
  vendor/                  (git-ignoriert) EZ-Connect-Lite, gepinnt
```

`vendor/` wird **nicht** eingecheckt (Groesse + Lizenz). Die Reproduzierbarkeit
kommt aus `sdk.lock` (Commit `99c0521...`, Datum 2016-08-01) plus der
Verifikation in `bootstrap.sh`, das `vendor/SDK_PROVENANCE.txt` mit
Commit-Hash, `axf2firmware`-SHA256 und GCC-Version schreibt.

## Toolchain

| Werkzeug | Version / Herkunft |
| --- | --- |
| `arm-none-eabi-gcc` | 14.2.1 (Debian/Ubuntu-Paket) — SSoT fordert ≥ 4.9.3 |
| `axf2firmware` | `sdk/tools/bin/Linux/axf2firmware` aus dem Vendor-Tree (i386-Host-Tool, laeuft via `ld-linux.so.2`) |
| Link-Skript | `apps/mcufw/mw300_min.ld`, abgeleitet von `build/toolchains/GNU/mw300-xip.ld` |

## Was die Firmware tut

1. `Reset_IRQHandler` (erstes Objekt in `.text`, also auf `0x1f0032d8`):
   VTOR auf `0x00100000`, `.data` kopieren, `.bss` nullen, `main()`.
2. `main()`: CSU-Clock-Gate fuer UART0+GPIO oeffnen, GPIO2/3 auf UART0 muxen,
   UART0 mit 115200 8N1 initialisieren, Banner ausgeben, GPIO40 blinken und
   in jeder Runde eine Zaehlerzeile senden.

Register-Details und ihre Herkunft (SDK-Header bzw. Disassembly von
`CLK_ModuleClkEnable()`) stehen in `apps/mcufw/src/mcufw_regs.h`.

## Bewusste Grenzen (Phase 3.1)

- **Kein Vendor-Link.** `libwmsdk.a` wird nicht gelinkt: die Vendor-Startups
  erwarten Kconfig-generierte Objekte und RTOS-Globals. `mcufw` ist deshalb
  eigenstaendig; die SDK-Bibliothek ist fuer Phase 3.2 (WLAN/miIO) bereitgelegt.
- **Clock-Tree wird nicht programmiert.** `MCUFW_UART_CLK` ist eine Annahme
  (40 MHz) fuer den UART-Teiler; eine falsche Annahme verschiebt nur die
  Baudrate, nicht die Container-Gueltigkeit. Verifikation am echten Gerät ist
  Phase 3.2.
- **Yeelight-LED-Pin unbekannt.** Verwendet wird GPIO40 aus dem
  `mw302_rd`-Boardfile; der reale Yeelight-Pin kommt aus dem Pinmux-Dump des
  Original-Images (Phase 3.2).
- **Vektor-Tabelle 64 Eintraege** (16 Core + 48 IRQ). IRQs oberhalb 47 und
  nicht belegte IRQs landen auf `Default_IRQHandler` (Endlosschleife), nicht in
  einem muelligen Handler.
- **Reproduzierbarkeit:** Compiler-Ausgabe ist deterministisch (keine
  `__DATE__`/`__TIME__`, `-fno-ident`); **nicht** deterministisch sind die vier
  Bytes `0x08`–`0x0b` im Container, weil `axf2firmware` dort `time(NULL)`
  schreibt. Der Verifier kann zwei Builds mit `--determinism-against` bis auf
  dieses Feld vergleichen.
