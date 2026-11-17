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

**Verifikationskette:** Der Client liefert `file_md5`; im Image finden sich
zusätzlich die Strings `"user fw download failed, downloaded=%u, total=%u\n\r"`,
`"WiFi Firmware binary zer…"` und die Magic `"XZ"` (`\xfd7zXZ`) → Firmware-Payload
ist **XZ-komprimiert** und wird nach dem Download entpackt.

Statusabfragen:

| Handler | Adresse | Antwort |
| :--- | :--- | :--- |
| `get_ota_state` | `0x1f008068` | `{"result":[%s]}` aus Statustabelle `0x2000035c` (`"idle"`, …), UART `"ota state:%s\r\n"` |
| `get_ota_progress` | `0x1f0086c4` | `{"result":%d}` aus `FUN_1f00850c()`, UART `"ota progress:%u%\r\n"` |
| `report_ota_state` | `0x1f0084a0` | UART `"\r\nreport_ota_state: %s \r\n"`, setzt Property `ota_state`, `commit(FUN_1f007b20(…,5))` |

### Offener Punkt (ehrlich benannt)

Der OTA-Worker `0x1f007884` liegt in einem Analyse-Loch (`0x1f00787c`–`0x1f00799b`);
der erzwungene Decompiler-Lauf (`export5/000_ota_worker_task.c`) liefert
**registerbasierte, unzuverlässige** Ausgabe (`unaff_r6`). Die konkrete
MD5-Verifikation der heruntergeladenen Datei ist damit noch **nicht** byte-exakt
belegt — dort liegt der nächste Arbeitsschritt (manuelle Disassembly oder
Register-Propagation mit `-analysisTimeoutPerFile`).

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
