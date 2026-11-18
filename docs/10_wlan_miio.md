# 10 — WLAN-Stack-Bringup (libwmsdk mlan/mdev) + miIO-kompatibler UDP-Kanal (54321)

Phase 3.4. Ziel war **nicht** die echte Assoziation (die ist Phase 5, Flash-Gate),
sondern: der WLAN-Init-Graph aus `libwmsdk.a` liegt nachweisbar im Image, ist
vollständig auflösbar, und der miIO-Kanal — der einzige OTA-Weg (docs/06 §0) —
existiert als prüfbares, deterministisches Stück Code.

Alle Befunde sind gegen den realen Build auf devastator belegt; die Kommandos
stehen in §6 und sind reproduzierbar.

## 1. Ergebnis

| Prüfung | Ergebnis |
| :--- | :--- |
| `make -C sdk` | rc=0 |
| Verifier `tools/verify_mrvl_container.py` | **41/41 PASS** |
| WLAN-Symbole im ELF | **199** aus `mlan_init.o` + `mdev_startup.o` |
| `arm-none-eabi-nm -u mcufw.elf` | **0** undefinierte Symbole |
| Host-Selftest (`make host-test`) | **13/13 PASS** |
| ELF über 2 Clean-Builds | byte-identisch `1dac6aac…0a9e1b` |
| Container | 197252 B, Load-Addr `0x00100000 / 0x1f0032d8 / 0x20000040` (= color1_0061) |

Der Container ist **nicht** byte-identisch zwischen Builds: `axf2firmware`
schreibt `time(NULL)` in Offset `0x08` (schon in docs/07 belegt). Deterministisch
ist das ELF — und damit alles, was das Gerät ausführt.

## 2. Der WLAN-Graph aus libwmsdk.a

Die Link-Kette, ausgehend von `mcufw_wlan_bringup()`:

```
mcufw_wlan_bringup()
  └── wlan_init_lock_list()      mlan_init.o   (Locks/Timer-Listen)
  └── wlan_init_adapter()        mlan_init.o   (Adapter-Alloc, mlan_ops)
  └── wlan_init_fw()             mlan_init.o   (Firmware-Download-Pfad)
  └── wlan_init_priv()           mlan_init.o
  └── wlan_init()                mlan_init.o   (Kommando-Kanal zum WLAN-MCU)
        mdev_startup.o liefert CLK_Init + boot_init (Uhr-/PMU-Basis)
```

Damit zieht `mlan_init.o` den kompletten `wlan_*`-Namensraum nach (Scan, Join,
11n, WMM, 11d/11h, Sniffer, Power-Save) — 199 `T`-Symbole. Das ist gewollt:
genau diese Fläche ist der Vertrag, den Phase 5 gegen den echten AP bedienen muss.

**Link-Kollision (gelöst):** `mdev_startup.o` definiert `Reset_IRQHandler`,
`Default_IRQHandler` und `HardFault_IRQHandler` erneut — dieselben Namen wie in
`startup.c` (Phase 3.1/3.2, deterministische `.data`/`.bss`-Initialisierung).
Bewusst **nicht** über `--allow-multiple-definition` gelöst, sondern eng:
`build/vendor/` enthält einen Archiv-Klon, in dem nur diese drei Symbole in
`mdev_startup.o` per `objcopy --redefine-sym` auf `mdev_*` umbenannt sind. Der
Rest des Members ist bitgleich, `CLK_Init`/`boot_init` behalten ihre Namen, und
das globale Link-Verhalten bleibt streng (echte Kollisionen fallen weiter auf).

**RTOS-Grenze:** `mcufw_os_ready()` (in `mcufw_shim.c`) gibt heute `0` zurück.
`mcufw_wlan_bringup()` läuft die Kette deshalb bis zur Stufe `needs-rtos` und
meldet das ehrlich, statt einen Join zu behaupten. Phase 5 setzt dort `1`,
sobald Scheduler und lwIP stehen.

**Konfiguration:** `mcufw_wlan_cfg_t` liegt in `.nvram`
(`0x480c0000`, NOBITS, 256 B, **nicht** Teil des Containers) — SSID (32 B),
PSK (63 B), `muid`, `provisioned`. Sie überlebt einen Reset und ist genau das
Struct, das der miIO-Provisioning-Handler (3.5) schreiben wird. Compile-Time-
Defaults via `-DMCUFW_WLAN_DEFAULT_SSID=…` / `-DMCUFW_WLAN_DEFAULT_PSK=…`.
Ist der NVRAM-Magic `0x4d435746` nicht gesetzt, greifen die Defaults (leer).

## 3. miIO-Kanal (Port 54321)

`mcufw_miio.c` ist eine **reine Funktion über Byte-Puffern**: kein Socket, kein
RTOS, kein globaler Zustand außer dem übergebenen `mcufw_miio_ctx_t`. Das ist
der Grund, warum derselbe Code im Host-Test und auf dem Target läuft.

**Header (32 B), wie auf der Leitung beobachtet:**

| Off | Feld | Wert |
| :--- | :--- | :--- |
| 0 | magic | `0x2131` (LE) = `31 21` |
| 2 | length | Gesamtpaketlänge, inkl. Header |
| 4 | (unbekannt) | wird ignoriert, im Reply 0 |
| 8 | did | `0x05eb8034` (Referenzlampe `.115`) |
| 12 | stamp | monoton, Replay-Schutz |
| 16 | checksum | `MD5(header[0..16) ‖ token)` |

**Hello (unverschlüsselt):** Request = 32 B, Body null. Reply = 32 B, Bytes
`16..31` = Checksum. Der Token selbst steht **nicht** auf der Leitung — die
Checksum beweist lediglich, dass beide Seiten denselben Token kennen. Ist das
Gerät ungepaart, ist der Token `ff…ff`; genau das liefert `.115` heute
(docs/06 §0). Daraus folgt: ein Hello ohne vorher bekanntes Token ist **kein**
Provisioning — der Kanal kann nur bestätigen, nicht vergeben.

**Verschlüsselte Requests** (`miIO.*`): Body = AES-128-CBC, Padding PKCS#7,
`key = MD5(token)`, `iv = MD5(key ‖ token)` — die in docs/06 §3 belegte
Wire-Krypto, hier in `mcufw_crypto.c` selbst implementiert, damit kein Vendor-
Kontextlayout geraten werden muss.

**Guards (jeder mit eigenem Return-Code, siehe `mcufw_miio.h`):**
`E_SHORT` (<32 B) → `E_MAGIC` → `E_LEN` (Längenfeld ≠ Datagrammlänge) →
`E_CHECKSUM` → `E_TOKEN_MISSING` (verschlüsselt, Token `ff…ff`) →
`E_CRYPTO` (Padding/Entschlüsselung) → `E_JSON` (kein `method`) →
`E_UNSUPPORTED` (nicht in der miIO-Tabelle) → `E_ROOM`.
Ein abgelehntes Paket erzeugt **keine** Antwort (still drop), nur einen Zähler —
so verhält sich die Referenzlampe auch.

**`miIO.ota` ist der einzige OTA-Kanal** (docs/06 §0: LAN-55443 ist gegen OTA
doppelt gesperrt, miIO nicht). In 3.4 wird die Methode erkannt, gezählt und
quittiert; Download/Flash folgt in Phase 5.

**Transport:** `lwip_socket/bind/recvfrom` sind **nicht** Teil dieser Unit — lwIP
ist in 3.4 nicht gelinkt (§4). Der Socket-Loop landet mit dem RTOS/lwIP-Bringup
und ruft dann nur noch `mcufw_miio_handle()` pro Datagramm.

## 4. Was hier bewusst fehlt (Phase 5)

| Offen | Warum | Wo es hängt |
| :--- | :--- | :--- |
| Echte Assoziation, DHCP, IP | braucht Scheduler + lwIP + WLAN-Firmware im Flash | `mcufw_os_ready()` = 0 |
| UDP-Transport (Socket-Loop) | lwIP-Core nicht gelinkt | dito |
| Provisioning (Token/SSID schreiben) | setzt funktionierenden miIO-Empfang voraus | 3.5 + Phase 5 |
| Lan-JSON-RPC (55443), MQTT | explizit **nicht** 3.4 | 3.5 |
| `mcufw_shim.c`-Ersatz | FreeRTOS/lwIP-Shims sind inert, aber 25 Symbole + Heap-Bump-Allocator sind Attrappe | Phase 5 ersetzt sie durch die echten Objekte |

Die Shims sind der ehrlichste Teil des Meilensteins: sie sind **nicht** „fertig“,
sie machen nur sichtbar, welche Fläche der SDK tatsächlich verlangt. Jeder
Eintrag in `mcufw_shim.c` nennt das Archiv-Mitglied, das ihn zieht.

## 5. Sicherheitsgrenzen dieses Meilensteins

- Kein Flash, kein OTA, kein Netzverkehr: `.115` wurde **nicht** angefasst, es
  gibt keinen Socket. Der komplette Nachweis ist statisch (ELF, `nm`) plus
  Host-Ausführung der Krypto-/Parser-Pfade.
- Der did `0x05eb8034` ist nur als Konstante in `main.c` verdrahtet (Anzeige +
  Testpaket im RAM). Er wird nicht gesendet.
- NVRAM wird **nicht** geschrieben — `.nvram` ist NOBITS und bleibt unberührt.

## 6. Reproduktion

```bash
ssh devastator
cd /home/jarvis/projects/yeelight-re

make -C sdk                    # build + container + Verifier (41/41)
make -C sdk/apps/mcufw host-test   # 13/13 PASS auf dem Host

# WLAN-Nachweis
arm-none-eabi-nm sdk/apps/mcufw/build/mcufw.elf \
  | grep -E ' T (wlan_init|wlan_init_adapter|CLK_Init|boot_init)$'
arm-none-eabi-nm -u sdk/apps/mcufw/build/mcufw.elf | wc -l   # -> 0

# Determinismus (ELF)
cd sdk/apps/mcufw
for i in 1 2; do make clean >/dev/null && make all >/dev/null && sha256sum build/mcufw.elf; done
```
