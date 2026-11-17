# APK-RE: Provisioning, Token-Handling und LAN-Control (com.yeelight.cherry 3.3.04)

**APK:** `yeelight_962_3.3.04_candidate.apk` (Yeelink-Signatur verifiziert)
**Dekompiliert mit:** `/home/jarvis/tools/jadx/bin/jadx`, `JAVA_HOME=/opt/jdk21`
**Methodik:** Voll-Dekompilierung (`jadx -d jadx_3304`, deobf) für Quer-Referenzen + gezielte
`--single-class`-Läufe für die obfuskierten Yeelight-Klassen (`com.yeelight.yeelib.c.*`).
Da `jadx --deobf` die Yeelight-Pakete umbenennt, wird in diesem Dokument **beides** genannt:
Obfuskierter Originalname (so, wie er im echten Stacktrace/DEX steht) und JADX-Deobf-Name.

---

## 1. Provisioning: `miIO.config_router`

Klasse: `com.miot.service.connection.wifi.step.ApConfigStep`
Methode: `setMiioRouter(String ssid, String passwd, Class cls)` (Zeilen 478–512)

```java
public void setMiioRouter(String str, String str2, Class cls) {
    MyLogger.getInstance().log(TAG, "setMiioRouter, ssid = " + str + ", pw = " + str2 + ", BEGIN!");
    People people = (People) SmartConfigDataProvider.getInstance().get("people");
    AppConfiguration.Locale locale = ServiceManager.getInstance().getAppConfig().getLocale();
    if (people == null) { finishSmartConfig(true); return; }
    JSONObject jSONObject = new JSONObject();
    try {
        jSONObject.put("id", this.mId);
        jSONObject.put("method", "miIO.config_router");
        JSONObject jSONObject2 = new JSONObject();
        jSONObject2.put("ssid", str);
        if (str2 == null) { str2 = ""; }
        jSONObject2.put("passwd", str2);
        jSONObject2.put("uid", Long.valueOf(people.getUserId()));
        jSONObject2.put("bind_key", this.mBindkey);
        if (locale != null && locale != AppConfiguration.Locale.cn) {
            jSONObject2.put("country_domain", locale.toString());
        }
        jSONObject2.put("tz", new GregorianCalendar().getTimeZone().getID());
        jSONObject2.put("gmt_offset", (Integer) ...get(KEY_TIME_GMT_OFFSET, 480));
        jSONObject.put("params", jSONObject2);
    } catch (JSONException unused) { }
    String gatewayAddr = getGatewayAddr();
    if (TextUtils.isEmpty(gatewayAddr)) { return; }
    MyLogger.getInstance().log(TAG, "async_get_token start");
    MiioLocalAPI.async_get_token(gatewayAddr, new AnonymousClass7(jSONObject), 9);
}
```

### Payload-Struktur (verifiziert)

```json
{
  "id": 1731892800,
  "method": "miIO.config_router",
  "params": {
    "ssid": "<WLAN-SSID>",
    "passwd": "<WLAN-Passwort, leer erlaubt>",
    "uid": 1234567890,
    "bind_key": "<bindkey aus MiotCloudApi.getBindKey>",
    "country_domain": "de",
    "tz": "Europe/Berlin",
    "gmt_offset": 60
  }
}
```

| Feld | Herkunft im Code |
|---|---|
| `id` | `this.mId = (int)(System.currentTimeMillis()/1000)` — Sekunden-Timestamp, in `startConnectionAsso()` gesetzt (Zeile 629) |
| `uid` | `people.getUserId()` — Xiaomi-Account-ID des angemeldeten Nutzers |
| `bind_key` | `mBindkey`, gefüllt in `getBindkey()` (Zeile 163 ff.) aus `MiotCloudApi.getBindKey(...)` → `jSONObject.optJSONObject("result").optString("bindkey")`. Bei Fehler Retry alle 2 s (`onFailure`) |
| `country_domain` | `AppConfiguration.Locale.toString()` — **nur** wenn Locale ≠ `cn`. Für `de` also immer gesetzt |
| `tz` / `gmt_offset` | `TimeZone.getID()` / GMT-Offset in Minuten aus `SmartConfigDataProvider` (Default 480 = UTC+8) |
| Ziel-IP | `getGatewayAddr()` = `long2Ip(WifiManager.getDhcpInfo().gateway)` → im AP-Modus typisch `192.168.4.1` |

### Ablaufkette (vollständig)

1. `startConnection(0)` → `getBindkey()` → holt `bindkey` aus der miot-Cloud.
2. `startConnectToDeviceAp()` → verbindet per `ConnectionUtils.connectToAP(...)` mit dem Geräte-AP
   (`scanResult.SSID`, `scanResult.BSSID`), Timeout 15 000 ms (`MSG_RECONNECT_DEVICE_AP`).
3. `startConnection(1)` → `startConnectionAsso()` → bindet Prozess an das WLAN-Netz
   (`bindProcessToNetwork`) und ruft `setMiioRouter(ssid, passwd, ...)`.
4. `async_get_token(gateway, cb, 9)` → Gerät liefert `did` + `token` (siehe §2).
5. Im Callback `AnonymousClass7.onResponse`: `mDid` / `mToken` übernehmen,
   `mDeviceFinder.addToken(did, token)`, **dann**
   `MiioLocalAPI.async_rpc(gateway, configRouterJson, did, token, cb)` — verschlüsselt mit dem Geräte-Token.
6. Antwort `parseRpcResponse(...) != null` → `MSG 112` (Erfolg), sonst `MSG 114` → Retry nach 1 s.
7. Teardown: `stopConnection()` sendet `{"id":…, "method":"miIO.stop_diag_mode","params":""}` per `async_rpc`.

**Wichtig:** `config_router` wird **nicht** per UDP-Broadcast/SmartConfig-Paket gesendet, sondern als
verschlüsselter miIO-RPC über **UDP Port 54321 direkt an das Gateway** (Geräte-AP-Modus).

---

## 2. Token-Handling

Klasse: `com.xiaomi.miio.MiioLocalAPI` (1 106 Zeilen), Krypto über `JNIBridge` (native `libmiio.so`).

### Konstanten

```java
private static final int UDP_PORT = 54321;
private static final int UDP_TIMEOUT_REQ = 2000;
private static final byte[] FULL_TOKEN = "ffffffffffffffffffffffffffffffff".getBytes();
private static final byte[] ZERO_TOKEN = "00000000000000000000000000000000".getBytes();
```

### Token-Bezug: `get_token(String ip, MiioLocalRpcResponse cb, int timeout)`

```java
udp_send(datagramSocket, InetAddress.getByName(str),
         JNIBridge.hencrypt(new JNIBridge.MiioMsg(-1L, -1, FULL_TOKEN, null)));
...
JNIBridge.MiioMsg miioMsgHdecrypt = JNIBridge.hdecrypt(bArr);
if (new String(FULL_TOKEN).equals(new String(miioMsgHdecrypt.token).toLowerCase())) {
    ... // Verwerfen: Antwort war noch der Handshake-Echo, kein echtes Geräte-Token
}
MiioLocalRpcResult r = new MiioLocalRpcResult(MiioLocalErrorCode.SUCCESS,
    "{\"id\":1,\"result\":{\"token\":\"" + new String(miioMsgHdecrypt.token).toLowerCase()
    + "\", \"did\":\"" + miioMsgHdecrypt.did + "\"}}", ...);
```

**Bootstrap-Mechanismus:** Im AP-Modus ist dem Gerät noch kein Token bekannt. Die App sendet einen
*Handshake* (`hencrypt`) mit dem konstanten `FULL_TOKEN` (`ff…ff`) und `did=-1` an UDP 54321.
Das Gerät antwortet mit seinem **echten 32-Hex-Token** und seiner `did`. Ein Antwort-Token, das
exakt `ff…ff` entspricht, wird als ungültig verworfen (Race-Schutz). Das Ergebnis wird als
miot-kompatibles JSON `{"id":1,"result":{"token":"…","did":…}}` verpackt.

### Token-Nutzung: `async_rpc(String ip, String json, long did, String token, cb)`

```java
byte[] bArrEncrypt = JNIBridge.encrypt(new JNIBridge.MiioMsg(j2, i2, bytes, str2.getBytes(CHARSET)));
...
JNIBridge.MiioMsg miioMsgDecrypt = JNIBridge.decrypt(bArr, bytes);
aPIString = new MiioLocalRpcResult(SUCCESS, new String(miioMsgDecrypt.message),
        miioMsgDecrypt.did, miioMsgDecrypt.stamp,
        new String(miioMsgDecrypt.token).toLowerCase(), str4).toAPIString();
```

* `MiioMsg(did, stamp, message, token)` → `stamp` ist ein Sekunden-Timestamp,
  `message` = JSON-RPC-Body, `token` = 32-Hex-Gerätetoken.
* Krypto (AES-128-CBC, Key = MD5(token), IV = MD5(key+token), Token als ASCII) liegt **nativ** in
  `libmiio.so` (`JNIBridge.encrypt/decrypt/hencrypt/hdecrypt`, Klassen `JNIBridge` + `MiioMsg`).
  Das ist derselbe miIO-Datagram-Aufbau wie bei `python-miio`/`miIO`-Protokoll.
* Zusätzlich existieren Multicast-/SmartConfig-Pfade über `JNIBridge.smencrypt(...)` und
  `JNIBridge.smencryptpk(bytes, did, tokenBytes)` — der klassische SmartConfig-Broadcast
  (Paketgröße wird auf 254 Byte geprüft, danach `mc_loop(...)`).

### Token-Speicherung

Der Token wird nach erfolgreichem `get_token` im `mDeviceFinder` registriert
(`mDeviceFinder.addToken(String.valueOf(did), token)`) und in `ApConfigStep.mToken` gehalten.
Damit ist der Token ab Provisioning-Zeitpunkt lokal für alle folgenden LAN-RPCs verfügbar.

**Konsequenz für eigene Tools:** Wer im AP-Modus (Gerät strahlt eigenes WLAN aus) provisionieren will,
braucht **kein** Cloud-Login-Token — nur `uid` + `bind_key` (Cloud) für `config_router`, der
Gerätetoken kommt unverschlüsselt-gebootstrapped per UDP 54321.

---

## 3. LAN-Control aktivieren

### 3.1 Es gibt kein `set_ps` / `set_developer_mode`

Die Volltextsuche über alle DEX-/Source-Ebenen findet **keine** Methode `set_ps` und keine
`set_developer_mode`-RPC. LAN-Control wird ausschließlich über die **MIoT-Action `sendCmd`**
mit `CmdKey = "cfg_lan_ctrl"` geschaltet. `cfg_lan_ctrl` liegt je Gerätespezifikation als
Enum-Konstante in der Service-Klasse (z. B. `Ceiling19Service.z0.cfg_lan_ctrl`,
analog `cfg_save_state`, `cfg_bg_proact`, `miband_sleep`).

### 3.2 Abstrakte Basis + konkrete Implementierung

Basis: `com.yeelight.yeelib.c.h` (JADX-deobf: `com.yeelight.yeelib.p152c.AbstractC4649h`)

```java
public abstract boolean mo12924M1(boolean z);   // Originalname: M1(boolean)
```

Implementierung am Beispiel Deckenleuchte: `com.yeelight.yeelib.c.m.h`
(JADX-deobf: `com.yeelight.yeelib.p152c.p158m.C4752e1`-Familie), Zeile 1274:

```java
@Override // com.yeelight.yeelib.c.h
public boolean M1(boolean z2) {
    String str = "setGeekMode --> Invoke, enable : " + z2;
    try {
        F1().sendCmd(Ceiling19Service.z0.cfg_lan_ctrl,
                     z2 ? TimerCodec.ENABLE : TimerCodec.DISENABLE, new x(z2));
        return true;
    } catch (MiotException e2) {
        e2.printStackTrace();
        return false;
    }
}
```

`F1()` liefert die Service-Instanz des Geräts:

```java
public Ceiling19Service F1() {
    AbstractDevice abstractDevice = this.n;
    if (abstractDevice != null && (abstractDevice instanceof YeelightCeiling19Device)) {
        return ((YeelightCeiling19Device) abstractDevice).mDeviceService;
    }
    return null;
}
```

**Der interne Name von LAN-Control in der Yeelight-App ist `setGeekMode`.**

### 3.3 RPC-Transport: `sendCmd` → MIoT-Action

`com.yeelight.yeelib.device.xiaomi.Ceiling19Service`, Zeile 2598:

```java
public void sendCmd(EnumC5238z0 enumC5238z0, String str, CompletionHandler completionHandler) throws MiotException {
    if (!this.mDevice.isConnectionEstablished()) {
        throw new MiotException("device not configurated connection");
    }
    ActionInfo actionInfoCreate = ActionInfoFactory.create(getService(), "sendCmd");
    if (actionInfoCreate == null) { throw new MiotException("actionInfo is null"); }
    if (!actionInfoCreate.setArgumentValue("CmdKey", enumC5238z0.toString())) {
        throw new MiotException("invalid value");
    }
    if (!actionInfoCreate.setArgumentValue("CmdValue", str)) {
        throw new MiotException("invalid value");
    }
    MiotManager.getDeviceManipulator().invoke(actionInfoCreate, new C5166b0(this, completionHandler));
}
```

Auf der Leitung entspricht das einem miIO-RPC der Form
`{"id":<n>,"method":"set_<property>","params":[<CmdKey>, <CmdValue>]}` bzw. dem generischen
miot-`action`-Aufruf `sendCmd` mit `siid/aiid` und Argumenten `CmdKey`/`CmdValue`.
Werte: `TimerCodec.ENABLE = "1"`, `TimerCodec.DISENABLE = "0"`
(`com.miot.service.manager.timer.TimerCodec`, Zeilen 23–24).

### 3.4 UI-Einstiegspunkte

**`com.yeelight.yeelib.ui.activity.LanControlListActivity`** — Liste aller Geräte, Toggle pro Zeile:

```java
dVar.f15604d.setImageResource(hVar.d0().S() ? R$drawable.icon_yeelight_lan_switch_on
                                            : R$drawable.icon_yeelight_lan_switch_off);
dVar.f15604d.setOnClickListener(new View.OnClickListener() {
    public final void onClick(View view) {
        com.yeelight.yeelib.c.h hVar2 = hVar;
        hVar2.M1(!hVar2.d0().S());          // schaltet cfg_lan_ctrl auf den invertierten Status
    }
});
```

`d0()` = State-Objekt des Geräts, `S()` = aktueller LAN-Control-Status (aus dem Geräte-State gelesen).

**`com.yeelight.cherry.ui.activity.QuickConnectLanControlActivity`** — Onboarding-Schritt nach dem
Koppeln; LAN-Control ist **standardmäßig aktiv** und wird erst beim Abschluss committet:

```java
private boolean f9174g = true;              // Default: LAN-Control AN

// Toggle-Button
public void onClick(View view) {
    quickConnectLanControlActivity.f9174g = !quickConnectLanControlActivity.f9174g;
    QuickConnectLanControlActivity.this.b0();
}

// "Fertig"-Button
public void onClick(View view) {
    QuickConnectLanControlActivity.this.f9170c.M1(QuickConnectLanControlActivity.this.f9174g);
    Intent intent = new Intent();
    intent.setClass(QuickConnectLanControlActivity.this, MainActivity.class);
    intent.putExtra("com.yeelight.cherry.device_id", QuickConnectLanControlActivity.this.f9170c.G());
    ...
}
```

### 3.5 Runtime-Gating: wann LAN statt Cloud benutzt wird

`com.miot.service.common.manager.DeviceManipulator` hält das Flag `mEnableLanCtrl`
(Setzer: `enableLanCtrl(boolean)`; durchgereicht in `DeviceManipulatorImpl` →
`ServiceManager.getInstance().getDeviceManipulator().enableLanCtrl(z)`).

`com.miot.service.manipulator.channel.ChannelHelper`:

```java
public static Channel getChannel(Context context, InvokeInfo invokeInfo) {
    WanChannel wanChannel = sWanChannel;
    if (!ServiceManager.getInstance().getDeviceManipulator().isLanCtrlEnabled()) {
        return wanChannel;
    }
    WifiManager wifiManager = (WifiManager) context.getSystemService("wifi");
    if (wifiManager.getConnectionInfo() == null || wifiManager.getConnectionInfo().getBSSID() == null) {
        return wanChannel;
    }
    ConnectionUtils.convertToSSID(wifiManager.getConnectionInfo().getSSID());
    String upperCase = wifiManager.getConnectionInfo().getBSSID().toUpperCase();
    ConnectionInfo connectionInfo = invokeInfo.getConnectionInfo();
    return (upperCase.equalsIgnoreCase(connectionInfo.getBssid())
            && connectionInfo.getLocation() == Location.LOCAL) ? sLanChannel : wanChannel;
}
```

**Drei Bedingungen für lokalen Kanal:** (1) `isLanCtrlEnabled()`, (2) aktuelles WLAN-BSSID ==
`ConnectionInfo.bssid` des Geräts, (3) `ConnectionInfo.location == LOCAL`. Sonst immer Cloud (`WanChannel`).

---

## 4. Kurzfassung für Implementierer

| Frage | Antwort (belegt) |
|---|---|
| Wie provisionieren? | JSON-RPC `miIO.config_router` per **UDP 54321** an `getDhcpInfo().gateway` (AP-Modus), verschlüsselt mit dem Geräte-Token |
| Woher Token im AP-Modus? | `MiioLocalAPI.get_token`: Handshake mit `FULL_TOKEN = ff…ff`, Gerät antwortet mit Token + `did` |
| Welche Cloud-Felder zwingend? | `uid` + `bind_key` (aus `MiotCloudApi.getBindKey`); `country_domain` nur außerhalb CN |
| Wie LAN-Control an? | MIoT-Action `sendCmd` mit `CmdKey="cfg_lan_ctrl"`, `CmdValue="1"` (bzw. `"0"` aus) |
| Gibt es `set_ps`? | Nein — im APK nicht vorhanden |
| Wie erkennt die App den LAN-Status? | Geräte-State-Getter `S()`; UI-Toggle invertiert ihn und schreibt zurück |

**Einschränkung:** Krypto-Details (AES-Modus, Padding) sind nativ in `libmiio.so` implementiert und
wurden hier nicht disassembliert; die Java-Ebene belegt nur die Aufrufstruktur `JNIBridge.encrypt(MiioMsg)`.
