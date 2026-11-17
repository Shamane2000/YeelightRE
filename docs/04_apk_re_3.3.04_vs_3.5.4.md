# APK-RE: 3.3.04 vs. 3.5.4 — Provisioning- und LAN-Control-Gating im Vergleich

**APKs**
| Version | Datei | Hinweis |
|---|---|---|
| 3.3.04 | `yeelight_962_3.3.04_candidate.apk` | Yeelink-Signatur verifiziert, obfuskiertes `yeelib` |
| 3.5.4 | `yeelight_official_current.apk` | aktueller Play-Store-Stand, `yeelib` **unobfuskiert** |

**Methode:** Voll-Dekompilierung (`jadx -d jadx_3304`, deobf) für 3.3.04, plus gezielte
`--single-class`-Läufe für beide Versionen auf denselben Klassen. Ein 3.5.4-Vollbaum wurde nicht
erzeugt (RAM-Limit Devastator); die Aussagen unten stützen sich auf die direkt extrahierten Klassen.

---

## 1. Kernaussage

**Funktional ist der Provisioning- und LAN-Control-Flow in 3.5.4 unverändert.** Änderungen sind
(1) Obfuskierungsstatus des Yeelight-SDK, (2) Methoden-Umbenennungen, (3) ein Retry-Guard in
`ApConfigStep`. Es gibt **kein** neues Gating, das LAN-Control serverseitig oder per Feature-Flag abschaltet.

---

## 2. Provisioning (`ApConfigStep`)

Beide Versionen senden identisch:

| Prüfpunkt | 3.3.04 | 3.5.4 |
|---|---|---|
| `method` | `"miIO.config_router"` (Z. 489) | `"miIO.config_router"` (Z. 500) |
| `uid` | `people.getUserId()` (Z. 496) | `people.getUserId()` (Z. 507) |
| `bind_key` | `mBindkey` (Z. 497) | `mBindkey` (Z. 508) |
| `country_domain` | nur bei Locale ≠ `cn` (Z. 498–500) | nur bei Locale ≠ `cn` (Z. 510) |
| Token-Bezug | `MiioLocalAPI.async_get_token(gatewayAddr, cb, 9)` (Z. 511) | identisch (Z. 521–522) |
| `tz` / `gmt_offset` | vorhanden | vorhanden |

### Einziger funktionaler Unterschied: Retry-Guard

```java
// 3.3.04
MyLogger.getInstance().log(ApConfigStep.TAG, "setmiiorouter start");
...
if (ApConfigStep.this.getHandler() == null) {
    return;
}

// 3.5.4
MyLogger.getInstance().log(ApConfigStep.TAG, "setmiiorouter begin");
ApConfigStep.this.mHasSendWificonfig = true;
...
if (ApConfigStep.this.getHandler() == null || ApConfigStep.this.mCurrentIndex != 1) {
    return;
}
```

3.5.4 führt das Feld `private boolean mHasSendWificonfig` ein und bricht die Erfolgs-Weiterleitung ab,
wenn der Step inzwischen nicht mehr in Phase 1 steht. Das verhindert doppelte
`MSG_UPDATE_DEVICE_STATE`-Sprünge nach einem späten UDP-Response (Race zwischen Retry und
Nutzer-Abbruch). **Reines Robustheits-Fix, keine Protokolländerung.**

---

## 3. LAN-Control-Schalter

| Aspekt | 3.3.04 | 3.5.4 |
|---|---|---|
| Basisklasse | `com.yeelight.yeelib.c.h` (obfuskiert) | `com.yeelight.yeelib.device.WifiDeviceBase` (lesbar) |
| Setter LAN-Control | `M1(boolean)` (abstrakt in `c.h`) | `L1(boolean)` |
| Status-Getter | `d0().S()` | `d0().T()` |
| Implementierung | `F1().sendCmd(<Service>.cfg_lan_ctrl, z ? "1" : "0", cb)` | unverändert (gleiche `sendCmd`-Action) |
| QuickConnect-Default | `private boolean f9174g = true;` → `M1(f9174g)` | `private boolean f8255g = true;` → `L1(f8255g)` |
| Listen-Toggle | `hVar.M1(!hVar.d0().S())` | `wifiDeviceBase.L1(!wifiDeviceBase.d0().T())` |

### 3.3.04 (obfuskiert)

```java
// QuickConnectLanControlActivity
private boolean f9174g = true;
...
QuickConnectLanControlActivity.this.f9170c.M1(QuickConnectLanControlActivity.this.f9174g);

// LanControlListActivity
com.yeelight.yeelib.c.h hVar2 = hVar;
hVar2.M1(!hVar2.d0().S());
```

### 3.5.4 (deobfuskiert)

```java
// QuickConnectLanControlActivity
private boolean f8255g = true;
...
QuickConnectLanControlActivity.this.f8251c.L1(QuickConnectLanControlActivity.this.f8255g);

// LanControlListActivity
public static /* synthetic */ void b(WifiDeviceBase wifiDeviceBase, View view) {
    wifiDeviceBase.L1(!wifiDeviceBase.d0().T());
}
```

**Wichtig:** In 3.5.4 ist `M1(boolean)` weiterhin vorhanden, bedeutet dort aber etwas anderes
(z. B. `cfg_save_state`); der LAN-Control-Schalter ist auf `L1` gewandert. Wer Code gegen beide
Versionen schreibt, darf sich nicht auf den Methodennamen verlassen, sondern muss auf das
`CmdKey`-String-Argument `"cfg_lan_ctrl"` in der `sendCmd`-Action matchen.

---

## 4. Kanal-Gating (LAN vs. Cloud)

`DeviceManipulatorImpl.enableLanCtrl(boolean)` ist in beiden Versionen identisch:

```java
public void enableLanCtrl(boolean z) {
    ServiceManager.getInstance().getDeviceManipulator().enableLanCtrl(z);
}
```

`ChannelHelper.getChannel(...)` (unverändert, in beiden APKs aus `classes.dex`) wählt den lokalen
Kanal nur bei `isLanCtrlEnabled() && BSSID-Match && Location.LOCAL`, sonst Cloud.
→ **Kein Runtime-Gating-Unterschied zwischen 3.3.04 und 3.5.4.**

---

## 5. Nebenbefund: Obfuskierungswechsel

3.5.4 liefert das Yeelight-SDK (`com.yeelight.yeelib.*`) **unobfuskiert** aus, inklusive
`WifiDeviceBase`, `DeviceStatusBase`, `YeelightDeviceManager`, `AppUtils` und der
Interface-Pakete (`u3.e` statt `com.yeelight.yeelib.e.e`). Konsequenzen:

* RE gegen 3.5.4 ist deutlich schneller (lesbare Klassennamen, keine Deobf-Kollisionen).
* Für 3.3.04 bleibt der Umweg über `jadx --single-class com.yeelight.yeelib.c.<x>` nötig,
  da `--deobf` die Yeelight-Pakete in `p152c/p158m`-Namen umschreibt.
* Symbolische Namen (Feld-/Methodennummern) sind zwischen den Versionen **nicht** stabil —
  nur Protokoll-Strings (`miIO.config_router`, `cfg_lan_ctrl`, `sendCmd`, `CmdKey`, `CmdValue`)
  sind versionsübergreifend konstant.

---

## 6. Empfehlung

1. **Gegen Strings implementieren, nicht gegen Symbole:** `miIO.config_router`, `async_get_token`,
   `cfg_lan_ctrl`, `sendCmd`, `CmdKey`/`CmdValue`.
2. **Provisioning-Logik aus 3.5.4 übernehmen** (Retry-Guard) — sie ist die robustere Variante.
3. Offen (nicht Teil dieses Tickets): Disassemblierung von `libmiio.so`, um AES-Modus/Padding von
   `JNIBridge.encrypt` unabhängig von `python-miio` zu bestätigen.
