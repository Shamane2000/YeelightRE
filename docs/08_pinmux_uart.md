# 08 — Pinmux, GPIO-Semantik und UART-Taktung aus `color1_0061.elf`

Phase 3.2 des Rebuilds. Ziel: den **realen Hardware-I/O-Pfad** der Yeelight-`color`
(YLDP02YL, Marvell 88MW302) belegen und in `sdk/apps/mcufw` übernehmen —
Pinmux (LED/PWM, Taster, UART), korrekte GPIO-Register-Semantik und die
UART-Taktung — und dabei `libwmsdk.a` echt in das Image linken.

**Randbedingung eingehalten:** reiner Build auf devastator (`192.168.1.154`).
Kein Flash, kein Gerätezugriff, kein Öffnen. Testträger `.115` unangetastet.

---

## 1. Methode

Analyse-Werkzeug (neu im Repo): `tools/pinmux_re.py`, läuft auf devastator in
`/home/jarvis/projects/yeelight-re/.venv` (Python 3.14.4, capstone 5.0.7,
pyelftools 0.33 — `pyelftools` musste nachinstalliert werden).

```bash
cd /home/jarvis/projects/yeelight-re
.venv/bin/python tools/pinmux_re.py firmware/color1_142_0061/color1_0061.elf
```

Das Skript

1. baut eine Wort-Landkarte (`addr -> uint32`) über alle allokierten
   `SHT_PROGBITS`-Sektionen,
2. disassembliert alle `SHF_EXECINSTR`-Sektionen linear (Thumb-2/M-Class,
   `skipdata=True`, damit Dateninseln die Analyse nicht abbrechen) — 69 547
   Instruktionen,
3. löst PC-relative `ldr`-Literal gegen die MW300-Speicherkarte
   (UART0/1/2, GPIO, PINMUX, RCU, CSU, …) auf,
4. findet Aufrufe der Peripherie-Accessoren und propagiert deren **konstante**
   Pin-/Funktionsargumente rückwärts aus `movs r0/r1,#imm`.

Die Accessoren wurden vorab aus der Disassembly identifiziert (alle in
`libwmsdk.a`, siehe §5):

| Code-Adresse | Accessor | Registerbeweis |
|---|---|---|
| `0x1f036584` | `GPIO_PinMuxFun(pin, func)` | `PINMUX_BASE + 4*pin`, `bits[2:0] = func`, `bit15` gelöscht |
| `0x1f0365dc` | `GPIO_PinModeConfig(pin, mode)` | `bit3` (`DI_EN`), `bits[13..15]` (Pull) |
| `0x1f03669c` | `GPIO_WritePinOutput(pin, lvl)` | lvl=1 → `0x46060054+bank*4`, lvl=0 → `0x46060060+bank*4` |
| `0x1f0366c8` | `GPIO_SetPinDir(pin, dir)` | dir=1 → `0x46060018+bank*4`, dir=0 → `0x46060024+bank*4` |
| `0x1f0366f2` | `GPIO_ReadPinLevel(pin)` | `0x46060000+bank*4`, Bit `pin&31` |

---

## 2. Pinmux-Registermodell (belegt, nicht geraten)

`regs/pinmux_reg.h` (Vendor-SDK) und die Disassembly von `GPIO_PinMuxFun`
stimmen exakt überein:

```
PINMUX_BASE = 0x48010000     ein 32-bit-Wort pro Pin:  addr = BASE + 4*pin
  [2:0]  FSEL_XR         Funktionsnummer
  [3]    DI_EN           Input-Enable
  [13]   PIO_PULLDN_R    Pull-Down
  [14]   PIO_PULLUP_R    Pull-Up
  [15]   PIO_PULL_SEL_R  Pull-Auswahl / Function-Valid
```

`GPIO_PinMuxFun(pin, func)`: `func == 0` → nur `bits[2:0]` löschen (GPIO-Modus,
`bit15` bleibt); `func >= 2` → `bit15` löschen und `bits[2:0] = func` setzen.
Für die „Sonder-Pins“ 6–10, 22–26, 28–33 nimmt der Vendor-Code einen
eigenen Zweig (`func == 1` → nur `bits[2:0] = 1`).

---

## 3. Pinmux-Tabelle der Yeelight `color` (aus `color1_0061.elf`)

| Funktion | Pin(s) | FSEL | Richtung | Beweis (Code-Offsets) |
|---|---|---|---|---|
| **UART0 TXD** | GPIO2 | 2 | AF | `0x1f0117e2` `fsel(2,2)` — UART-Dispatcher |
| **UART0 RXD** | GPIO3 | 2 | AF | `0x1f0117e2` `fsel(3,2)` |
| UART2 TXD/RXD (Alternative) | GPIO48 / GPIO49 | 2 | AF | `0x1f0117f0` / `0x1f0117fc` |
| **Lampen-PWM (GPT0)** | GPIO0, GPIO1, GPIO4, GPIO5 | 1 | out | `0x1f00e286…0x1f00e2d8`: `mode(pin,1)` + `fsel(pin,1)` + `dir(pin,1)` für 0/1/4/5 |
| **Board-Ausgänge** | GPIO44, GPIO45 | 0 | out | `0x1f03a04c…0x1f03a06c`: `fsel(44/45,0)`, `out(…,1)`, `dir(44,0)`, `dir(45,1)`; Variante `fsel(44/45,7)` bei `0x1f03a080/8c` (zweite Board-Revision) |
| SPI-/Flash-Gruppe | GPIO28–33 | 0 | — | `0x1f039cd4…0x1f039cfc` (`fsel(28..33, 0)`) |
| SSP0-Gruppe | GPIO24, 25, 26 | 3 / 0 | — | `0x1f03868e` `fsel(24,3)`; `0x1f0384f8/…/619/62c` `fsel(25|26, 0)` |

### Was die Phase-3.1-Annahmen angeht

| Phase-3.1-Claim | Befund 3.2 | Konsequenz |
|---|---|---|
| „LED-Pin = GPIO40“ | **widerlegt.** Im gesamten Binary existiert **kein** Accessor-Aufruf mit Pin 40 (alle Accessor-Aufrufe geprüft; konstante Pins: 0,1,2,3,4,5,24,25,26,28–33,44,45,48,49, dazu dynamische). Die Lampe hängt an **GPT0-PWM auf GPIO0/1/4/5**. | `mcufw` treibt keinen „LED-Pin 40“ mehr; Status-Heartbeat läuft auf dem belegten Board-Ausgang **GPIO44**. |
| „UART-Clock 40 MHz“ | **korrigiert → 38,4 MHz** (§4) | Divisor 21 statt 22. |
| Taster | **nicht über eine Konstante belegbar.** Kein `GPIO_ReadPinLevel(pin_const)`; die Taster-/IRQ-Pfade laufen über `GPIO_PinModeConfig` mit dynamischem Pin (`0x1f00f1e0`, `0x1f00f218`) und die Tabellenpfade `0x1f038c90/0x1f038d74/0x1f038e7e`. | Taster bleibt **offener Punkt** für Phase 3.3 (Interrupts), keine Behauptung im Code. |

---

## 4. GPIO-Register-Semantik — Korrektur eines Phase-3.1-Fehlers

`regs/gpio_reg.h` (3 Bänke × 32 Pins, ein Wort pro Bank) und die Disassembly
ergeben eindeutig:

```
0x00 + bank*4  GPLR   Input-Daten (ro)
0x0c + bank*4  GPDR   Richtung (ro, nur über GPSR/GPCR änderbar)
0x18 + bank*4  GPSR   Richtung setzen  (→ Ausgang)      [write-only]
0x24 + bank*4  GPCR   Richtung löschen (→ Eingang)      [write-only]
0x54 + bank*4  GSDR   Ausgangsdaten = 1                 [write-only]
0x60 + bank*4  GCDR   Ausgangsdaten = 0                 [write-only]
```

Beweis: `GPIO_WritePinOutput` (`0x1f03669c`) schreibt nach `0x54`/`0x60`;
`GPIO_SetPinDir` (`0x1f0366c8`) nach `0x18`/`0x24`.

**Phase-3.1-Bug:** `main.c` schrieb die *Daten* über `GPSR`/`GPCR` und
„initialisierte“ über `GPDR |= bit` (Register ist read-only). Damit wurde in
Wahrheit nur die **Richtung** umgeschaltet — der Pin hat nie einen Pegel
getrieben. In 3.2 behoben: `GPSR`/`GPCR` = Richtung, `GSDR`/`GCDR` = Pegel
(`mcufw_regs.h`, Abschnitt „GPIO“).

---

## 5. Vendor-Bibliothek + Startup

`libwmsdk.a` (`sdk/vendor/ez-connect-lite/sdk/libs/libwmsdk.a`, 8 815 352 B)
wird jetzt als **Archiv** (kein `--whole-archive`) in `mcufw` gelinkt:

```make
VENDOR_LIB := $(SDK_DIR)/sdk/libs/libwmsdk.a
$(CC) $(LDFLAGS) $(OBJS) $(VENDOR_LIB) -lgcc -o $@
```

Gezogen werden genau die benötigten Mitglieder — `mw300_pinmux.o` mit
`GPIO_PinMuxFun` und `GPIO_PinModeConfig` (dieses Objekt hat **null**
undefinierte Symbole). Der Pinmux-Pfad im Image läuft damit durch echten
Vendor-Code, nicht durch nachgebaute Stores:

```
$ arm-none-eabi-nm sdk/apps/mcufw/build/mcufw.elf | grep -E "GPIO_Pin|main"
1f00364c T GPIO_PinModeConfig
1f0035f4 T GPIO_PinMuxFun
1f00341c T main
$ make -C sdk/apps/mcufw check-vendor
== vendor link: 2 Symbole aus libwmsdk.a
```

Der neue Make-Target `check-vendor` (hängt an `container`) erzwingt:
`GPIO_PinMuxFun` **und** `GPIO_PinModeConfig` sind `T` (definiert) und es gibt
**genau eine** `main`-Definition im Image. Grund für die letzte Prüfung:
`libwmsdk.a` enthält `ttcp.o` mit einem lokalen `main`-Symbol — ein
`--whole-archive`-Link würde kollidieren.

**Vendor-Startup: im SDK-Snapshot nicht vorhanden** (verifiziert, keine
Annahme):

```
$ arm-none-eabi-nm libwmsdk.a | grep -cE "Reset_Handler|__Vectors|SystemInit"
0
$ arm-none-eabi-ar t libwmsdk.a | grep -iE "startup|vector|init"
mlan_init.o
mdev_startup.o        # WLAN-Stack-Init, kein CPU-Startup
```

Der Vektor-/Reset-Pfad (`startup_mw300.c` des Vendor-BSP) ist **nicht** Teil von
`ez-connect-lite`; das SDK liefert nur die Linker-Skripte
`build/toolchains/GNU/mw300.ld` und `mw300-xip.ld`. Bis das BSP-Startup
beschafft ist, bleibt `sdk/apps/mcufw/src/startup.c` (Vektor-Tabelle +
`Reset_Handler` → `main`, Entry `0x1f0032d9`) der funktionale Ersatz — bewusst
so dokumentiert statt ein fremdes Startup zu simulieren.

`mw300_gpio.o` (`GPIO_SetPinDir`/`GPIO_WritePinOutput`/`GPIO_ReadPinLevel`) ist
absichtlich **noch nicht** gezogen: es braucht die RAM-Callback-Tabelle
`intCbfArra` (Interrupt-Dispatch). Das gehört zu Phase 3.3 (Interrupts/Taster);
bis dahin nutzt `mcufw` die registergenaue direkte Form (§4).

---

## 6. UART-Taktung und Baudrate

### Takt: 38,4 MHz, nicht 40 MHz

`mw300_clock.h` pinnt den Hauptquarz des 88MW30x:

```c
#define CLK_MAINXTAL_FREQUENCY  38400000   /* 38.4 MHz */
```

Der UART-Takt ist entweder der langsame XTAL-Zweig oder der schnelle
PLL-Zweig, umgeschaltet über `CLK_SetUARTClkSrc(CLK_UART_ID_x, CLK_UART_FAST|SLOW)`
plus **fraktionalem** Teiler (`CLK_UARTDividerSet(CLK_UartSrc_Type,
CLK_Fraction_Type)`). `mcufw` programmiert den Clock-Baum nicht um — es läuft auf
dem, was `boot2` hinterlässt — deshalb ist die Taktfrequenz ein Parameter:

```c
#ifndef MCUFW_UART_CLK
#define MCUFW_UART_CLK 38400000u   /* Phase 3.1 hatte hier 40000000 */
#endif
```

Belege: `mw300_clock.h` (`CLK_MAINXTAL_FREQUENCY`, `CLK_UartSrc_Type` mit
`CLK_UART_FAST`/`CLK_UART_SLOW`, `CLK_SetUARTClkSrc`, `CLK_UARTDividerSet`);
`mw300_uart.o` (Archiv) ruft `CLK_GetUARTClkSrc`/`CLK_GetUARTDivider`/
`CLK_GetSystemClk` und rechnet daraus seinen Teiler — es gibt **keine**
hartkodierte Baudrate im Binary (Suche nach 115200/921600/9600 als Literal
und als Datenwort: 0 Treffer).

### Divisor (16× Oversampling, gerundet)

| Takt | Baud | Divisor | Ist-Baud | Fehler |
|---|---|---|---|---|
| **38,4 MHz** | 115200 | **21** (`0x15`) | 114285,7 | −0,79 % |
| 38,4 MHz | 9600 | 250 (`0xfa`) | 9600 | 0 % |
| ~~40 MHz (3.1)~~ | 115200 | 22 (`0x16`) | 113636,4 | −1,36 % |

Formel in `mcufw`: `div = (CLK + 8*BAUD) / (16*BAUD)`.

### Banner als Live-Nachweis

`mcufw` gibt die benutzten Werte und die **Pinmux-Readbacks** aus, damit ein
späterer Live-Test ohne Debugger prüfbar ist:

```
mcufw 0.2-phase3.2 -- no WLAN, no miIO, no cloud
boot-marker 0x4d435546
uart-clk 38400000 baud 115200 div 0x15
pinmux gpio2(txd) 0x2 gpio3(rxd) 0x2
pin gpio44 level 0
banner-ready
```

(`0x2` = `FSEL=2`, `bit15` gelöscht — genau das Muster, das der
Referenz-Dispatcher `0x1f0117d2` erzeugt.)

---

## 7. Verifikation (echte Läufe, rc=0)

```
$ make -C sdk                     # bootstrap entfällt, vendor/ liegt
== elf layout
  Entry point address: 0x1f0032d9
  LOAD 0x00100000 (seg1) / 0x1f0032d8 (seg2) / 0x20000040 (seg3)
.init 12816  .text 1304  .data 4  .bss 12
== container: 14324 Bytes
PASS .../mcufw_MRVL.bin: 41/41 Checks OK
```

Determinismus (zwei saubere ELF-Builds, `make -C apps/mcufw clean && make elf`):

```
build1=adaccac680f8b4974799012c7787ff892c151acfb6e85e62a655d899d86aa4ec
build2=adaccac680f8b4974799012c7787ff892c151acfb6e85e62a655d899d86aa4ec
DETERMINISTIC OK
```

Der Container ist erwartungsgemäß **nicht** byte-identisch (8 Bytes: Timestamp
`0x08–0x0b`, Entry `0x10–0x13` — `axf2firmware` schreibt `time(NULL)`), das ELF
ist es.

---

## 8. Offene Punkte → Phase 3.3

1. **Taster-Pin** über `GPIO_PinModeConfig`-Konstanten bzw. IRQ-Konfiguration
   (`0x1f038c90` ff.) bestimmen — braucht die Interrupt-Ebene.
2. **GPT0-PWM** (GPIO0/1/4/5) als Lampentreiber: GPT-Treiber aus `libwmsdk.a`
   (`mw300_gpt.o`) ziehen, `intCbfArra` bereitstellen.
3. **Vendor-Startup (`startup_mw300.c`)** aus dem Marvell-BSP beschaffen; bis
   dahin bleibt `startup.c` aktiv.
4. **GLK/RCU-Readback**: realen UART-Clock-Zweig im Banner auslesen
   (`CLK_GetUARTClkSrc`), statt ihn nur anzunehmen — erst mit UART-Zugriff am
   Gerät sinnvoll (Testträger bleibt bis Phase 5 unangetastet).
