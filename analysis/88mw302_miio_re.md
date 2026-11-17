# statische RE-Notizen: Marvell 88MW302 miIO-Firmware (`upd_lumi.gateway.v3`)

Stand: Ghidra 12.1.4 headless auf `devastator` (192.168.1.154), Projekt
`ghidra_workspace/ylre` (via `.gitignore` ausgeschlossen), Image
`firmware/ref_88mw302/upd_lumi.gateway.v3.elf`.

Alle Adressen sind **virtuelle Adressen im geladenen Image** (Abschnitt `.text2`
@ `0x1f003718`, File-Offset `0x13718`, Thumb-2 Little Endian, ARM Cortex-M4).
Reproduzierbar über `ghidra_workspace/export3/decompiled/ALL.c` (2771 Funktionen)
plus den Nachanalyse-Exporten `export4` / `export5` für die Lückenbereiche.

## 1. RPC-Dispatch (miIO-Methodentabelle)

| Objekt | Adresse | Beleg |
| :--- | :--- | :--- |
| Methodentabelle (`meth_desc[8]`) | `0x103198` … `0x103650` | 151 Einträge à 8 Byte (`{handler, name}`), `ghidra_workspace/rpc_table.txt` |
| Modul-Deskriptor "miIO" | `0x1f0067f8` | Literal-Pool von `FUN_1f0065f8`: `{0x20001aa0, 0x1f06d6cb "miIO", 0x103198, 0x103220, 0x88, 0, 0x20001adc, …, 0x103220, 0x103650, 0x430}` |
| Register-Aufruf | `FUN_1f0065f8` (318 B) | `psm_open("ot_config")` + Modulregistrierung |

Der Deskriptor teilt die Tabelle in **17 miIO-Kernslots** (`0x103198`–`0x103220`)
und **134 Geräte-Slots** (`0x103220`–`0x103650`). Die ersten 17 Slots (Ghidra-Labels
aus den Tabellen-XREFs):

```
0x103198 test_JsDlgt          0x1f0057f0
0x1031a0 config_JsDlgt        0x1f005810
0x1031a8 info_JsDlgt          0x1f005f94
0x1031b0 restore_JsDlgt       0x1f0058cc
0x1031b8 rpc_test_JsDlgt      0x1f006028
0x1031c0 stop_diag_mode       0x1f0070ee
0x1031c8 wifi_assoc_state     0x1f00710c
0x1031d0 get_ota_state        0x1f008068
0x1031d8 get_ota_progress     0x1f0086c4
0x1031e0 ota_JsDlgt           0x1f0080dc
0x1031e8 reboot_JsDlgt        0x1f0080b0
0x1031f0 config_router        0x1f0089f0
0x1031f8 xdel_JsDlgt          0x1f0146e4
0x103200 xset_JsDlgt          0x1f01aef8
0x103208 xdel_JsDlgtS         0x1f014620
0x103210 xset_JsDlgtS         0x1f014340
0x103218 send_event_JsDlgt    0x1f0140ac
```

Anmerkung: Die ersten Funktionen waren bei der Erstanalyse **nicht** als Funktion
erkannt (nur Pointer-Tabelle, kein Code-XREF) — sie wurden per Nachscript
(`DisasDecompile.java`, Adressliste) erzwungen und anschließend neu analysiert
(`export3`, 2620 → 2771 Funktionen).

## 2. `miIO.config_router` — Ablauf

`config_router_JsDlgt @ 0x1f0089f0`:

1. Parst `params` aus dem RPC-Request (`FUN_1f03fc70`), liest `ssid` / `passwd`
   (Puffer bei `0x20001c63` bzw. Stackkopie), prüft Längen (SSID 1..32, PSK 8..63/64).
2. Persistiert die UID des Aufrufers:
   `FUN_1f00617c` → `FUN_1f046a82(psm, "ot_config.uid", …)` (**PSM-Schreibpfad**),
   zusätzlich RAM-Cache via `FUN_1f0062dc` → `DAT_20000210/DAT_20000214`.
3. Setzt Änderungsflags: `FUN_1f0064b4` schreibt `DAT_2000037d` und toggelt in der
   WLAN-Konfiguration (`FUN_1f03fbc4(&DAT_1f06d45a /* "otn" */)` → `[*+0x30]+0xde`).
4. Antwortet **sofort** per `FUN_1f03c2fc(param_2, param_3, 0)`.
5. Plant eine verzögerte Folgeaktion:
   `FUN_1f006e6c(&LAB_1f008cb8_1, 0, 2000, 0)` → **2000 ms** später
   `LAB_1f008cb8` (10 Byte, Deferred-Callback).

### `event.router_changed` — Emitter `FUN_1f008cf4 @ 0x1f008cf4`

Baut `{"method":"event.router_changed","params":{…}}` und wertet das Flagwort
`_DAT_480c02c8` aus (von `config_router` gesetzt):

| Flagwert | Parameternutzlast |
| :--- | :--- |
| `0xf0f0f0f0` | `"ssid or passwd changed"}` |
| `low16 == 0xbe49` | `"ssid changed "` |
| `high16 == 0x5a87` | `"passwd changed"` |

Versand: `FUN_1f03c254(0, 0, json, len, &LAB_1f0089cc_1 /* ack-cb */, 0)` mit
**Retry-Schleife: max. 4 Versuche, je `FUN_1f0582f4(3000)` = 3000 ms Pause**,
Abbruch vorzeitig, sobald das Ack-Flag `DAT_20001bcc` gesetzt wird.
Danach wird das Flagwort genullt (`_DAT_480c02c8 = 0`).

Das ist der einzige im Image sichtbare **Ack-Timeout-Mechanismus** (3 s × 4).

## 3. Token-Erzeugung / -Rotation

### 3.1 Kernfunktion `FUN_1f008e10 @ 0x1f008e10` (272 B)

```
psm_open(&ctx, "ot_config")                       // FUN_1f046c0c
rc = psm_get_variable(&ctx, "ot_token", buf, 0x23) // FUN_1f046c90
if (rc == 0 && param_1 == 0) {                     // Token vorhanden, kein Force
    FUN_1f0049f6(buf, 0x23, &token16 @0x20001ca8, 0x10, 0)  // 32 hex -> 16 Byte
    return 0;
}
memset(&token16, 0, 0x10);                         // sonst: neu erzeugen
rnd = (exception==0) ? FUN_1f0580b4() : FUN_1f0580ec();   // beide: *(u32*)0x2000b7f4
len  = snprintf(work, 0x34, "%u", rnd);             // Format-String @0x1f07e8ec = "%u"
memcpy(work+len,       "TAG_KEY", 0x10);
memcpy(work+len+0x10,  &token16, 0x10);            // 16 Nullbytes
memcpy(work+len+0x20,  "TAG_MAC", 6);
FUN_1f049f9c(work, len+0x26, &token16, 0x10);      // MD5 -> 16 Byte Token
snprintf(buf, 0x23, "%.2x"×16, token16[i]);        // ASCII-Hex
psm_set_variable(&ctx, "ot_token", buf);           // FUN_1f046c50 -> persistent
psm_close(&ctx);
```

* Hash: `FUN_1f049f9c` = Wrapper um `FUN_1f06a4a8` (init) / `FUN_1f06a4d4`
  (update) / `FUN_1f06a52e` (final) → **MD5**, 16-Byte-Ausgabe.
* Entropiequelle: nur das 32-Bit-Wort `0x2000b7f4` (Tick-/Zählerstand) —
  **max. 32 Bit Token-Entropie im sichtbaren Pfad**, plus die statischen
  Salt-Strings `TAG_KEY` / `TAG_MAC`.
* Persistenz: `ot_config.ot_token` (ASCII-Hex, 32 Zeichen), RAM-Kopie
  `0x20001ca8` (16 Byte).
* Aufruf mit `param_1 != 0` erzwingt Rotation (Force-Pfad ist im Image vorhanden,
  aber **kein** direkter Aufrufer mit `1` nachweisbar → siehe 3.3).

### 3.2 Boot-Pfad `FUN_1f00900c @ 0x1f00900c` (598 B)

UART-Ausgaben (verifizierte Strings):

```
"[MIIO-NORMAL-BOOT]\r\n"  bzw.  "[MIIO-TEST-STARTUP-PASS]\r\n"
"mac address is %s\r\n"   (Buffer via "%.2x%.2x%.2x%.2x%.2x%.2x", 0x54,0x41,0x47,0x5f,0x4d,0x41)
"digital_did is %d \r\n"  (FUN_1f006508)
"last four byte of key is %.2x%.2x%.2x%.2x \r\n"  (mit 0,0,0,0 → Key wird NICHT geloggt)
FUN_1f008e10(0);          // Token laden/erzeugen
```

Danach PSM-Namespace `TAG_DOMAIN` (Default `"ot.io.mi.com"`, XREF-String @`0x1f0737c2`).

### 3.3 Rotation bei AP→STA / Re-Provisioning

Im Image existiert **kein** Aufruf von `FUN_1f008e10(1)`. Die Rotation entsteht
indirekt, weil der Token beim Boot nur dann neu berechnet wird, wenn der
PSM-Eintrag fehlt. Der einzige Löschpfad, der `ot_config.*` mitnimmt, ist der
Property-Handler `set_prop`:

```
set_device_prop_JsDlgt @ 0x1f01342c:
  if (prop == "psm_erase")  FUN_1f046efc(0, 0);   // komplettes PSM löschen (Zeile 17782 in ALL.c)
  if (prop == "token_did")  FUN_1f010cd4(did);    // Cloud-Token-Sync
  if (prop == "debug_mode") FUN_1f01c920(…);
  if (prop == "reboot")     FUN_1f036054(…);
```

`FUN_1f010cd4` sendet
`{"id":%d,"method":"_sync.neighborDevInfo","params":{"did":["%u"]}}`.

→ **Belegte Kette:** Re-Provisioning/Reset ⇒ `psm_erase` (oder `miIO.restore`
@ `0x1f0058cc`) ⇒ PSM wipe ⇒ Reboot ⇒ `FUN_1f008e10(0)` findet
`ot_config.ot_token` nicht ⇒ MD5-Neuerzeugung ⇒ neuer Token, alte App-/Cloud-
Sessions werden ungültig.

### 3.4 Token-Handling für Nachbargeräte (Leak-Vektor)

`FUN_1f035b40 @ 0x1f035b40` (Settler in verkettete Nachbar-Dev-Liste `0x20008b1c`):

```
UART: "\r\n##Tkn set=%llu,%s,%s\r\n"        // DID + Token im Klartext auf Debug-UART!
FUN_1f0349a0(param_4, 0x20, &entry[3])       // 32 Byte -> 16 Byte (Digest/Expand)
entry[0..1] = did_lo/hi ; entry[+0x12] |= 1/2 (Flags) ; entry[+0x13..] = 16-Byte-Token
```

Zusätzlich: die **UART-CLI** exponiert das PSM direkt (alle in `.text`):

| CLI-Kommando | Funktion | Effekt |
| :--- | :--- | :--- |
| `psm-get <var>` | `FUN_1f046fa4` @ `0x1f046fa4` | liest PSM-Variable, druckt Wert; markiert `(enc)` bei verschlüsselten Variablen |
| `psm-set <var> <val>` | `FUN_1f047180` @ `0x1f047180` | schreibt PSM-Variable |
| `psm-delete <var>` | `FUN_1f04711c` @ `0x1f04711c` | löscht Variable |
| `psm-erase` | `FUN_1f046efc` @ `0x1f046efc` | löscht gesamtes PSM |
| `psm-dump` | `FUN_1f047258` @ `0x1f047258` | listet alle Variablen |

⇒ `psm-get ot_config.ot_token` auf dem Debug-UART liefert den miIO-Token
(hexadezimal, 32 Zeichen) — ohne jede Authentifizierung.

## 4. `miIO.ota` — Prüf- und Auslieferungskette

`ota_JsDlgt @ 0x1f0080dc` (484 B):

1. **Magic-Check** auf dem Request-Header: `*(int16*)(msg+0x10) == 0xd431`
   (als `-0x2bcf` im Dekompilat). `0xd431` taucht auch im Boot als
   `FUN_1f057744(0xd431)` auf → gemeinsame Protokoll-/Image-Konstante.
   Zusätzlich `FUN_1f0423a8(json+0x28, &buf, 8)`; bei Fehler **und**
   `DAT_20001bb0 == 0` → Fehlerantwort `"invalied"` (`0xffffffff`).
   `DAT_20001bb0` ist damit ein Bypass-/Debug-Flag für den Magic-Check.
2. Parameter aus dem Request (`FUN_1f004e6c`, Strings im Image vorhanden):
   `file_md5` (0x21 B, danach `FUN_1f043d90` = Normalisierung des Hex-Strings),
   `app_url`, `wifi_url`, `mcu_url` (je 0x100 B), `sequence` (4 B),
   Flags `no_reboot`, `silent` (je 1 B, via `FUN_1f004e8e`).
3. Progress-Zähler werden genullt: `DAT_20001124`, `DAT_20001128`, `DAT_2000112c`.
4. **Sofort-Ack**: `FUN_1f03c2fc(param_2, param_3, 0)` — der Download läuft
   asynchron weiter (typisches miIO-Verhalten: App bekommt `ok`, bevor Bytes fließen).
5. Queueing: `FUN_1f006c80(&LAB_1f008784_1, 0, 5, app_url, wifi_url, mcu_url,
   sequence, no_reboot | silent<<1)` → Worker bei `0x1f007884`.
6. Fehlerpfad: `FUN_1f03c1f4(param_2, param_3, code, msg)` mit
   `"invalied"` (-1) bzw. `"no mem"` (-5, `0xfffffffb`).

**Verifikationskette (byte-exakt, siehe 4.2):** Der Client liefert `file_md5` (32 Hex-Zeichen);
das Image prüft die **heruntergeladene Datei** mit einer eigenen MD5-Implementierung
(`FUN_1f043b7c`/`1f043ba8`/`1f043c20`/`1f043460`, IVs `0x67452301`/`0xefcdab89`/`0x98badcfe`/
`0x10325476`) gegen genau diesen Wert und danach die CRC32 je Container-Segment. Die
Datei-Magic ist **kein XZ**: der OTA-Container trägt `"SBHF"` (`0x48464253`) bzw.
`"MRVL"`+`0x2e9cf17b`. Die XZ-Magic (`\xfd7zXZ`) kommt im Image zwar 3× vor, aber
**nicht** im OTA-Download-/Prüfpfad (sie gehört zur Dateisystem-Dekoression, nicht zu `miIO.ota`).

Statusabfragen:

| Handler | Adresse | Antwort |
| :--- | :--- | :--- |
| `get_ota_state` | `0x1f008068` | `{"result":[%s]}` aus Statustabelle `0x2000035c` (`"idle"`, …), UART `"ota state:%s\r\n"` |
| `get_ota_progress` | `0x1f0086c4` | `{"result":%d}` aus `FUN_1f00850c()`, UART `"ota progress:%u%\r\n"` |
| `report_ota_state` | `0x1f0084a0` | UART `"\r\nreport_ota_state: %s \r\n"`, setzt Property `ota_state`, `commit(FUN_1f007b20(…,5))` |

### 4.1 OTA-Worker `0x1f008784` — Register-Propagation geschlossen

Der als „Analyse-Loch“ (`0x1f00787c`–`0x1f00799b`) markierte Bereich war ein
**Funktionsgrenzen-Artefakt** des Ghidra-Projekts, kein Decompiler-Registerproblem:
bei `0x1f008784` war *keine* Funktion definiert (`getFunctionAt` / `getFunctionContaining`
→ `null`), deshalb landeten die Fragmente in `export5/000_ota_worker_task.c` als
Tail-Calls mit `unaff_*`-Registern.

Mit `DecompAt.java` (`createFunction(0x1f008784,"ota_worker_task_1f008784")` +
`DecompInterface`) dekompiliert der Bereich jetzt sauber: **518 Byte**
(`0x1f008784`–`0x1f00898a`), **ohne ein einziges `unaff_*`**.
Artefakt im Repo: `analysis/ota_worker_task_1f008784.c`.

Parameter (aus `FUN_1f006c80(&LAB_1f008784_1, 0, 5, …)` in `ota_JsDlgt @ 0x1f0080dc`):

| # | Typ | Bedeutung | Setzer |
| :--- | :--- | :--- | :--- |
| 1 | `char *` | `app_url` | `FUN_1f004e6c(…,"app_url", buf, 0x100, 0)` |
| 2 | `char *` | `wifi_url` | `FUN_1f004e6c(…,"wifi_url", …)` |
| 3 | `char *` | `mcu_url` | `FUN_1f004e6c(…,"mcu_url", …)` |
| 4 | `char *` | `sequence` (≤ 8 Zeichen, Zielauswahl `a`/`w`/`m`) | `FUN_1f004e6c(…,"sequence", buf, 4, 0)` |
| 5 | `byte` | `flags` = `no_reboot \| silent<<1` | `FUN_1f004e8e` |

Rekonstruierter Ablauf (Adressen = Instruktionsadressen, Thumb-2):

| Adresse | Beleg |
| :--- | :--- |
| `0x1f008784` | `push {r4-r11,lr}` / `sub sp,#0x14` — Callback-Einstieg (Queue-Eintrag `0x1f008785`, Thumb-Bit) |
| `0x1f00878a`–`0x1f008796` | Argumente sichern: `r7=app_url`, `r6=wifi_url`, `r5=mcu_url`, `[sp]=sequence`, `[sp+4]=flags` |
| `0x1f008798` | `bl 0x1f0084f8` — Guard/Lock; Rückgabe ≠ 0 ⇒ Abbruch ohne Download (Cleanup-Pfad) |
| `0x1f0087a6` | `bl 0x1f00b400` — OTA-/HTTP-Kontext initialisieren |
| `0x1f0087aa`–`0x1f0087ba` | Gewichte `0x20001bb4` (app), `0x20001bc8` (wifi), `0x20001bbc` (mcu) auf 0 |
| `0x1f0087bc`–`0x1f0087f6` | je URL `FUN_1f042f5c(url,"http",4)==0` ⇒ Gewicht setzen: app `0x20`, wifi `0x10`, mcu `0x140` |
| `0x1f0087fa`–`0x1f008808` | Summe der drei Gewichte == 0 ⇒ keine gültige URL ⇒ Fehlerpfad |
| `0x1f00880c`–`0x1f00882a` | Zielliste: erstes Zeichen von `sequence` muss `a`/`w`/`m` sein, sonst Default `"wam"` (`0x1f06dbd0`); `FUN_1f042fd0(buf, seq, 8)` |
| `0x1f00882e`–`0x1f008834` | `flags`-Bit 1 (`silent`) == 0 ⇒ `bl 0x1f008710` (State-Report initialisieren) |
| `0x1f008838`–`0x1f00884e` | `FUN_1f043f7c()` (App-Passiv-Partition) und `FUN_1f04400c()` (MCU-Partition); schlägt einer fehl ⇒ `bFallback = true` |
| `0x1f008852`–`0x1f0088ca` | Schleife `i=0..2` über die Zielliste: `'a'` ⇒ `FUN_1f008304(app_url)` (Fallback `FUN_1f008344`), `'w'` ⇒ `FUN_1f00835a(wifi_url)` (`FUN_1f0083fe`), `'m'` ⇒ `FUN_1f008414(mcu_url)` (`FUN_1f008474`); vorher OTA-State 1 (`FUN_1f0085b4(1)`), nach Erfolg Fortschritt = 100 (`0x20001124`/`0x2000112c`/`0x20001128`), Fehler ⇒ `iVar8 = 2` |
| `0x1f0088cc`–`0x1f0088d8` | alle Ziele ok ⇒ `iVar8 = 1` |
| `0x1f008900`–`0x1f008920` | Cleanup: alle vier Argument-Strings via `FUN_1f058de8` freigeben |
| `0x1f008922`–`0x1f008936` | `iVar8 == 2` ⇒ OTA-State 5 (`"failed"`), `FUN_1f008758()` (Report), Rückgabe `0xffffffff` |
| `0x1f008938`–`0x1f008968` | `iVar8 == 1` + Fallback: `no_reboot`(Bit 0) == 0 ⇒ State 3 (`"installing"`), `FUN_1f0582f4(6000)` (6 s), Report, `FUN_1f041d38()` (Reboot); sonst State 4 (`"installed"`) |
| `0x1f00896a`–`0x1f00897e` | `iVar8 == 1` + Normalpfad: State 1, dann `FUN_1f008638(Zielliste, no_reboot)` (Install-Stage), `FUN_1f008758()`, Rückgabe = Rückgabe der Install-Stage |

OTA-State-Tabelle `0x2000035c` (Strings ab `0x1f06dbd1`):
`0 "idle"`, `1 "downloading"`, `2 "downloaded"`, `3 "installing"`, `4 "installed"`,
`5 "failed"`, `6 "busy"`.

### 4.2 MD5-Verifikation der Downloaddatei — byte-exakt belegt

Kette (jede Stufe mit Adresse belegt):

```
ota_worker_task_1f008784 (0x1f008784)
  └─ FUN_1f008304(app_url)                 0x1f008304   [analog FUN_1f00835a / FUN_1f008414]
       ├─ FUN_1f043fc4()                   -> App-Passiv-Partitionshandle (FUN_1f0419b4(1,..))
       └─ FUN_1f044058(url, partptr, 0)    0x1f044058
            ├─ FUN_1f045890(url, &ctx, &resp, 0)
            │     Rückgabe ≠ 0 -> "[%s]%sUnable to connect to server"
            │     resp->status (resp+8) != 200 -> "[%s]%sHTTP Error %d"  -> -1
            │     resp->chunked (resp+0x28) != 0 -> "HTTP chunked fs update is not supported"
            │     Typ = *partptr, erlaubt {1,2,3,5,6}, sonst "Unknown type: %d" -> -1
            └─ FUN_1f043ef8(…, http_ctx, resp->content_length (resp+0x2c), &typ)
                 Typ 1 -> FUN_1f043450 -> FUN_1f04326c    (App/Firmware)  "Firmware update failed"
                 Typ 2 -> FUN_1f044640                    (WiFi-FW)       "WiFi Firmware update failed"
                 Typ 3 -> FUN_1f044890                    (FS)            "FS update failed"
                 Typ 5/6 -> FUN_1f044298                  (User-FW/misc)  "User Firmware update failed"
```

`FUN_1f043ef8` setzt dabei `DAT_2000988c = 1` (Busy-Flag, verhindert Deep-Sleep) und
löscht es bei Erfolg wieder.

**Container-Vorprüfung** in `FUN_1f04326c(read_cb, ctx, content_length, part, post_cb)`:

```
0x1f04327c  if (content_length == 0)              -> "Firmware binary zero length"        -> -24583 (0xffff9ff9)
0x1f043288  if (content_length > *(part+0x10))    -> "Firmware binary too large %d > %d"  -> -24583
0x1f0432a4  Header (0x14 B) in Schleife vom Stream lesen (exakt 0x14 Bytes)
            -> Fehler: "Failed to get firmware data for initial validation" -> -1
0x1f0432d2  Magic-Prüfung:
              word0 == 0x48464253  ("SBHF")                       -> OK, Klartext-Image
              ODER word0 == 0x4c56524d ("MRVL") && word1 == 0x2e9cf17b -> OK, Container
              sonst -> "File data is not a firmware file"          -> -24589 (0xffff9ff3)
0x1f043304  Flash-Bereich vorbereiten (FUN_1f04403c) + löschen (FUN_1f0441cc)
            -> Fehler: "Failed to erase firmware area "
0x1f043350  Schreibschleife: erste Iteration = die 0x14 Header-Bytes, dann ≤0x100-Byte-Blöcke
              FUN_1f044174(ctx, buf, n)  -> Fehler: "Failed to write firmware data"
              read-Callback < 1          -> "http recv error/timeout, %d"
0x1f043366  Fortschritt: DAT_20001124 = written*100/content_length
0x1f04339c  "Validating firmware start ... filesize = %d"
0x1f0433c0  Verifikation (nur wenn Header NICHT "SBHF"):
              iVar1 = FUN_1f043e40(*(part+0xc) /* Flash-Adresse */, written)
0x1f0433d4  Fehler: "[%s]%sFirmware verification failed with code %d"
0x1f0433dc  Erfolg: "Validating firmware done"  -> 0
```

**Die MD5-Prüfung selbst** — `FUN_1f043e40(flash_addr, len)`:

```c
iVar2 = FUN_1f043004(0x20009866);                       // strlen(file_md5)
if ((iVar2 == 0) ||                                     // kein md5 übergeben -> MD5 wird übersprungen
    (iVar2 = FUN_1f043cdc(flash_addr,len,0x20009866), iVar2 == 0)) {
        ... Container-Segment-CRC32-Prüfung ...         // siehe unten
}
return 0xffff9ff4;                                      // -24588 = Verifikation fehlgeschlagen
```

`FUN_1f043cdc(addr, len, expected_md5)`:

```c
FUN_1f06ca94(hex, 0, 0x21);                  // 33-Byte-Puffer für Hex-String
handle = FUN_1f041e10(0);                    // Flash-Treiber
buf    = FUN_1f058cf0(0x200);                // 512-Byte-Lesepuffer
if (handle == 0 || buf == 0) return 0;       // ACHTUNG: ohne Flash-Handle/Malloc gilt "match"
FUN_1f043b7c(ctx);                           // MD5-Init: 0x67452301,0xefcdab89,0x98badcfe,0x10325476
while (len > 0) {                            // 512-Byte-Blöcke, READ-BACK aus dem Flash
    n = min(len,0x200);
    FUN_1f041dd4(handle, buf, n, addr);      //   Flash-Read
    FUN_1f043ba8(ctx, buf, n);               //   MD5-Update (64-Bit-Bitzähler)
    addr += n; len -= n;
}
FUN_1f043c20(digest, ctx);                   // MD5-Final (0x80-Padding, 16-Byte-Digest)
free(buf);
for (i = 0; i < 0x20; i += 2)                // 32 Hex-Zeichen, KLEINBUCHSTABEN
    FUN_1f0423c4(hex+i, 3, "%02x", digest[i>>1]);
return FUN_1f0423a8(hex, expected_md5, 0x20) != 0;   // 0 = identisch, 1 = MISMATCH
```

Damit ist der Ablauf eindeutig:

* **Eingabe des Hashes:** die **Datei-Bytes, wie sie geschrieben wurden** — `FUN_1f04326c`
  gibt `*(part+0xc)` (Flash-Basisadresse des Zielbereichs) und `written`
  (= empfangene Content-Length, **inklusive** der 0x14 Header-Bytes) weiter. Der Hash
  läuft über **Read-back aus dem Flash**, nicht über den RAM-Puffer (512-B-Blöcke).
* **Sollwert:** global `0x20009866`, gefüllt von `ota_JsDlgt`:
  `FUN_1f004e6c(…,"file_md5", buf, 0x21, 0)` → `FUN_1f043d90(buf)` →
  `memcpy(0x20009866, buf, 0x21)`. Das ist eine **blinde 33-Byte-Kopie ohne
  Normalisierung** ⇒ nur exakt passende Lowercase-Hex-Werte (32 Zeichen) bestehen die
  Prüfung; `file_md5` in Großbuchstaben oder mit `0x`-Präfix scheitert.
* **Vergleich:** `FUN_1f0423a8(hex, 0x20009866, 0x20)` = `memcmp`/`strncmp` über 32 Zeichen.
* **Mismatch-Pfad:** `FUN_1f043cdc` → 1 ⇒ `FUN_1f043e40` → `0xffff9ff4` (-24588) ⇒
  `FUN_1f04326c` loggt `"[%s]%sFirmware verification failed with code %d\r\n"` (-24588),
  schließt den Treiber-Handle (`FUN_1f041e00`) und gibt `0xffff9ff4` zurück ⇒
  `FUN_1f043ef8` loggt `"[%s]%sFirmware update failed\r\n"` ⇒ `FUN_1f044058` → `-1` ⇒
  `FUN_1f008304` → `-1` ⇒ Worker `iVar8 = 2` ⇒ OTA-State 5 `"failed"`,
  `report_ota_state`, Rückgabe `0xffffffff` — **kein Reboot**.
* **Zweite Ebene (nur MRVL-Container):** `FUN_1f043e40` liest nach dem MD5 den
  Container-Header (`0x14 B` je Eintrag, Magic `"MRVL"` + `0x2e9cf17b`), iteriert
  `local_2c` Segmente und prüft für Segmente vom Typ 2 die **Hardware-CRC32** (Marvell
  CRC-Engine, `FUN_1f043da4`) gegen den im Header hinterlegten Sollwert; Abweichung ⇒
  ebenfalls `0xffff9ff4`. Segmente der Typen 1/3/4 werden übersprungen.
* **Reine `"SBHF"`-Images** (kein MRVL-Container) überspringen MD5 + CRC komplett
  (`if (local_158[0] == 0x48464253) return 0;`) — dort prüft der Bootloader.
* **Randfall (ehrlich vermerkt):** schlägt der Flash-Treiber-Init oder das
  `FUN_1f058cf0(0x200)`-Malloc fehl, liefert `FUN_1f043cdc` **0 = "match"**, d. h. die
  MD5-Prüfung wird in diesem Fehlerfall stillschweigend als bestanden gewertet (der
  CRC32-Teil läuft weiter und schlägt dann über den Treiber-Handle fehl).

## 5. `-9999 user ack timeout` / `set_ps` / `developer_mode`

**Negativbefund, reproduzierbar:**

```
grep -ac "user ack"      upd_lumi.gateway.v3.elf  -> 0     (bin -> 0)
grep -ac -- "-9999"      upd_lumi.gateway.v3.elf  -> 0     (bin -> 0)
grep -ac "set_ps"        upd_lumi.gateway.v3.elf  -> 0     (bin -> 0)
grep -ac "developer_mode" upd_lumi.gateway.v3.elf -> 1
```

* `-9999 user ack timeout` und `set_ps` existieren in **diesem** Image nicht
  (Text im `.bin`-Volltext ebenso wenig). Die Konstante stammt aus der
  App-/Cloud-Seite (miIO-Tunnel: Client wartet auf das Ack des Geräts) bzw. aus
  einem anderen Build (`lumi.gateway` vs. Yeelight-Bulb-Firmware).
* Der ack-seitige Mechanismus im Image ist der Retry-Block in
  `FUN_1f008cf4` (4 × 3 s, Ack-Flag `DAT_20001bcc`).
* `developer_mode` ist vorhanden (1 Treffer) und wird im Property-Dispatch
  (`set_device_prop_JsDlgt @ 0x1f01342c`) behandelt (`FUN_1f01c920`).

## 6. Reproduktion

```bash
ssh devastator
cd /home/jarvis/projects/yeelight-re
export JAVA_HOME=/opt/jdk21 PATH=/opt/jdk21/bin:$PATH
# 1) headless Projekt + Analyse + Vollexport
/opt/ghidra_12.1.4/support/analyzeHeadless ghidra_workspace ylre \
  -import firmware/ref_88mw302/upd_lumi.gateway.v3.elf \
  -processor "ARM:LE:32:Cortex" -scriptPath ~/re_scripts -postScript MiioExport.java … all
# 2) Lückenbereiche (RPC-Handler-Callgraph + OTA-Worker) erzwingen
~/re_scripts/run_pass4.sh     # DisasDecompile.java mit gap_funcs.txt
```

Artefakte: `ghidra_workspace/{export,export2,export3,export4,export5}/`,
vollständige Dekompilation in `export3/decompiled/ALL.c`.
