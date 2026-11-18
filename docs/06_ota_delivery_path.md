# 06 — OTA-Delivery-Pfad (`color` / 88MW300): Woher kommt die Firmware-URL?

**Frage (Phase 3.0):** Ist ein **strikt OTA-only** Custom-Firmware-Flash auf dem Testträger
`.115` (`color` YLDP02YL, fw 65) möglich — d. h. bezieht der OTA-Worker seine Firmware-URL
aus einem **RPC-Parameter** (lokal setzbar) oder hart aus dem **Mi-Cloud-CDN**?

**Statik-Basis:** `firmware/color1_142_0061/` (`upd_yeelink.light.color1.bin`,
`1.4.2_0061`, SHA256 `972abef8…f983a`; Segmente `0x00100000`, `0x1f0032d8`, `0x20000040`).
Alle Adressen = virtuelle Adressen im geladenen Image (Thumb-2, LE), Disasm via
`tools/mrvl_disasm.py` (capstone).
**Live-Basis:** `color` @ `192.168.1.115`, fw 65 (18.11.2026), nur Read-Methoden.

## 0. TL;DR

| Frage | Antwort | Beleg |
| :--- | :--- | :--- |
| URL-Quelle des OTA-Workers | **RPC-Parameter** `app_url` / `wifi_url` / `mcu_url`, frei wählbar | `ota` @ `0x1f0097e8`, Getter `0x1f004292` mit Slots `0x1f0099f4`/`f8`/`fc` |
| Host-Whitelist im RPC-Pfad? | **Nein** — je URL nur `memcmp(url,"http",4)` | `0x1f009e5e` / `0x1f009e78` / `0x1f009e8c`, jeweils `bl 0x1f01a6e8` |
| Integritätsprüfung | Container-Magic + **CRC-32 je Segment** + **MD5** (16 B **vom Aufrufer**) | Header `0x1f01a9e0`, MD5 `0x1f01aad6` → `0x1f01ac50` |
| Signatur / Modell-/Downgrade-Guard | **nicht vorhanden** | Apply-Routine `0x1f01a8xx`–`0x1f01ab12` |
| Über **LAN** (TCP 55443) erreichbar? | **Nein** (Whitelist **und** falsche Modultabelle) | Dispatcher `0x1f00fe74`; live: `method not supported` |
| Über **miIO** (UDP 54321) erreichbar? | **Ja**, Methode `miIO.ota` (Token nötig) | Modultabelle `0x00103098` Eintrag `ota` → `0x1f0097e9` |
| **OTA-only-Fazit** | **Technisch ja** — aber nur über miIO/Cloud-Kanal, nicht über LAN. Offene Voraussetzung: gültiger 16-B-Token; `.115` liefert aktuell `ff…ff` | `miIO hello` → `did 05eb8034`, `token ffffffff…`, danach Request-Timeout |

## 1. Kanal-Matrix (deterministisch)

Drei getrennte Dispatch-Pfade im selben Image:

| Pfad | Transport | Tabelle | OTA-Methoden enthalten? |
| :--- | :--- | :--- | :--- |
| **LAN-RPC** | TCP 55443, Klartext-JSON | `0x00103118` (31 Einträge, Modul `user`) | **nein** |
| **miIO** | UDP 54321, AES-128-CBC + Token | `0x00103098` (16 Einträge, Modul `miIO`) | **ja** (`ota`, `get_ota_state`, `get_ota_progress`) |
| Cloud | miIO-Tabelle, cloud-gestempelt | `0x00103098` | ja (derselbe Handler, URLs vom Cloud-Push) |

### 1.1 LAN-Dispatcher `0x1f00fe74` (492 B, identischer Körper wie color2 `0x1f00fec4`)

```
0x1f00fe86  bl   0x1f01a7ac        ; Methoden-String aufbereiten, Länge -> [sp,#0xc]
0x1f00fe8a  ldr  r4,[pc,#0xb4]    ; -> 0x200009e0  Whitelist-Array (Name-Pointer)
0x1f00fe8e  mov  sb,r4
0x1f00fe90  ldr  r1,[r4],#4       ; naechster Name (NULL-terminiert)
0x1f00fe94  cbz  r1,0x1f00fea0
0x1f00fe98  bl   0x1f01a6d4        ; strcmp(methode, name)
0x1f00fe9e  bne  0x1f00fe8e        ; weiter bis Treffer/Ende
0x1f00fea0  ldr  r3,[sb]  / beq 0x1f00ff2a   ; kein Treffer -> Rueckgabe 2
0x1f00fea8  bl   0x1f0047f8        ; Format-Deskriptor
0x1f00febc  bl   0x1f004540        ; Puffer 0x20 Byte (r3=0x20)
0x1f00fef2  ldr  r2,[pc,#0x50]    ; -> 0x1f042927  "%s_JsDlgt"
0x1f00fef4  ldr  r5,[pc,#0x50]    ; -> 0x000000f8
0x1f00fef6  ldr  r7,[pc,#0x54]    ; -> 0x00103118  user-Modultabelle
0x1f00fef8  bl   0x1f019b52        ; snprintf(buf, 0x20, "%s_JsDlgt", methode)
0x1f00fefc  lsrs r5,r5,#3         ; 0xf8 >> 3 = 31 Eintraege
0x1f00ff04  ldr  r0,[r7,#4]       ; name des Eintrags
0x1f00ff08  bl   0x1f01a6d4        ; strcmp(name, buf)
0x1f00ff16  blx  r3               ; handler(ctx=sp+8, r1=0x1f0105d9, r2=r4)
```

* Whitelist `0x200009e0` (**20** Namen): `get_prop, set_default, set_power, toggle,
  set_bright, start_cf, stop_cf, set_scene, cron_add, cron_get, cron_del, set_ct_abx,
  set_rgb, set_hsv, set_adjust, adjust_bright, adjust_ct, adjust_color, set_music, set_name`.
* Der Name wird mit Suffix `_JsDlgt` gebaut und **ausschließlich** in `0x00103118`
  (Modul `user`) gesucht.

⇒ `ota_JsDlgt` / `get_ota_state_JsDlgt` / `get_ota_progress_JsDlgt` existieren nur in
`0x00103098` (Modul `miIO`) und stehen nicht in der Whitelist → **doppelt gesperrt**.

### 1.2 miIO-Modultabelle `0x00103098` (16 Einträge `{handler, name}`)

`test` `0x1f004cbd` · `config` `0x1f004cf1` · `info` `0x1f00555d` · `restore` `0x1f004de5` ·
`rpc_test` `0x1f0055fd` · `stop_diag_mode` `0x1f006591` · `wifi_assoc_state` `0x1f0065b1` ·
`get_ota_state` `0x1f00977d` · `get_ota_progress` `0x1f009d7d` · **`ota` `0x1f0097e9`** ·
`reboot` `0x1f0097bd` · `config_router` `0x1f00a085` · `xgetKeys` / `xget` / `xdel` / `xset`.

## 2. `ota`-Handler `0x1f0097e8` (nm-Symbol `ota`; `0x1f0097e9` = Thumb-Einstieg)

### 2.1 Lokal-Guard (greift nur beim miIO-Transport)

```
0x1f0097ee  ldr  r5,[r2,#8]      ; r5 = Transport-Block aus param_2
0x1f0097f8  str  r3,[sp,#0x10]   ; r3 = 0
0x1f0097fa  str  r3,[sp,#0x14]   ; 8 Nullbytes als Vergleichsmuster
0x1f0097fe  bl   0x1f016f14      ; param_2 ausrichten -> r0
0x1f009802  ldrh r2,[r5,#0x10]   ; Transport-Port
0x1f009804  movw r3,#0xd431      ; 0xd431 = 54321 = miIO-UDP
0x1f009808  cmp  r2,r3
0x1f00980a  bne  0x1f009826      ; kein miIO -> Guard ueberspringen
0x1f00980c  adds r0,#0x28        ; param_2 + 0x28 (Cloud-/Sender-UID)
0x1f009810  movs r2,#8
0x1f009812  bl   0x1f019b36      ; memcmp(param_2+0x28, 0, 8)
0x1f009816  cbz  r0,0x1f009826   ; == 0  -> OK
0x1f009820  bl   0x1f0137d0      ; sonst Antwort -1 (r2 = -1)
0x1f009824  b    0x1f0099ea      ; Abbruch, kein Download
```

⇒ Ein über **UDP 54321** eintreffender `ota`-Request wird nur akzeptiert, wenn das
8-Byte-Feld `param_2+0x28` (Sender-/Cloud-UID) **0** ist. Ein direkter lokaler
miIO-Aufruf trägt keine UID (0) → Guard passiert; ein cloud-gestempelter Request
mit UID ≠ 0 wird mit `-1` abgewiesen.

### 2.2 Parameter-Extraktion (`params` = JSON-**Objekt**)

`ctx = malloc(0x1c)` (`0x1f009828` → `0x1f02c778`), `memset` (`0x1f009838`, 0x1c).
Danach sieben Getter `0x1f004292(json, params, name)` — Slots `0x1f0099f4` … `0x1f009a0c`:

| Offset | Parameter | String-VA | Weiterverarbeitung |
| :--- | :--- | :--- | :--- |
| `ctx+0x00` | `app_url` | `0x1f041eb6` | Getter `0x1f009842`; Länge → `malloc(len+4)` → Kopie (`0x1f004020`) |
| `ctx+0x04` | `wifi_url` | `0x1f041ebe` | Getter `0x1f00987c`; dito |
| `ctx+0x08` | `mcu_url` | `0x1f041ec7` | Getter `0x1f0098b6`; dito |
| `ctx+0x0c` | `file_md5` | `0x1f041ecf` | Getter `0x1f0098ee`; `malloc(0x10)` + Hex→Bin (`0x1f004430`) = **16-B-MD5** |
| `ctx+0x10` | `proc` | `0x1f041ed8` | Getter `0x1f009918` → später `memcmp("dnld",5)` |
| `ctx+0x14` | `mode` | `0x1f042a30` | Getter `0x1f009948` → später `memcmp("silent",7)` |
| `ctx+0x18` | `sequence` | `0x1f041edd` | Getter `0x1f009978` → später Segment-Selektor (`a`/`w`/`m`) |

Sind **alle drei** URLs `NULL` → Log `"no url"` (`0x1f041eaf`, Slot `0x1f009a24`);
`malloc`-Fehler → `"no mem"` (`0x1f0415e3`, Slot `0x1f009a20`).
Erfolgspfad: Zustands-Globals `0x20000cb4` / `0x20000cb8` / `0x20000cb0` auf 0
(`0x1f0099ac`–`0x1f0099ba`), Antwort an den Aufrufer (`0x1f0137d0`), dann Task-Erzeugung
mit Worker-Einsprung `0x1f009e3d` (Slot `0x1f009a1c`) — der Download läuft asynchron.

## 3. Download-Worker `0x1f009e3c` (`r5 = ctx`)

```
0x1f009e40  mov  r5,r0                  ; ctx
0x1f009e42  bl   0x1f009bac             ; Vorbereitung; != 0 -> Fehlerpfad
0x1f009e52  ..   Globals 0x20000cb4/cb8/cb0 := 0   ; sb/sl/fp = Statuszeiger
0x1f009e5e  ldr  r0,[r5]      ; app_url  -> cbz; memcmp(url,"http",4) @0x1f009e6c
0x1f009e74  str  r3,[fp]      ; Fehlercode 0x20 bei Nicht-"http"
0x1f009e78  ldr  r0,[r5,#4]   ; wifi_url -> memcmp @0x1f009e80; Fehlercode 0x10
0x1f009e8c  ldr  r0,[r5,#8]   ; mcu_url  -> memcmp @0x1f009e94; Fehlercode 0x10
0x1f009eb2  ldr  r1,[r5,#0x18] ; sequence: 1. Zeichen 'a'(0x61)/'w'(0x77)/'m'(0x6d)
0x1f009ec6  -> sonst Default 0x1f041f1d "wam"
0x1f009ece  bl   0x1f01a778    ; Selektor-String in lokalen Puffer (8 B)
0x1f009ed8  bl   0x1f019b36    ; memcmp(ctx+0x10, "dnld"(0x1f041f21), 5)
0x1f009ee6  bl   0x1f019b36    ; memcmp(ctx+0x14, "silent"(0x1f041f26), 7)
```

* **Einzige URL-Validierung** ist der `"http"`-Präfixvergleich — für **alle drei** URLs.
  `http://…`, `https://…`, beliebiger Host, beliebiger Port. Ein Host-/Domain-Whitelist-
  Vergleich existiert im RPC-Pfad **nicht**: die Strings `"https://"` `0x1f041f2d`,
  `".xiaomi.com"` `0x1f041f36`, `".mi-img.com"` `0x1f041f42` liegen im Cloud-OTA-Block
  (Nachbarn `"idle"` / `"downloading"` = State-Strings) und haben **keine**
  Literal-Pool-XREFs in den Codesegmenten.
* Segment-Auswahl: `sequence` = Zeichenkette aus `a` (App) / `w` (WiFi) / `m` (MCU),
  Default `"wam"`; `proc == "dnld"` und `mode == "silent"` schalten Verhalten (Log/Reboot) um.
* HTTP-Client `0x1f009100`: baut den Request aus einem 18-Byte-Template
  (Image-String `"GET %s HTTP/1.1"`, Quellpfad `…/miio_app/miio_common/Source/ot/d0_watcher_httpc.c`),
  **reiner Klartext-Socket** (kein TLS), Antwort-Header wird gegen 8 Byte geprüft.
* `get_ota_state` (`0x1f00977d`) liest die State-Globals (`"idle"` / `"downloading"` / …),
  `get_ota_progress` (`0x1f009d7d`) die Progress-Global `0x20000cb0`.

## 4. Verifikation & Schreiben (`0x1f01a8xx`–`0x1f01ab12`)

| Schritt | Adresse | Befund |
| :--- | :--- | :--- |
| 20-Byte-Container-Header lesen | `0x1f01a9e0` | `cmp r5,#0x14 / bne` → harte Längenprüfung |
| Magic-Vergleich | `0x1f01a9e4`–`0x1f01aa10` | 12 B aus Header vs. Konstante, **zwei** akzeptierte Magics (Alt-/Neu-Variante) |
| Stream-Init | `0x1f01aa20` | `0x1f01ae30` / `0x1f01afc4` |
| Chunked Read + Progress | `0x1f01aa3e`–`0x1f01aa94` | 0x100-B-Blöcke, `progress = 100*consumed/size` |
| Schreiben / Flash-Status | `0x1f01af6c` | Fehlerpfad `r7 != 0` |
| **MD5-Prüfung** | `0x1f01aad6` | bei Magic-Abweichung: `0x1f01ac50(ctx+0x0c, size)` → MD5 über den **geschriebenen** Stream gegen den **vom Aufrufer gelieferten** 16-B-Digest |
| Fehlertext | `0x1f01aae4` | `"Firmware verification failed with code %d"` |
| Erfolg | `0x1f01aaf8` | Log + `return 0` |

Es gibt **keinen** Modell-, Hardware- oder Downgrade-Vergleich und **keine** Signatur —
der MD5-Wert kommt aus dem RPC-Payload und ist damit frei wählbar. Ein selbst gebauter
`MRVL`-Container (App-Slot `0x10000`, ≤ 384 KB, CRC-32 je Segment) passiert diese Prüfung,
wenn `file_md5` = MD5 des ausgelieferten Images gesetzt wird.

## 5. Live-Nachweise (`color` @ `192.168.1.115`, fw 65, 18.11.2026)

| Probe | Ergebnis |
| :--- | :--- |
| `get_prop [fw_ver,model,power,lan_ctrl,name]` (TCP 55443) | `["65","color","on","1","Schlafzimmer Schreibtischlampe"]` |
| `get_ota_state` (TCP 55443) | `{"code":-1,"message":"method not supported"}` ← Whitelist-Gate |
| `get_ota_progress` (TCP 55443) | `{"id":0,"code":-1,"message":"invalid command"}` (`id`=0 = fw-65-Catch-all) |
| `get_prop [cfg_lan_ctrl,music_on]` | `["","0"]` (`cfg_lan_ctrl` in fw 65 nicht exponiert) |
| `miIO hello` (UDP 54321) | `did 05eb8034`, `stamp 7931`, **`token ff…ff`** |
| `miIO.info` / `miIO.get_ota_state` (UDP, `ff…ff`-Token) | **Timeout** → ohne gültigen Token kein miIO-Request |

**Kein Flash, kein Öffnen, keine Schreiboperation** — nur `get_prop` / `hello` / Read-Methoden.

## 6. Antwort auf die Phase-3.0-Frage

**Ist „strikt OTA-only" realisierbar? — Ja, technisch, aber nicht über LAN.**

1. Der OTA-Worker ist **kein** Cloud-only-Konstrukt: die Firmware-Quelle kommt vollständig
   aus dem RPC-Payload (`app_url` + `file_md5`), geprüft werden nur `"http"`-Präfix,
   Container-Magic, CRC-32 je Segment und der selbst gelieferte MD5.
2. Der dafür nötige Handler `ota` (`0x1f0097e9`) hängt **ausschließlich** im
   `miIO`-Modul (`0x00103098`) — über TCP 55443 ist er doppelt gesperrt
   (Whitelist + falsche Modultabelle). Live bestätigt.
3. Nutzbarer Kanal ist damit **miIO UDP 54321** (AES-128-CBC, 16-B-Token) — derselbe Kanal,
   über den auch `set_ps ["cfg_lan_ctrl","1"]` läuft. Er verlangt einen **gültigen Token**;
   `.115` antwortet aktuell mit `ff…ff` (kein Token) → Requests laufen in Timeouts.
4. Offene Voraussetzung ist damit allein die **Token-Beschaffung** (AP-Mode-Handshake
   liefert den Token im Klartext, siehe `analysis/color2_lan_ctrl_channel.md` §3/§4).
   Das ist ein Provisioning-Schritt, kein Firmware-Problem.

### Kanal + Protokoll (aus dem Code abgeleitet, **nicht** live ausgeführt)

```jsonc
// miIO UDP 54321, AES-128-CBC/Token, params = OBJEKT (nicht Array)
{"id":1,"method":"miIO.ota","params":{
   "app_url":  "http://192.168.1.154:8000/upd_yeelink.light.color1.custom.bin",
   "file_md5": "<32 hex = MD5 des Images>",
   "proc":     "dnld",
   "mode":     "silent",
   "sequence": "a"
}}
```

Ablauf: Gerät holt das Image per `GET` vom lokalen HTTP-Server → prüft Magic/CRC/MD5 →
schreibt in den App-Slot → Progress über `get_ota_progress` (nur miIO) abfragbar.
`wifi_url` / `mcu_url` bleiben leer (nur App-Segment, `sequence:"a"`).

### Minimal nötiger Alternativweg (Info für PO)

* **A (im Rahmen von OTA-only):** `.115` in den Setup-AP bringen (Factory-Reset ist der
  einzige Weg zum Klartext-Token) → Token per `miIO hello` holen → `set_ps
  ["cfg_lan_ctrl","1"]` → danach `miIO.ota` mit lokaler URL. Kein Öffnen des Geräts.
* **B (falls Reset unerwünscht):** Token aus einem Cloud-/App-Kontext extrahieren
  (App-RE-Route) — aufwändiger, gleiche Hard-Grenzen (kein Öffnen).

## 7. Caveats

* Statik-Basis ist `color` **1.4.2_0061**; der Testträger läuft **fw 65** (= 1.4.2_0065,
  vier Builds neuer). `color1_0061` und `color2_0027` teilen die Tabellen-/Handler-Adressen
  exakt (`0x00103098` / `0x00103118`, `ota` `0x1f0097e9`) — innerhalb der SDK-Generation
  ist das Layout stabil, ein 0065-Drift also unwahrscheinlich, aber **nicht** statisch
  bewiesen (fw-65-/fw-76-Binary nicht beschaffbar).
* „Keine Signatur" ist über die Apply-Routine belegt (kein Public-Key-/ECDSA-Aufruf, kein
  Zertifikats-Speicher); der `MRVL`-Container ist ohnehin unsigniert
  (siehe `[[notes/concepts/yeelight-88mw302-sdk-recherche]]`).
* Der `miIO.ota`-Payload ist aus den Parameternamen/JSON-Getter-Aufrufen rekonstruiert;
  ein echter End-to-End-Test ist ohne Token **und** ohne Flash-Freigabe bewusst unterblieben.
