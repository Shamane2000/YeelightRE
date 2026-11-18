# 09 — Taster-Ebene, GPT0-Lampen-PWM und Vendor-Startup (Phase 3.3)

Status: **abgeschlossen (Build-Nachweis grün)** · Basis: Phase-3.2-Fundament
(`make -C sdk` rc=0, Pinmux belegt, UART 38,4 MHz, `libwmsdk.a` gelinkt).
Referenz-Binary: `firmware/color1_142_0061/color1_0061.elf` (ARM EABI5, not
stripped) auf devastator. Werkzeug: `tools/irq_gpt_re.py`
(`vec`/`calls`/`at`/`words`/`refs`/`range`/`table`/`hexdump`).

> Harte Randbedingung eingehalten: **reiner Build auf devastator**, kein Flash,
> kein Gerätezugriff, kein Öffnen. Testträger `.115` unangetastet.

---

## 1. Taster-Pin: Ergebnis der Interrupt-Ebene

**Befund: color1 (Glühbirne) hat keinen GPIO-Taster.** Der Taster-Pfad ist in
diesem Image nicht „noch offen", sondern **beweisbar nicht vorhanden**. Die
Interrupt-Ebene wurde dafür vollständig ausgewertet:

| # | Prüfung | Ergebnis |
|---|---|---|
| 1 | GPIO-IRQ-Pfad existiert | IRQ 9 GPIO → Handler `0x1f0379c4` (Vektorslot 25, Thumb-Zeiger `0x1f0379c5`). Handler liest `GEDR` (`0x46060048 + bank*4`), quittiert über `GCRER`/`GCFER` (`0x78`/`0x90`) |
| 2 | Callback-Verteilung | Handler dispatcht aus dem Vendor-Array `gpio_irq_obj_list` `@0x20017f5c`, Stride 8 Byte `{fn, arg}`, 50 Einträge (GPIO0…GPIO49) → 400 Byte bis `0x200180ec` (zwei Literale im selben Modul) |
| 3 | Registrierungssstellen | **0** — der einzige Literal-Treffer auf `0x20017f5c` im gesamten Image liegt im Literal-Pool des Handlers selbst (`code 0x1f037a20`). Kein Pin-Interrupt wird je scharf geschaltet |
| 4 | Pegel-Lesezugriff | `GPIO_GetPinLevel` `@0x1f0366f2` (liest `GPLR` `0x46060000`) hat **null** Aufrufstellen |
| 5 | Indirekte Registrierung | Scan aller PROGBITS-Datensektionen auf Thumb-Funktionszeiger in den GPIO-Layer: **0 Treffer** → auch keine Dispatch-Tabelle |
| 6 | Echte Eingabe-IRQs im App-Image | nur die PMU-Wakeup-Pins **ExtPin0/ExtPin1** (IRQ 0/1 → Handler `0x0010181d` / `0x00101859`, Vektorslots 16/17). Sie quittieren das PMU-Wakeup-Statuswort (`PMU + 0x180`, ein Bit pro Pin) — AON-/Wakeup-Ebene, **nicht** GPIO |

Konsequenz für `mcufw` (`mcufw_regs.h`):

```c
#define MCUFW_IRQ_EXTPIN0  0    /* PMU wakeup pin 0 */
#define MCUFW_IRQ_EXTPIN1  1    /* PMU wakeup pin 1 */
#define MCUFW_IRQ_GPIO     9
#define MCUFW_GPIO_IRQ_ARRAY   0x20017f5cu   /* 50 x 8 Byte {fn, arg} */
#define MCUFW_GPIO_IRQ_STRIDE  8u
#define MCUFW_GPIO_PIN_COUNT   50u
#define MCUFW_BUTTON_GPIO_PIN  (-1)          /* bewiesen: kein GPIO-Taster */
```

Die GPIO-Interrupt-Infrastruktur (Register, IRQ-Slot, Array-Layout) ist damit
korrekt dokumentiert und in `mcufw` verfügbar; es wird aber **kein Taster-Pin
behauptet**. Der Banner gibt stattdessen den echten Zustand aus
(`GRER`/`GFER` = 0 ⇒ keine Taster-IRQ-Quelle scharf).

**Offen (Phase 3.4):** Zuordnung der PMU-Wakeup-Pads ExtPin0/ExtPin1 zu
physischen Gehäusepins (AON-Registerblock, `PMU + 0x180`) — nur mit
Board-Zugriff oder weiterem Referenz-Image (z. B. einer Variante mit Taster)
auflösbar.

---

## 2. GPT0-PWM-Treiber als Lampen-Treiber

Die Lampe hängt **nicht** an einem GPIO, sondern an vier GPT0-PWM-Kanälen:

| Kanal | GPIO | Pinmux | Beleg |
|---|---|---|---|
| GPT0_CH0 | 0 | FSEL 1 | Init-Tabelle `@0x1f00f1c4`, Init-Code `0x1f00e286…0x1f00e2d8` |
| GPT0_CH1 | 1 | FSEL 1 | dito |
| GPT0_CH4 | 4 | FSEL 1 | dito |
| GPT0_CH5 | 5 | FSEL 1 | dito |

- **Periode** `0xC350` (50000 Zähler-Takte) — in jedem der vier Tabelleneinträge.
- **Dispatch** `FUN_1f0110a0` (Modus 1/2/3, Vtable-Slot `+0x44`), getrieben vom
  LED-Timer (200 ms); Helligkeit wird als `0xC350 − level` an den Treiber
  gegeben ⇒ **String ist low-aktiv** (dunkel = Kanalausgang high).
- **Register** (`regs/gpt_reg.h`): Basis `0x46070000` (Gegenprobe: einziges
  Literal `0x46070000` im Image `@0x1f048ffc`), Kanalblock `0x200 + ch*0x30`,
  `CH_CNTL +0x00` (CH_IO `[2:0]` = 6 = PWM edge, `POL` bit 16), `CMR0 +0x10`
  (Duty-Vergleich), `CMR1 +0x20`, `UPPVAL 0x060`, `CLK_CNTL 0x080`,
  `CNT_EN 0x000` (Start/Stop/Reset).

Übernommen in `sdk/apps/mcufw/src/mcufw_gpt.c` + `mcufw_gpt.h`:

```c
void mcufw_gpt0_pwm_init(void);          /* Clock-Gate, Periode, 4 Kanäle, Start */
void mcufw_lamp_set(uint16_t level);     /* 0 = aus … 0xC350 = voll, low-aktiv    */
uint16_t mcufw_lamp_get(void);           /* Software-Readback                     */
uint32_t mcufw_lamp_cmr0(unsigned i);    /* Hardware-Readback CMR0 je Kanal       */
uint32_t mcufw_lamp_counter(void);       /* GPT0 CNT_VAL                          */
```

**Bewiesen vs. angenommen** (bewusst getrennt, damit nichts als Faktum gilt,
was es nicht ist):

- *bewiesen*: Basis, Kanalblock/Stride, CMR0-Offset, `CH_IO = 6`,
  `UPPVAL = 0xC350`, invertierte Duty, 4 Kanäle auf GPIO0/1/4/5 FSEL 1
- *angenommen*: `CLK_CNTL`-Felder (`CLK_SRC 0`, `CLK_DIV 0`, `CLK_PRE 0`) und
  das CSU-Clock-Gate-Bit `CSU_GATE_GPT0 = (1u<<6)` — die App überlässt beides
  dem SDK-`GPT_Init()`, dessen Konfigurationsstruktur **nicht** in `.data`
  liegt (`0xC350`-Fenster-Scan über alle Datensektionen: 0 Treffer, die Struct
  wird auf dem Stack gebaut). Beide Werte sind in `mcufw_gpt.c` als `#define`
  mit Kommentar hinterlegt und müssen bei erstem Hardware-Zugriff bestätigt
  werden.

---

## 3. Vendor-`startup_mw300.c`: nicht beschaffbar → dokumentierter Ersatz

Verifiziert (Phase 3.2, hier bestätigt): im SDK-Snapshot gibt es **kein**
`Reset_Handler`, `__Vectors` oder `SystemInit` — weder in `libwmsdk.a`
(`ar t`: nur `mlan_init.o`, `mdev_startup.o`, …) noch als Quelle. Marvell hat
den Startup in `boot2` (Flash-ROM) verlagert; die App liefert nur die
Vektortabelle.

**Ersatz in `sdk/apps/mcufw/src/startup.c` (funktional äquivalent, dokumentiert):**

| Anforderung | Umsetzung in `startup.c` |
|---|---|
| Vektortabelle an der Container-Adresse | `.isr_vector` → SRAM0 `0x00100000`, 64 Slots (16 Exceptions + 48 IRQs) |
| Reset-Einstieg = Container-Entry | `Reset_IRQHandler` in `.text.Reset_IRQHandler`, per Linkerscript erster `.text`-Block ⇒ `e_entry = 0x1f0032d9` (Thumb) |
| VTOR umbiegen | `REG32(SCB_VTOR) = MCUFW_INIT_BASE` vor jedem C-Code |
| `.data`/`.bss` initialisieren | Kopier-/Null-Schleife über `_sidata/_sdata/_edata/_sbss/_ebss` |
| Fremd-IRQ | alle 48 IRQ-Slots auf `Default_IRQHandler` (Traps statt Garbage) |

Gegenprobe am Referenz-Image: Vektorslot 1 (Reset) = `0x1f0032d9` — genau die
Konvention, die `mcufw` erzeugt (Verifier-Check „entry thumb-bit convention"
und „entry point equals app segment load address" PASS).

---

## 4. Korrektur eines Phase-3.2-Befunds (GPIO Daten vs. Richtung)

Phase 3.2 hatte den Ausgabepfad als „`GSDR`/`GCDR` (Daten) + `GPSR`/`GPCR`
(Richtung)" dokumentiert — **das ist vertauscht**. Belege:

- `regs/gpio_reg.h`: `0x0c-0x14` = *Pin Direction Register* (`GPDR`),
  `0x54-0x5c` = *Bitwise Set Direction* (`GSDR`), `0x60-0x68` = *Bitwise Clear
  Direction* (`GCDR`), `0x18-0x20` = *Pin Set* (`GPSR`), `0x24-0x2c` = *Pin
  Clear* (`GPCR`).
- Vendor-API-Namen (`mw300_gpio.h`): `GPIO_SetPinDir()` → `GSDR`/`GCDR`,
  `GPIO_WritePinOutput()` → `GPSR`/`GPCR`.
- Referenz-Zugriffe: `0x1f03669c(pin,1)` → `0x46060054` (`GSDR`),
  `0x1f03669c(pin,0)` → `0x46060060` (`GCDR`), `0x1f0366c8(pin,1)` →
  `0x46060018` (`GPSR`), `0x1f0366c8(pin,0)` → `0x46060024` (`GPCR`),
  `0x1f0366f2(pin)` → `0x46060000` (`GPLR`).
- Plausibilitätsprobe: die LED-Helfer `0x1f011818` („off") und `0x1f011836`
  („on") setzen *beide* zuerst die Richtung auf Ausgang und unterscheiden sich
  nur im Datenwort — unter der alten (vertauschten) Lesart wären beide Pfade
  wirkungslos.

`mcufw_regs.h`, `mcufw_gpt.c` und `main.c` verwenden jetzt die korrekte
Semantik (`gpio_out_init` → `GSDR`, `gpio_out_set` → `GPSR`/`GPCR`).

---

## 5. Build- und Verifikationsnachweis (devastator, 192.168.1.154)

```
$ make -C sdk clean && make -C sdk            # 2x hintereinander
build1_rc=0
build2_rc=0
elf1=6c8c6542b37964fd682651cb98057ad049234c03d8b044e58e145c3747f6907d
elf2=6c8c6542b37964fd682651cb98057ad049234c03d8b044e58e145c3747f6907d
DETERMINISTIC=YES
```

- Verifier (`tools/verify_mrvl_container.py`): **PASS, 41/41 Checks OK**
  (magic2 `0x2e9cf17b` identisch, 3 Segmente, Ladeadressen
  `0x00100000 / 0x1f0032d8 / 0x20000040`, Segmenttypen `[2,2,2]`,
  Entry `0x1f0032d9`).
- Erwartete, begründete Abweichungen: Image-Größe 15004 B vs. 311836 B
  (mcufw ist absichtlich minimal), Container-Timestamp (`axf2firmware`
  schreibt `time(NULL)`; deshalb wird die **ELF**-Determinismus-Prüfung
  verglichen, nicht der Container).
- Neue Symbole im Image: `mcufw_gpt0_pwm_init`, `mcufw_lamp_set`,
  `mcufw_lamp_get`, `mcufw_lamp_cmr0`, `mcufw_lamp_counter`
  (`check-vendor` prüft `T mcufw_gpt0_pwm_init` + `T mcufw_lamp_set` als
  hartes Gate).
- Layout unverändert: `.init 12816` (SRAM0), `.text 1984` (XIP), `.data 4`
  (SRAM1), `.bss 16`.
- Vektor-Readback aus dem Referenz-Image (`irq_gpt_re.py vec`): Slot 1 Reset
  `0x1f0032d9`, Slot 25 IRQ9 GPIO `0x1f0379c5`, Slot 30 IRQ14 GPT0
  `0x1f037995`, Slot 34 IRQ18 UART0 `0x1f037289` — passt zu den IRQ-Konstanten
  in `mcufw_regs.h`.

---

## 6. Offene Punkte (Phase 3.4)

1. **PMU-Wakeup-Pad-Mapping** ExtPin0/ExtPin1 → physische Pins (AON-Block
   `PMU + 0x180`); nur mit Board-Zugriff oder Taster-Variante auflösbar.
2. **GPT0-Clock-Bestätigung**: `CLK_CNTL` (Divider/Prescaler) und
   `CSU_GATE_GPT0` gegen echtes Silizium prüfen — solange nicht bestätigt,
   keine Aussage über die resultierende PWM-Frequenz.
3. **PWM-Frequenz**: aus `0xC350` und der (noch unbestätigten) Taktquelle
   ableitbar; erst nach (2) belastbar.
