# Yeelight-App: Was haben neuere Versionen gesperrt? (APK-Diff 3.2.34 → 3.5.4)

Task: `t_bca6bc0a` · Modell-Fokus: `yeelink.light.color` (**YLDP02YL**, 88MW300)
Analysierte APKs (APKPure, SHA-256 in der Vault-Notiz `yeelight-firmware-apk-uebersicht.md`):

| Version | versionCode | DEX-Dateien |
| --- | --- | --- |
| 3.2.34 | 3234 | classes.dex, classes2.dex |
| 3.2.58 | 3258 | " |
| 3.3.04 | 3304 | " |
| 3.3.18 | 3318 | " |
| 3.4.13 | 3413 | " |
| 3.5.4 | 35402 | " |

Methodik: `androguard` (kein Voll-JadX nötig, DEX-Ebene reicht für Instruktions-/String-Diffs).
Werkzeuge im Repo (`tools/`):

- `apk_stringsets.py <apk> <out.txt>` — Menge aller `const-string`-Literale je Methode (versionsweise, für `comm`/Diff).
- `apk_string_diff.py <alt.txt> <neu.txt> <alt> <neu> <out.md>` — kategorisierter Diff-Report.
- `apk_method_owner.py <apk> <needle> --any --owners` — welche Methode referenziert einen String/Descriptor (mit Instruktionszahl = Größenmetrik).
- `apk_class_dump.py <apk> <klassen-substring>` — Dalvik-Dump einer Klasse/Methode.

Artefakte: `analysis/apk_excerpts/strings_<version>.txt` (regenerierbar), `analysis/apk_excerpts/diff_3.2.34_vs_3.5.4.md`.

---

## Ergebnis in einem Satz

Der **Protokoll-Funktionsumfang ist über alle sechs Versionen identisch** (`miIO.config_router`, `cfg_lan_ctrl`, `miIO.stop_diag_mode` in jeder Version vorhanden, 0 String-Diff im `miIO.*`-Namensraum). Neuere Versionen sperren **kein** Kommando ab — sie **binden das Provisioning an ein Mi-Cloud-Konto** (`bind_key`) und haben zwei **alte Hilfspfade** entfernt (ESP-**SmartConfig** und **Cherry-BLE-OAD**). „`set_ps`" und „`set_developer_mode`" existieren in **keiner** Version — die These „neuere App hat Direkt-Kommandos gesperrt" ist damit widerlegt.

---

## 1. Provisioning `miIO.config_router` — offen, aber ab 3.3.04 mit `bind_key`

`Lcom/miot/service/connection/wifi/step/ApConfigStep;->setMiioRouter` (UDP 54321 → `MiioLocalAPI.async_get_token(json, resp, 9)`).

**3.2.34 (93 instr) — Params ohne bind_key:**

```
const-string v2, "id"
const-string v2, "method"
const-string v3, "miIO.config_router"
const-string v3, "ssid"
const-string v5, "passwd"
const-string v6, ""
const-string v5, "uid"
const-string v5, "country_domain"
const-string v6, "tz"
const-string v5, "gmt_offset"
invoke-static v5, v6, v7, Lcom/xiaomi/miio/MiioLocalAPI;->async_get_token(Ljava/lang/String; Lcom/xiaomi/miio/MiioLocalRpcResponse; I)V
```

**3.5.4 (101 instr) — `bind_key` zusätzlich im Params-Objekt:**

```
const-string v5, "ssid"
const-string v7, "passwd"
const-string v8, ""
const-string v7, "uid"
const-string v7, "bind_key"
const-string v7, "country_domain"
const-string v8, "tz"
invoke-static v7, v8, v9, Lcom/xiaomi/miio/MiioLocalAPI;->async_get_token(Ljava/lang/String; Lcom/xiaomi/miio/MiioLocalRpcResponse; I)V
```

Größenverlauf (Instruktionen = Proxy für „wurde etwas ergänzt?"), verifiziert über alle sechs APKs:

| Version | `ApConfigStep.setMiioRouter` | `BleComboStep.setMiioRouter` |
| --- | --- | --- |
| 3.2.34 | 93 | 90 |
| 3.2.58 | 94 | 90 |
| 3.3.04 | **101** | **93** |
| 3.3.18 | 101 | 93 |
| 3.4.13 | 101 | 93 |
| 3.5.4 | 101 | 93 |

→ Der Sprung passiert genau bei **3.3.04**: Ab da holt der Client vor dem Provisioning einen `bind_key` aus der Mi-Cloud (`MiotCloudApi.getBindKey` → `POST /user/get_bindkey`, Host `api.io.mi.com`) und schickt ihn mit. Das Gerät wird damit **während** des Provisionings an das Konto gebunden.

Konsequenz für den Firmware-Neubau: Der `config_router`-RPC selbst ist unverändert (kein Feld umbenannt, kein HMAC ergänzt, `async_get_token`-Signatur identisch = gleicher `hencrypt`-Handshake). Ein alter Client (3.2.34) kann weiter provisionieren, nur ohne Cloud-Bindung — das Feld ist offenbar optional, wird aber von neuerer Firmware evtl. anders behandelt (auf dem Gerät zu verifizieren, siehe Folgepunkte).

## 2. LAN-Control (`cfg_lan_ctrl` / GeekMode) — **nicht** gesperrt

Der Toggle existiert in **allen** Versionen und ist identisch aufgebaut. 3.2.34 (Modellklasse `com.yeelight.yeelib.device.e.p` = color):

```
--- d  (25 instr)
const-string v0, "setGeekMode"
...
sget-object v0, Lcom/yeelight/yeelib/device/service/ColorService$a;->c   # c == "cfg_lan_ctrl"
const-string v1, "1"
const-string v2, "0"
invoke-virtual ... sendCmd(String; String; CompletionHandler)
```

3.5.4 (obfuskiert: `Lr3/a0;->L1`, 21 instr) — gleiche Kette, nur mit ent-obfuskiertem Enum:

```
const-string v0, "setGeekMode"
sget-object v0, Lcom/yeelight/yeelib/device/service/Lamp2Service$CmdKey;->cfg_lan_ctrl
const-string v1, "1"
const-string v2, "0"
```

Was sich geändert hat, sind nur **Log-/UI-Strings**, die in 3.5.4 fehlen:

- nur in 3.2.34: `setGeekMode --> onSucceed`, `setGeekMode --> onFailed`, `GeekModeFeature, destroyFeatureView!`

→ Die Fähigkeit ist da; was neuere Versionen „verschlossen" haben, ist die **Auffindbarkeit im UI** (Feature-View wird nicht mehr aufgebaut), nicht der Kommandopfad.

## 3. OTA — cloud-gated, unverändert

Firmware-Update-Pfad in beiden Versionen strukturell gleich (`yeelib/d/x` in 3.2.34 ↔ `yeelib/managers/d0` in 3.5.4):

```
const-string v0, "/gingko"                                   # Zielverzeichnis
const-string v3, "Download gingko firmware, download url: "  # URL aus Cloud-Helper (g$c / h$e)
const-string v3, "Download gingko firmware, firmware storage path: "
→ android.app.DownloadManager$Request + File.exists()/mkdirs()
```

Metadaten kommen weiterhin aus `miIO`-Cloud (`miIO.info`, `latest_version`); `miIO.ota`/`miIO.start_ota`/`set_ota` kommen in **keiner** Version als App-Kommando vor. Der Update-Weg ist also in alt **und** neu Cloud-/Login-gebunden — hier hat sich nichts verschlechtert, er war nie lokal.

## 4. Was neuere Versionen wirklich entfernt haben

| Capability | 3.2.34 | 3.2.58 | 3.3.04 | 3.3.18 | 3.4.13 | 3.5.4 |
| --- | --- | --- | --- | --- | --- | --- |
| `miIO.config_router` (Provisioning) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| `cfg_lan_ctrl` / `setGeekMode` | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| `miIO.stop_diag_mode` | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| **ESP-SmartConfig-Provisioning** | ✅ | ❌ | ❌ | ❌ | ❌ | ❌ |
| **Cherry-BLE-OAD (`startOad`)** | ✅ | ❌ | ❌ | ❌ | ❌ | ❌ |
| `bind_key` im Provisioning-Payload | ❌ | ❌ | ✅ | ✅ | ✅ | ✅ |
| `miIO.set_ps` / `set_developer_mode` | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ |

Belege:

**SmartConfig** — `const-string "SmartConfig"` nur in 3.2.34 (1 Treffer), in 3.2.58–3.5.4 je 0 Treffer (ESP-Touch-Kompatibilitätspfad für ältere Gerätegenerationen, fällt mit dem Wechsel auf `ApConfigStep`/`BleComboStep` weg).

**Cherry-BLE-OAD** — `Lcom/yeelight/yeelib/device/e/m;->P` (42 instr) in 3.2.34, Log-Tag `BLE_CONNECT`:

```
const-string v1, "CherryDevice, startOad!"
invoke-virtual v5, Lcom/yeelight/yeelib/device/e/m;->O()Z       # Precondition (verbunden/Version ok)
const/16 v0, 12 ; new-array v0, v0, [B                           # 12-Byte-Header
... m$b->a (S) via a(S)/b(S) → Header-Byte 0/1, m$b->b (I) → Byte 2/3
arraycopy(m$b->d[B], 0, v0, 4, 4)                                # 4-Byte Payload
invoke-virtual v1, v0, Lcom/yeelight/yeelib/device/c/b;->a([B)V   # Block über BLE-GATT schreiben
```

Zugehörige Literale (nur 3.2.34, sonst nirgends): `oad upgrade, send block: `, `oad upgrade, send block failed!`, `oad upgrade, all blocks done! blocks: `, `start ota, load mcu file succeed?`, `Cherry device, trying to upgrade now!`, `Cherry device, trying to upgrade 2541 now!`, `############# OAD upgrade, complete! `. Zählung über alle Versionen: 13 OAD-bezogene Literale (3.2.34) vs. 5 (3.2.58–3.5.4, nur generische Download-Strings).

→ **Das ist der interessanteste Verlust:** In 3.2.34 konnte die App MCU-Firmware blockweise per BLE auf ein Cherry-Gerät schieben (`startOad`, 12-Byte-Header + 4-Byte-Blöcke). Ab 3.2.58 existiert dieser Pfad nicht mehr — nur noch der Cloud-OTA über `DownloadManager`.

## 5. Nebenbefund: Obfuskation

3.2.34 ist im `device`-Paket noch lesbar (`com.yeelight.yeelib.device.e.p`, `...service.ColorService$CmdKey`), ab 3.4.13/Build 3413 sind Geräte-Klassen zu `Lr3/*` obfuskiert (nur `Lamp2Service$CmdKey` blieb benannt). Das ist ein Analyse-Hindernis, **keine** Sperre.

---

## Handlungsempfehlungen für den Firmware-Neubau

1. **Provisioning-Client auf 3.2.34-Basis** bauen (`config_router` ohne `bind_key`) → volle Kontrolle, kein Cloud-Zwang; `MiioLocalAPI.async_get_token` (UDP 54321, Token `ff..ff`, `hencrypt`) unverändert übernehmbar.
2. **LAN-Control** über `cfg_lan_ctrl` „1"/„0" ist geräteseitig in allen App-Generationen erreichbar — Firmware muss es nur weiter akzeptieren (in der color-FW zu prüfen).
3. **BLE-OAD** aus 3.2.34 als Referenz für einen lokalen Flash-Weg evaluieren (Header-Format + Block-Schreiben) — Alternativweg zum SPI-Dump.
4. **SmartConfig** nur relevant, falls Geräte der 2018er-Generation mit altern Firmware provisioniert werden sollen.
