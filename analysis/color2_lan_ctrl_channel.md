# Kanal-Analyse: Wohin geht `set_ps ["cfg_lan_ctrl","1"]`, wenn LAN-Control aus ist?

**Firmware:** `yeelink.light.color2`, `fw_ver 2.0.6_0065` (Build `142_0027`), Segmente
`seg1 0x00100000`, `seg2 0x1f0032d8`, `seg3 (data) 0x20000040`
**Task:** t_c1c72324 · **Datum:** 2026-11-17 · **Methode:** Ghidra-Dekomplilat
(`ghidra_workspace/export_color2/ALL.c`), Segment-Dumps, Live-Messungen (`docs/01_live_findings.md`)

---

## 1. Kurzantwort

`set_ps ["cfg_lan_ctrl","1"]` wird **niemals** über den LAN-Kanal (TCP `55443`, JSON-RPC)
akzeptiert — dort greift eine statische 20-Methoden-Whitelist ohne `set_ps`/`get_ps`/
`config_router`.

Der Kanal, der es akzeptiert, ist die **miIO-JSON-RPC-Schicht über UDP `54321`**
(Xiaomi-miIO-Protokoll: AES-128-CBC, Token-/`hello`-Handshake, DID-basiert).
Sie benutzt **dieselbe RPC-Methoden-Registry** wie der LAN-Pfad, aber **ohne** die
LAN-Whitelist — deshalb ist `set_ps` (und sogar das privilegierte Modul `miIO` mit
`config_router`, `xget`/`xset`) dort erreichbar.

**App-frei aktivierbar?** Ja — aber nur solange die Birne im **Setup-AP-Mode** ist:
dort liefert `miIO hello` den 16-Byte-Token im Klartext, und genau damit signiert man
den `set_ps`-Request. Sitzt die Birne bereits im Heim-WLAN mit `lan_ctrl = 0`
(Station-Mode), antwortet `hello` mit `ff..ff` → ohne App/Cloud-Token kein app-freier
Weg. Der Einstieg ist dann ein Factory-Reset (= zurück in den AP-Mode).

---

## 2. Beweiskette (Firmware, Ghidra-Adressen)

### 2.1 Zwei getrennte RPC-Pfade auf eine Registry

| Pfad | Einstieg (Code) | Vor dem Dispatch | Erreichbare Methoden |
| :-- | :-- | :-- | :-- |
| LAN TCP `55443` | `FUN_1f010250` (Server) → `FUN_1f010094` (recv/JSON-Zeilen) | **`FUN_1f00fec4` Whitelist-Gate** | nur die 20 Whitelist-Namen |
| miIO UDP `54321` | `FUN_1f00a6c0` (Token/DID-Setup) → `FUN_1f014b64` (Session) | Modul-Registry-Lookup (`<modul>.<methode>`) | Modul `miIO` (16) + Modul `user` (31) |

Beide Pfade enden in derselben 47-Methoden-Tabelle; der Unterschied ist ausschließlich
das Whitelist-Gate im LAN-Pfad.

### 2.2 Die LAN-Whitelist `0x200009e0` (20 Namen, statisch, NULL-terminiert)

Strings liegen bei `0x1f0441ad` ff. (Klartext-Namen, **ohne** `_JsDlgt`-Suffix):

```
get_prop, set_default, set_power, toggle, set_bright, start_cf, stop_cf, set_scene,
cron_add, cron_get, cron_del, set_ct_abx, set_rgb, set_hsv, set_adjust,
adjust_bright, adjust_ct, adjust_color, set_music, set_name
```

`NULL`-Terminator bei `0x20000a30` (20 Zeiger). Die Tabelle ist ein `.data`-Konstrukt
(`0x20000040`-Segment) und wird nirgends zur Laufzeit befüllt → sie gilt in **jedem**
Betriebsmodus (AP wie Station), es gibt keinen Provisioning-Sonderzweig.

Gate-Funktion `FUN_1f00fec4` (0x1f00fec4, 202 Byte), Kern:

```c
piVar2 = DAT_1f00ff90;                 /* = 0x200009e0 Whitelist */
do {
  piVar6 = piVar2;
  if (*piVar6 == 0) break;             /* NULL -> Ende */
  iVar3 = FUN_1f01bcd4(param_3);       /* strcmp(method, *piVar6) */
  piVar2 = piVar6 + 1;
} while (iVar3 != 0);
if (*piVar6 != 0) {                    /* Treffer -> weiter dispatchen */
  ...
  FUN_1f01b152(auStack_100,0x20,DAT_1f00ff94,param_3);  /* "%s_JsDlgt" */
  puVar8 = DAT_1f00ff9c; uVar1 = DAT_1f00ff98;          /* 0x00103118 / 31 */
  for (uVar7 = 0; (int)uVar7 < (int)(uVar1 >> 3); uVar7 = uVar7 + 1)
    iVar3 = FUN_1f01bcd4(puVar8[1],auStack_100);        /* Tabellen-Name vs. "%s_JsDlgt" */
  ...
}
/* kein Treffer -> Rueckgabe 2 = "method not supported" */
```

Aufrufer von `FUN_1f00fec4` — vollständig, nur **zwei** Stellen:

* `ALL.c:17580` in `FUN_1f010094` — der TCP-LAN-Client-Handler (`FUN_1f030058` recv,
  Zeilensplit an `DAT_1f01023c`).
* `ALL.c:17496` in `FUN_1f010074` — interner Loopback-Dispatch (`param_5 = 1`) für die
  LAN-State-Push-Logik, keine externe Angriffsfläche.

→ Kein anderer Codepfad benutzt diese Whitelist; umgekehrt heißt das: der miIO-Pfad
kommt an `FUN_1f00fec4` gar nicht vorbei.

### 2.3 LAN-Server und Port 55443

`FUN_1f010250` (LAN-Server-Steuerung, 570 Byte): Nachricht `0x00` = einschalten,
`0x01` = ausschalten.

```c
iVar4 = FUN_1f0301d0(2,1,6);        /* socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) */
FUN_1f0304b8(iVar4,0xfff,4,&local_254,4);   /* SO_REUSEADDR */
local_24f = 2;                      /* sin_family  */
local_24e = 0x93d8;                 /* Port, LE-Bytes d8 93 -> Netzwerk 0xd893 */
iVar5 = FUN_1f02fcbc(iVar4,&uStack_250,0x10);  /* bind() */
iVar5 = FUN_1f02fe10(iVar4,2);      /* listen() */
```

Byte-Order-Kalibrierung an bekannten Ports: SSDP-Ziel ist als Literal `0xbe07`
(`ALL.c:18114`, `FUN_1f010864(...,0xbe07,...)`) geschrieben, das sind die LE-Bytes
`07 be` = Port `0x07be` = **1982**; im selben SSDP-Advert wird der LAN-Port als
`0xd893` (`ALL.c:18112`) übergeben. Damit ist `0x93d8` = **`0xd893` = 55443**.
`SO_REUSEADDR` + `listen(backlog=2)` passen zu einem einzelnen LAN-RPC-Port.

### 2.4 Die RPC-Registry: Modul `miIO` (16) + Modul `user` (31)

`FUN_1f005db8` registriert beide Module über die Descriptor-Strukturen
`0x200014b4` (Name `"miIO"` @ `0x1f042a29`) und `0x200014f0` (Name `"user"` @
`0x1f042a2e`). Registrier-Mechanik: `FUN_1f018b98` setzt den Global-Slot
`0x20002ba8` auf das `miIO`-Descriptor, `FUN_1f018c5c` hängt das `user`-Descriptor
dahinter (verkettete Modul-Liste, Kopf `0x20002ba8`).

Descriptor-Layout (Auszug): `+0x00` next, `+0x10` Methoden-Tabelle, `+0x18`
Tabellen-Länge (Bytes, `>>3` = Einträge; Eintrag = `{handler, name_ptr}`).

**Modul `miIO` — Tabelle `0x00103098`, 16 Einträge (privilegiert):**

| # | Methode | Handler | # | Methode | Handler |
| :- | :-- | :-- | :- | :-- | :-- |
| 0 | `test` | 0x1f004cbd | 8 | `get_ota_progress` | 0x1f009d7d |
| 1 | `config` | 0x1f004cf1 | 9 | `ota` | 0x1f0097e9 |
| 2 | `info` | 0x1f00555d | 10 | `reboot` | 0x1f0097bd |
| 3 | `restore` | 0x1f004de5 | 11 | **`config_router`** | 0x1f00a085 |
| 4 | `rpc_test` | 0x1f0055fd | 12 | `xgetKeys` | 0x1f00ac11 |
| 5 | `stop_diag_mode` | 0x1f006591 | 13 | `xget` | 0x1f00b0d9 |
| 6 | `wifi_assoc_state` | 0x1f0065b1 | 14 | `xdel` | 0x1f00afd9 |
| 7 | `get_ota_state` | 0x1f00977d | 15 | `xset` | 0x1f00b3e9 |

**Modul `user` — Tabelle `0x00103118`, 31 Einträge (u. a. die Whitelist-Namen plus
`get_ps`/`set_ps`/`ylk_config_router`):**

| # | Methode | Handler | # | Methode | Handler |
| :- | :-- | :-- | :- | :-- | :-- |
| 0 | `suicide` | 0x1f00c995 | 16 | `cron_add` | 0x1f00d7d9 |
| 1 | `set_name` | 0x1f00e0e9 | 17 | `set_adjust` | 0x1f00d6a1 |
| 2 | `set_indicator` | 0x1f00da19 | 18 | `toggle` | 0x1f00d671 |
| 3 | `set_music` | 0x1f00e055 | 19 | `set_default` | 0x1f00d641 |
| 4 | `set_alert` | 0x1f00dfb1 | 20 | `set_scene` | 0x1f00dc11 |
| 5 | `ylk_config_router` | 0x1f00cdf5 | 21 | `stop_cf` | 0x1f00d611 |
| 6 | `adjust_color` | 0x1f00decd | 22 | `start_cf` | 0x1f00da95 |
| 7 | `adjust_ct` | 0x1f00de89 | 23 | `set_ct_abx` | 0x1f00d571 |
| 8 | `adjust_bright` | 0x1f00de45 | 24 | `set_xy` | 0x1f00d4c5 |
| 9 | **`get_ps`** | 0x1f00d9a1 | 25 | `set_wrgb` | 0x1f00d419 |
| 10 | **`set_ps`** | **0x1f00d92d** | 26 | `set_hsv` | 0x1f00d355 |
| 11 | `get_aging_status` | 0x1f00cc45 | 27 | `set_rgb` | 0x1f00d2a5 |
| 12 | `miio_test_exit` | 0x1f00df4d | 28 | `set_bright` | 0x1f00d1f5 |
| 13 | `miio_test` | 0x1f00df15 | 29 | `set_power` | 0x1f00d0fd |
| 14 | `cron_get` | 0x1f00d8c5 | 30 | `get_prop` | 0x1f00ca45 |
| 15 | `cron_del` | 0x1f00d85d | | | |

Tabellen-Namen tragen den Suffix `_JsDlgt` (z. B. `"set_ps_JsDlgt"` @ `0x1f043b72`) —
genau den baut sich der Dispatcher aus dem Request (`"%s_JsDlgt"`, Format-String
@ `0x1f043f27`, referenziert über den Pool `0x1f00ff94`).
**Fazit:** `set_ps` ist eine reguläre RPC-Methode des Moduls `user` — sie wird nur im
LAN-Pfad herausgefiltert, existiert aber im Gerät.

### 2.5 Der miIO-Stack (UDP 54321)

* Port-Konstante: `.data` `0x2000029a` = `u16 0xd431` = **54321** (Port-Tabelle im
  `0x20000040`-Segment; dieselbe Region hält weitere Dienst-Ports).
* `FUN_1f00a6c0` (mDNS/SSDP- und Token-Setup, 592 Byte): initialisiert DID/Token
  (`FUN_1f02a804`), advertet den Dienst, und startet die miIO-Session mit
  `FUN_1f014b64(puVar2[1], ..., puVar8 /*8B*/, puVar7 /*16B Token*/, ...)`.
  Die Verzweigung `FUN_1f003a54()` unterscheidet **AP-Mode** (`== 0`) von
  **Station-Mode** — der miIO-Listener wird in **beiden** Modi gestartet.
* Kette (aus `docs/05_color2_firmware_re.md`, unverändert gültig):
  `FUN_1f014b64` → `FUN_1f017a9c` → `FUN_1f018d58(port)` = UDP-Listener.
* Methoden-Selektor: das miIO-JSON trägt modul-qualifizierte Namen (`"%s.%s"`,
  Format-String @ `0x1f043ab2`). Live belegt: `miIO.info`, `miIO.config_router`
  funktionieren über UDP; ein **nackter** Methodenname (`set_ps`) landet im
  Default-Modul `user`.

---

## 3. Live-Nachweis (echte Messungen, `docs/01_live_findings.md`)

| Gerät | Modus | `hello`-Token | `set_ps ["cfg_lan_ctrl","1"]` | Readback | TCP 55443 |
| :-- | :-- | :-- | :-- | :-- | :-- |
| `color2` `did=05eb8034` (`2.0.6_0065`) | AP `192.168.10.1` | **Klartext** `0f27f4ec…2d1e` | `{"id":11,"result":["ok"]}` | `get_prop ["lan_ctrl"]` → `["1"]` | nach `config_router` offen (`192.168.1.115`) |
| `color4` `did=1023ce17` (`fw 39`) | AP `10.10.1.1` | Klartext | `{"id":11,"result":["ok"],"exe_time":10}` | `["1"]` | offen (`192.168.1.116`) |
| `color` `did=0371f839` (`fw 76`) | Station, LAN **an** | `0000…0000` | n. v. | `lan_ctrl` aktiv | offen |
| `color2` `did=05eb8034` | Station, LAN **aus** (nach Reset) | `ffff…ffff` (ab `stamp=9`) | alter AP-Token → Timeout | – | `Connection refused` |

Der AP-Mode-Fall ist der Beweis für den Kanal: derselbe Request, der über TCP 55443 mit
`code 2 (method not supported)` abgewiesen wird, liefert über UDP 54321 `["ok"]` und
schaltet `lan_ctrl` real auf `1` (verifiziert per `get_prop`, nicht per Handschalter).

---

## 4. Konkreter app-freier Ablauf (implementiert)

`tools/yeelight_standalone.py` (Funktion `miio_send`, Paketformat + AES-CBC wie in
`MiioLocalAPI` der APK):

1. Birne in Setup-AP bringen (Factory-Reset) → SSID `yeelink-light-color2_miapXXXX`.
2. `miIO hello` an `<AP-IP>:54321` → **16-Byte-Token im Klartext**, dazu DID/Stamp.
3. `{"id":1,"method":"set_ps","params":["cfg_lan_ctrl","1"]}` → `{"result":["ok"]}`.
4. `{"id":2,"method":"get_prop","params":["lan_ctrl"]}` → `["1"]` (Verifikation).
5. `{"id":3,"method":"miIO.config_router","params":{"ssid":…,"passwd":…,"uid":0,"tz":"Europe/Berlin"}}` → `["ok"]`.
6. Birne joint das WLAN; danach sind TCP `55443` und SSDP `1982` sofort offen.

`set_developer_mode` (python-miio-Wrapper) ist **nicht** der Geräte-RPC — der echte
Aufruf ist `set_ps ["cfg_lan_ctrl","1"]`.

---

## 5. Grenzen dieser Analyse (ehrlich)

* **Nicht statisch aufgelöst:** die konkrete Dispatch-Funktion des miIO-Pfads. Die
  Modul-Liste (`0x20002ba8`) wird im Dekompilat nur in den beiden Registrier-Funktionen
  referenziert; der lesende Dispatcher greift über eine Alias-/Pool-Adresse zu, die mit
  dem vorhandenen `le32`-Xref-Tool (`tools/color2_xref.py`) nicht auflösbar ist
  (Thumb-`movw`/`movt`-Immediates werden nicht gescannt). Der Pfad ist deshalb
  **empirisch** (Live-Tests oben) belegt, nicht bis zur letzten Instruktion.
* **Station-Mode ohne LAN-Control:** `hello` liefert `ff..ff`, der AP-Token wird
  abgelehnt (`miIO.info`/`set_ps`/`get_prop` Timeout). Ohne App/Cloud-Token gibt es
  dort keinen app-freien Weg — der begehbare Weg ist der Reset in den AP-Mode.
* Kein dynamischer Nachweis auf Instruktionsebene möglich (kein UART/JTAG, kein
  Firmware-Debugging am laufenden Gerät) — nur Dekompilat + Blackbox-Messung.
* `token=0000…0000` bei `did=0371f839` (LAN aktiv) ist ungeklärt; Vermutung:
  Gerät ohne vergebenen Token. Für die Provisioning-Frage irrelevant.
