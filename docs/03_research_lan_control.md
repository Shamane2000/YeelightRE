# Recherchebericht — `yeelink.light.color2`, `miio_client` & LAN-Control (`-9999`, APK, Firmware)

**Task:** t_40834df2 (frodo) · **Datum:** 2026-11-17 · **Host:** devastator · **Repo:** `/home/jarvis/projects/yeelight-re`

---

## 0. TL;DR

1. **`-9999 user ack timeout`** ist kein Token-/Krypto-Fehler, sondern die **App-Layer-ACK-Timeout-Meldung des `miio_client`**: Xiaomi dokumentiert offiziell, dass das WiFi-Modul eine nach unten durchgereichte Methode nach **1 s** ohne `result`/`error` als fehlgeschlagen wertet. Ursache im AP-Modus: die Methode wird von der Geräte-Logik **nicht bedient** (nicht implementiert / in diesem Modus nicht geroutet / Mainloop blockiert).
2. **`set_developer_mode` ist kein Geräte-Methodenname.** python-miio mappt ihn auf `set_ps ["cfg_lan_ctrl","1"]`. Wird `"method":"set_developer_mode"` roh gesendet, ist `-9999` die erwartbare Antwort.
3. **Reihenfolge ist entscheidend:** LAN-Control **vor** `miIO.config_router` per `set_ps` aktivieren und `["ok"]` verifizieren (2 unabhängige Referenzimplementierungen). Nach dem Provisioning ist der miIO-Token tot — das ist **dokumentiertes Sollverhalten**, kein Bug.
4. **Firmware frei verfügbar:** `dgiese/dustcloud-documentation` enthält Roh-Dumps für `color2` (142-0027), `ct2` (142-0015), `mono1` (142.55), `color1` (142.0061), `ceiling1` (155.0180) — inkl. **IDA-Datenbanken (.idb)** für `color1` und `mono1`.
5. **APK 3.3.04 direkt abrufbar und authentisch signiert** (Yeelink-Cert) — aber LAN-Control-Code (`cfg_lan_ctrl`, `miIO.config_router`, `LanControlListActivity`) steckt **auch in 3.5.4**. Die UI-Abschaltung ist damit mit hoher Wahrscheinlichkeit **serverseitig/cloud-gesteuert**, nicht versionsbedingt.

---

## 1. Warum `-9999 "user ack timeout"`? (Primärquelle)

**Xiaomi/eigenes SDK-Repo `MiEcosystem/miio_open`**, PDF `md_images/new/MIIO模组串口指令详情.pdf` (S. 4–5, Abschnitt `get_down`/`json_get_down`):

> „MCU 在获得下行方法后，有 1s 时间处理，并用 result/error 命令上传结果。超过 1s 后，则 wifi 模块认为调用失败。“
> *(Nach Erhalt einer Downlink-Methode hat die MCU 1 s Zeit, das Ergebnis per `result`/`error` hochzuladen. Nach Ablauf dieser 1 s betrachtet das WiFi-Modul den Aufruf als fehlgeschlagen.)*

Architektur (`MiEcosystem/miio_open`, `wifi/README.md`, Abschnitte „WIFI串口命令" / „工作流程"):
Cloud → Modul (`miio_client`) → seriell `get_down` (Empfehlung: Polling alle 200 ms) → Business-Logik (MCU bzw. bei uns die Marvell-88MW302-Applikation) → Antwort `result "<payload>"` → Modul antwortet `ok`.
**Kommt kein `result`, entsteht modulseitig der Fehler, den wir als `{"code":-9999,"message":"user ack timeout"}` sehen.**

Unabhängige Bestätigungen, dass `-9999` = „Methode nicht bedient / Business-Logik antwortet nicht":
- `hypfer/Valetudo`, `lib/miio/MiioSocket.js`: Kommentar „*'user ack timeout' is sent by the miio_client if the robots business logic fails to respond*".
- `syssi/esphome-yeelight-ceiling-light`, `docs/flashing-over-the-network.md`: Tabelle „`-9999 user ack timeout` → method not implemented; firmware never acknowledges".
- `parhelion-rev/mijia-exporter`: `-9999 user ack timeout` bei **falschem Methodennamen** (`miio.info` statt `miIO.info`).
- `slightc/mi_home_cli` (README): alte Yeelight-Firmware antwortet auf `get_properties` (MIoT) mit `user ack timeout`, während `miIO.info` funktioniert → **Legacy-miIO-Methoden ≠ MIoT-Methoden**.
- `rytilahti/python-miio`, `miio/miioprotocol.py:320`: `RECOVERABLE_ERRORS = [-30001, -9999]` → `-9999` wird als **retry-fähig** behandelt (nicht als harter Fehler).

**Konsequenz für den AP-Modus:** `miIO.info` und `miIO.config_router` werden bedient, `set_ps` offenbar nicht. Zwei Erklärungen, beide testbar:
- **H1 (wahrscheinlich):** In AP/Provisioning-State routet die Applikation nur eine Whitelist (`miIO.info`, `miIO.config_router`, teils `get_prop`); `set_ps` wird verworfen → `-9999`.
- **H2:** Die verwendete Methode war schlicht `set_developer_mode` (existiert nicht) statt `set_ps`.

---

## 2. Kanonischer LAN-Control-Flow (belegte Quellen)

**python-miio** (`miio/integrations/yeelight/light/yeelight.py`):
```python
# Zeile 439-441
def set_developer_mode(self, enable: bool) -> bool:
    """Enable or disable the developer mode."""
    return self.send("set_ps", ["cfg_lan_ctrl", str(int(enable))])
```
→ Der CLI-Befehl `set_developer_mode` **ist** `set_ps`; das Flag wird über `get_prop` als Property `lan_ctrl` gelesen.

**AlexxIT/XiaomiGateway3** (`custom_components/xiaomi_gateway3/core/core_utils.py:205-214`) — vollständiger Flow inkl. Read-back:
```python
resp = await device.send("get_prop", ["lan_ctrl"])      # ["1"] = schon aktiv
if resp.get("result") == ["1"]: return "Already enabled"
resp = await device.send("set_ps", ["cfg_lan_ctrl", "1"])
if resp.get("result") == ["ok"]: return "Enabled"
```

**Wassim24/lumiere** (`src/net/pairing.ts`, `reference/yeelocal.py`, `docs/spec.md`) — cloud-freies Pairing, deckt exakt unseren Fall ab:
1. Hotspot `yeelink-light-*` beitreten, Gateway i. d. R. `192.168.10.1`
2. miIO-Hello (32 B: `21 31 00 20` + 28×`ff`), Port UDP 54321 → `did`, `stamp`, `token`
   *„Une ampoule non appairée renvoie son token en clair. Une ampoule appairée renvoie `ff…ff` ou `00…00`"* → bei uns (`stamp=9`, `ffff…`) also **bereits provisioniert**.
3. **`miIO.info` `[]`** (optional: `model`, `mac`)
4. **`set_ps ["cfg_lan_ctrl","1"]` → muss `["ok"]` liefern** — *„Indispensable : c'est ce qui permet le pilotage ensuite"* (3 Wiederholungsversuche)
5. **`miIO.config_router`** `{"ssid":…, "passwd":…, "uid":0, "tz":"<IANA>"}` — ein Timeout hier ist **kein** Fehler (Bulb wechselt das Netz)
6. Danach TCP `55443` (`get_prop`) statt miIO; *„Ne pas conserver le token : il est invalide après l'appairage"*

**Wichtig für AP-Modus:** lumiere führt Schritt 4 **im AP-Modus** mit dem Klartext-Token aus und erhält auf echten Bulbs `["ok"]` — d. h. `set_ps` **kann** im AP-Modus funktionieren. Dass es bei unserem Testbulb `-9999` gibt, ist damit modell-/firmware-spezifisch (fw `2.0.6_0065`, hw `mw302`) und muss gezielt gegengeprüft werden.

**Zusätzlicher, unabhängiger Beleg zur Token-Gültigkeit** (`lumiere/reference/yeelocal.py`, Docstring):
> „Tokens are only handed out by a bulb that has not been provisioned yet, so pair bulbs with this tool (after a factory reset) to keep their token."

---

## 3. Token-Rotation / „AP-Token überschreiben" — verhinderbar?

**Befund: nein, nicht per Parameter.** Die Maskierung (`ffff…`/`0000…`) ist lokales Verhalten des Moduls nach erfolgreichem Netzwechsel/Bind; sie ist bei lumiere, in unseren Live-Findings (`docs/01_live_findings.md`, `stamp=9`) und in Xiaomis miIO-Design unabhängig dokumentiert. Sie ist **kein** Cloud-Effekt, den man mit `uid`/`bind_key`/`country_domain` abschalten könnte.

Belegte `config_router`-Parameter (aus der App-DEX, s. §5): `ssid`, `passwd`, `tz`, `uid` sowie — als App-seitige Log-/Aufbau-Strings vorhanden — `bind_key` und `country_domain` (`"bind_key = %s"`, `"country_domain = %s"`, `"miIO.config_router"`).

Testbare Hypothesen für `mrrobot` (in dieser Reihenfolge, keine davon ist belegt):
- **A (empfohlen):** Reihenfolge fixen — Factory-Reset → AP-Modus → `set_ps ["cfg_lan_ctrl","1"]` **vor** `config_router`, `["ok"]` verifizieren. Damit ist der Token danach irrelevant (TCP 55443 braucht keinen).
- **B:** `config_router` **ohne** `uid`/`bind_key`/`country_domain` (nur `ssid`,`passwd`,`tz`) → prüfen, ob die Bulb überhaupt provisioniert und ob der Token danach noch gilt (Hypothese: Bind an die Cloud triggert die Rotation; unbestätigt).
- **C:** WAN-Sperre (Router-Firewall / kein DNS) während der Provisionierung → verhindert Cloud-Bind, ändert aber sehr wahrscheinlich **nichts** an der lokalen Token-Maskierung. Nur als Gegenprobe sinnvoll.
- **D:** Wenn `set_ps` im AP-Modus weiter `-9999` liefert: `get_prop ["lan_ctrl"]` als Read-back prüfen (evtl. ist es bereits `1`), 3× mit Re-Hello + frischem `stamp` wiederholen (lumiere-Praxis), und `-9999` als retry-fähig behandeln (python-miio-Semantik).

**Empfohlene konkrete Frames (AP-Modus, Klartext-Token):**
```
{"id":<n>,"method":"miIO.info","params":[]}
{"id":<n>,"method":"get_prop","params":["lan_ctrl"]}
{"id":<n>,"method":"set_ps","params":["cfg_lan_ctrl","1"]}     → erwartet {"result":["ok"]}
{"id":<n>,"method":"miIO.config_router","params":{"ssid":"…","passwd":"…","uid":0,"tz":"Europe/Berlin"}}
```
Packet-Bau (lumiere `docs/spec.md` §2.1, deckungsgleich mit mihome-binary-protocol):
`header = 0x2131 | BE16(32+len(body)) | BE64(did) | BE32(stamp+1)`; `packet = header(16) ‖ MD5(header‖token‖body) ‖ body`; Key `MD5(token)`, IV `MD5(key‖token)`. Vor **jeder** Anfrage frisches Hello für `stamp`.

---

## 4. Firmware-Quellen (offen, ohne Cloudflare)

**Repo `dgiese/dustcloud-documentation`** (public, raw.githubusercontent.com — direkt per curl, kein Bot-Schutz). Vollständige Liste der Yeelight-Firmware-Blobs:

| Modell | Pfad | Größe |
|---|---|---|
| `yeelink.light.color2` | `yeelink.light.color2/firmware/142-0027/0cb40a3fe9abbaa4689d9627e5430256_upd_yeelink.light.color2.bin` | 317 820 B |
| `yeelink.light.color1` | `yeelink.light.color1/firmware/142.0061/a0aa379b…_upd_yeelink.light.color1.bin` + `2363966f…_upd_yeelink.light.color1.bin` + **`.bin.idb` (5,1 MB)** + section-Dumps | 311–317 KB |
| `yeelink.light.ct2` | `yeelink.light.ct2/firmware/142-0015/50e36abab2bab04d047b8f8020ac2f6e_upd_yeelink.light.ct2.bin` | 305 260 B |
| `yeelink.light.mono1` | `yeelink.light.mono1/firmware/142.55/ffa341cf…_upd_yeelink.light.mono1.bin` + **`.idb` (4,8 MB)** + section-Dumps + `info.txt` | 303 668 B |
| `yeelink.light.ceiling1` | `yeelink.light.ceiling1/firmware/155.0180/{295914e7…,bbddec14…}_upd_yeelink.light.ceiling1.bin` | 636 KB |

**Verifiziert heruntergeladen** (color2, direkt per `curl`, HTTP 200):
```
URL    : https://raw.githubusercontent.com/dgiese/dustcloud-documentation/master/yeelink.light.color2/firmware/142-0027/0cb40a3fe9abbaa4689d9627e5430256_upd_yeelink.light.color2.bin
Größe  : 317820
SHA256 : 99977ae46e0a83b3159609bb9e43b0afd9cf1ae6872423d7b94ce66d3db527b9
Header : 4d52564c 7bf19c2e … ("MRVL{" → Marvell-Image, gleiches Format wie upd_yeelink.light.color2.bin)
```
→ **Wichtig für RE:** die beiden **`.idb`-Dateien** (IDA-Pro-Datenbanken für `color1` und `mono1`) sind der schnellste Weg zu Symbolen/Struktur des 88MW3xx-Images; sie sind mit dem color2-Build eng verwandt (gleiche Plattform, `mw302`/`mw300`).

Weitere Fundstellen:
- `MiEcosystem/miio_open`, `wifi/MHCW03P/factory_client.zip` (16,3 MB, heruntergeladen): Factory-Client + Firmware-Verzeichnis; `wifi/README.md`, `wifi/基于MIIO模组的WiFi产品设计规范.md`, `md_images/new/MIIO模组串口指令详情.pdf` (Primärquelle für §1).
- `vacuum.mindsolo.net/en/firmwares/2844/versions/12586` listet eine color2-Firmware („Версия 12586", Next.js-App) — **kein direkter Datei-Link im HTML** (Download nur clientseitig). Kein Link rekonstruierbar, nicht weiterverfolgt.
- **Nicht** in den Mirrors vorhanden: unser Live-Build `2.0.6_0065` (fw_ver=65) sowie `2.0.7_0076`. Also: ältere Dumps ja, aktueller Build nein.

---

## 5. APK `com.yeelight.cherry` — alter Build mit LAN Control

### 5.1 Gefundene, direkt abrufbare Quellen (kein Anti-Bot-Trick)

**A) Yeelight-CDN (offiziell, dauerhaft):**
```
https://fe-resource.yeelight.com/app/package/yeelight.apk
HTTP/2 200 · 38 089 207 B · last-modified 2025-09-24 (Tencent COS)
versionName 3.5.4 · versionCode 35402 · minSdk 19 · targetSdk 33
SHA256 abdcffbcf118e5d5c48088a478c8ea49f4c3fa911db1f1bc5013fed1d61fb7f2
```

**B) Spiegel mit 3.3.04 (verifiziert, signaturgeprüft):**
```
https://9az.197784.com/9az11/com.yeelight.cherry.apk
HTTP/2 200 · 40 423 365 B · last-modified 2021-05-18 · direkt per curl, ohne Challenge
versionName 3.3.04 · versionCode 3304 · minSdk 19 · targetSdk 29
SHA256 d42ddd5b77d501546fbba945119f973e438d7fac0868816714a05fd88d21e43f
Signatur: META-INF/CERT.RSA, subject/issuer "C=86, ST=SD, L=QD, O=Yeelink, OU=Yeelink, CN=Yeelink"
          (identischer Signer-DN wie das offizielle 3.5.4-APK von fe-resource.yeelight.com)
Lokal: devastator:/home/jarvis/projects/yeelight-re/apk/yeelight_962_3.3.04_candidate.apk
```
Hinweis: Die Datei wird über einen Cloudflare-Edge ausgeliefert, antwortet aber **ohne** Challenge/CAPTCHA auf einen normalen GET — es wurde **keine** Umgehung eingesetzt.

**C) Negativ-Ergebnisse (dokumentiert, damit niemand doppelt sucht):**
- `archive.org` (`advancedsearch.php`, Query `"com.yeelight.cherry" OR "yeelight"`, 63 Treffer): **kein** APK-Upload.
- `uptodown.com` (nicht CF-geschützt, versions-Liste auslesbar): älteste gelistete Version ist **3.3.08** (`/android/download/1135550744`, Download nur über JS-Zwischenschritt, keine direkte Datei-URL im HTML). **3.2.x nicht vorhanden.**
- APKMirror/APKPure/APKCombo: Cloudflare-Challenge → **nicht angetastet** (Vorgabe).

### 5.2 Statische Auswertung: LAN-Control-Code in beiden Versionen **vorhanden**

Methode: `AndroidManifest.xml` via `androguard.core.axml.AXMLPrinter`, Strings aus `classes*.dex` (DEX-String-Pool, `strings -a`), Vergleich 3.3.04 ↔ 3.5.4.

| Fund | 3.3.04 | 3.5.4 |
|---|---|---|
| `cfg_lan_ctrl` (PS-Key) | ✅ | ✅ |
| `miIO.config_router` | ✅ | ✅ |
| `set_developer_mode` (String) | ❌ | ❌ (erwartbar: ist nur ein python-miio-Alias) |
| Klassen `…ui.activity.LanControlListActivity`, `…QuickConnectLanControlActivity` | ✅ | ✅ |
| Felder/Keys `enableLanCtrl`, `isLanCtrlEnabled`, `lan_control_switch` | ✅ | ✅ |
| Strings `bind_key = %s`, `country_domain = %s` (config_router-Aufbau) | ✅ | ✅ |

**Interpretation (als Hypothese markiert):** Da der LAN-Control-Code in 3.5.4 unverändert enthalten ist, ist die von Usern beobachtete „fehlende LAN-Control-Option" **nicht** durch Entfernen des Codes entstanden — sie wird very likely **zur Laufzeit gegated** (Cloud-/Account-/Geräte-Flag, z. B. Mi-Brand-Geräte). Konsequenz: Das Installieren von 3.3.04 kann funktionieren (historische Nutzerberichte), ist aber **kein garantierter Weg** — der belegbare, robuste Weg bleibt der RPC `set_ps ["cfg_lan_ctrl","1"]` (Abschnitt 2).

Nebenbefund: `lumiere/reference/yeelocal.py` (Kommentar Z. 4-5) bestätigt unabhängig, dass **3.5.4** beide lokalen Transporte nutzt: miIO/UDP 54321 (AES, `libmiio.so`) **und** LAN-Text-Protokoll TCP 55443.

---

## 6. Aufgabenbezogene Antworten (kompakt)

**Task 1 — `miio_open` / `wmsdk`, warum `-9999`, wann `cfg_lan_ctrl` gespeichert wird:**
`-9999` = 1-s-ACK-Timeout des Moduls (Primärquelle §1). `cfg_lan_ctrl` wird als Property-Persistenz `set_ps` geschrieben; **Speicherzeitpunkt = wenn `["ok"]` zurückkommt**. Kommt `-9999`, wurde nichts gespeichert. Ein `marvell-iot/wmsdk`-Repo mit identifizierbarer `miio_client`-Quelle ließ sich **nicht** verifizieren (nicht öffentlich auffindbar) — die Architektur ist aber über `MiEcosystem/miio_open` (MCU+Modul-Modell) und `libmiio.so` in der App belegt.

**Task 2 — signaturfreie Dumps/OTA-URLs:** siehe Tabelle §4 (color2/ct2/mono1/color1/ceiling1 inkl. .idb). Keine offiziellen OTA-URLs (die `*.mi.com`-OTA-Endpunkte sind account-/signaturgebunden); `mindsolo` listet color2-Versionen, aber ohne direkt abrufbare Datei.

**Task 3 — Token-Überschreiben via `config_router` verhindern:** nein (§3). Belegte Parameter: `ssid`, `passwd`, `tz`, `uid`, `bind_key`, `country_domain`. Empfehlung: Reihenfolge (LAN-Control zuerst) + Factory-Reset, nicht Parametertricks.

---

## 7. Offene Punkte / Unsicherheiten

1. Ob `set_ps` auf **fw 2.0.6_0065** im AP-Modus grundsätzlich `-9999` liefert (dann: Methoden-Whitelist im Provisioning-State) oder ob im Testlauf die falsche Methode gesendet wurde — **nur empirisch klärbar** (Testmatrix §3 A/D).
2. Ob 3.3.04 die LAN-Control-UI für ein **Mi-Brand-Gerät** (`YLDP06YL`) tatsächlich anzeigt — statisch nicht entscheidbar (Cloud-Gating); Gerätetest nötig.
3. Das genaue Cloud-Flag, das die UI in 3.5.4 versteckt, ist nicht identifiziert (Kandidaten: `lan_control_switch`, `isLanCtrlEnabled`).
4. `marvell-iot/wmsdk` nicht verifizierbar; falls Zugriff auf einen `libmiio.so`-Dump möglich ist, wäre das der direkte Weg zum Methoden-Routing.

---

## 8. Quellen (alle am 2026-11-17 geprüft)

- `https://github.com/MiEcosystem/miio_open` — `wifi/README.md`, `wifi/基于MIIO模组的WiFi产品设计规范.md`, `md_images/new/MIIO模组串口指令详情.pdf` (1-s-ACK-Regel), `wifi/MHCW03P/factory_client.zip`
- `https://github.com/dgiese/dustcloud-documentation` — Firmware-Blobs §4 (raw-URLs verifiziert)
- `https://github.com/rytilahti/python-miio` — `miio/integrations/yeelight/light/yeelight.py:439-441`, `miio/miioprotocol.py:320`
- `https://github.com/AlexxIT/XiaomiGateway3` — `custom_components/xiaomi_gateway3/core/core_utils.py:205-214`
- `https://github.com/Wassim24/lumiere` — `docs/spec.md` (§2.1, §3), `src/net/pairing.ts`, `reference/yeelocal.py`
- `https://github.com/hypfer/Valetudo` — `lib/miio/MiioSocket.js` (`-9999`-Semantik)
- `https://github.com/syssi/esphome-yeelight-ceiling-light` — `docs/flashing-over-the-network.md` (`-9999`-Tabelle)
- `https://github.com/parhelion-rev/mijia-exporter` — `-9999` bei falschem Methodennamen
- `https://github.com/slightc/mi_home_cli` — Legacy-miIO vs. MIoT (`user ack timeout`)
- `https://github.com/esauvisky/emyee` — `lan_reset.py` (`set_ps` zum Deaktivieren)
- `https://fe-resource.yeelight.com/app/package/yeelight.apk` — offizielles 3.5.4-APK
- `https://9az.197784.com/9az11/com.yeelight.cherry.apk` — 3.3.04-Spiegel (Yeelink-signiert)
- `https://vacuum.mindsolo.net/en/firmwares/2844/versions/12586` — color2-Firmware-Listing (kein Direktlink)
- `https://archive.org/advancedsearch.php?q=%22com.yeelight.cherry%22` — Negativbefund
- Lokal: `docs/01_live_findings.md`, `docs/02_defcon26_88mw30x_notes.md`, Testbulb `did=05eb8034` (`yeelink.light.color2`, `hw_ver=mw302`, `fw_ver=2.0.6_0065`)
