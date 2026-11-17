# const-String-Diff 3.2.34 vs 3.5.4

- 3.2.34: 20066 Strings
- 3.5.4: 22416 Strings
- nur in 3.2.34: 6086
- nur in 3.5.4: 8436

## miIO-/Local-API-Methoden

### nur in 3.2.34 (0)
```
(keine)
```

### nur in 3.5.4 (0)
```
(keine)
```

## Provisioning/SmartConfig

### nur in 3.2.34 (1)
```
SmartConfig
```

### nur in 3.5.4 (3)
```
 bind_key = %s
BleComboStep.getBindKey code: 
BleComboStep.getBindKey: 
```

## LAN-Control / GeekMode

### nur in 3.2.34 (3)
```
GeekModeFeature, destroyFeatureView!
setGeekMode --> onFailed
setGeekMode --> onSucceed
```

### nur in 3.5.4 (0)
```
(keine)
```

## OTA/Firmware

### nur in 3.2.34 (242)
```
 PERF COUNTER: frameTotal: 
 event upload result 
 from remote package: no loader available.
 records were packed and to be uploaded
 start event upload timeDiff 
 start perf upload timeDiff 
############# OAD upgrade, complete! 
'. Resource will not be loaded
ANNOTATION
ASSEMBLE_PUSH : send token upload
An unexpected error occurred while attempting to upload crash reports.
BROADCAST_PACKAGE_REMOVED
BROADCAST_SMS
BROADCAST_STICKY
BROADCAST_WAP_PUSH
BluetoothDeviceBase, upgradeFirmware!
Camera's rotation cannot be changed, it is controller by the ARCore camera pose.
Cannot call setLocalRotation on AnchorNode while it is anchored.
Cannot call setWorldRotation on AnchorNode while it is anchored.
Cherry device, trying to upgrade 2541 now!
Cherry device, trying to upgrade now!
CherryDevice, startOad!
Compensating rotation for front camera
Context class loader is null. Must be running in test mode. Skip patching.
Could not load application package info
Could not load application package metadata
Could not load application package metadata.
Could not load creator class.
Crashlytics Report Uploader
Crashlytics report upload 
DOWNLOAD_MANAGER
Download error:
Download manager has been disabled, return now!
Download manager not found!
Error loading libarsceneview_jni.so
Error to upload the exception 
Exception loading
Failed to create class loader for remote package\n 
Failed to force reload of settings from Crashlytics.
Failed to load icon
```

### nur in 3.5.4 (230)
```
 encodedPayload
 loadBatchSize
 perf begin upload
" ClassLoader: 
%s.BlazeGenerated%sLoader
, base type classloader: 
, encodedPayload=
, loadBatchSize=
, mRotation=
, ndkPayload=
, payload=
, totalRam=
, totalStep1: 
, totalStep2: 
ALTER TABLE events ADD COLUMN payload_encoding TEXT
ANNOTATION_CLASS
ASSEMBLE_PUSH : send token upload, check:
AppMeasurementJobService processed last upload request.
Automatic data collection is enabled. Allowing upload.
CHARACTERISTIC_FIRMWARE
CREATE TABLE global_log_event_state (last_metrics_upload_ms BIGINT PRIMARY KEY)
Cannot load adaptive icon from uri: 
Could not load report file 
DCJobUploadRepeatedInterval
DROP TABLE IF EXISTS event_payloads
Database error while trying to delete uploaded bundles
Disable upload, time
DynamiteLoaderV2 was not cached.
EES not loaded for
EES program loaded for appId, activities
EncodedPayload{encoding=
Error loading bundle from SharedPreferences. Values will be lost
EventUploadNewSwitch
Event{type: %s, payload: %s}
Failed to create IDynamiteLoader.
Failed to determine which loading route to use.
Failed to get thread context classloader 
Failed to instantiate dynamite loader
Failed to load EES program. appId
Failed to load IDynamiteLoader from GmsCore: 
```

## Tokens/Handshake

### nur in 3.2.34 (8)
```
, betaDeviceToken=
ChannelImpl{, token='
FONT_TOKEN
activatorToken=
err.cookie_name_is_token
fcm_push_token
https://cloud-us.yeelight.com/mi_oauth/get_token_by_code
https://cloud-us.yeelight.com/mi_oauth/refresh_token
```

### nur in 3.5.4 (17)
```
, authToken=
, refreshToken=
ALTER TABLE apps ADD COLUMN session_stitching_token TEXT;
REFRESH_TOKEN_BASE_URL
REQUEST_STANDARD_AUTH_TOKEN_IS_EMPTY
TokenResult{token=
UPLOAD_FTOS_TOKEN
async_get_token response:
async_get_token start
fcm_push_token_v2
ftos_push_token
last_check_token
measurement.collection.enable_session_stitching_token.client.dev
measurement.collection.enable_session_stitching_token.service
mi_oauth/refresh_token
refresh_token
session_stitching_token
```

## Cloud-Endpunkte

### nur in 3.2.34 (73)
```
/Xiaomi/
/android_asset/
/api/v2/realip
/base/profile
/bright_down
/bright_up
/close
/color
/crash
/data
/device_list
/event/cb
/intro-rc-auster.html
/intro-rc-cycle.html
/j_security_check
/login
/logout
/mipush/
/open
/proc/net/arp
/sys/class/net/wlan0/address
/toggle
/wear_exception
http://abtest.mistat.xiaomi.com/experiments
http://ccc.sys.miui.com
http://data.mistat.xiaomi.com/mistats/v2
http://www.sec.co.kr/dlna
https://cloud-bj.yeelight.com/
https://cloud-bj.yeelight.com/api/v2/
https://cloud-bj.yeelight.com/api/v3/
https://cloud-bj.yeelight.com/hw/v2/
https://cloud-bj.yeelight.com/license/
https://cloud-bj.yeelight.com/privacy/
https://cloud-de.yeelight.com/
https://cloud-de.yeelight.com/api/v2/
https://cloud-de.yeelight.com/api/v3/
https://cloud-de.yeelight.com/hw/v2/
https://cloud-de.yeelight.com/license/
https://cloud-de.yeelight.com/privacy/
https://cloud-in.yeelight.com/
```

### nur in 3.5.4 (51)
```
/.vdevdir/
/NULL
/cmdline
/pass/v2/register/encrypt
/proc/
/share/share_request_batch
http://35.167.253.230/
http://page-dev.yeedev.com/
http://page-test.yeedev.com/
http://www.dbunit.org/properties/datatypeFactory
http://www.w3.org/1999/xhtml
https://account.xiaomi.com/helpcenter
https://api-bj-st.yeelight.com/
https://api-bj.yeelight.com/
https://api-bj.yeelight.com/license/
https://api-bj.yeelight.com/privacy/
https://api-de-st.yeelight.com/
https://api-de.yeelight.com/
https://api-de.yeelight.com/license/
https://api-de.yeelight.com/privacy/
https://api-in-st.yeelight.com/
https://api-in.yeelight.com/
https://api-ru-st.yeelight.com/
https://api-ru.yeelight.com/
https://api-sg-st.yeelight.com/
https://api-sg.yeelight.com/
https://api-sg.yeelight.com/license/
https://api-sg.yeelight.com/privacy/
https://api-us-st.yeelight.com/
https://api-us.yeelight.com/
https://api-us.yeelight.com/license/
https://api-us.yeelight.com/privacy/
https://app-measurement.com/a
https://beian.miit.gov.cn
https://cn.register.xmpush.xiaomi.com
https://madeby.google.com/home-app/
https://page-de-st.yeelight.com/
https://page-de.yeelight.com/
https://page-in-st.yeelight.com/
https://page-in.yeelight.com/
```

## Developer/Debug

### nur in 3.2.34 (2)
```
A fatal developer error has occurred. Check the logs for further information.
Developer error occurred. Please see logs for detailed information
```

### nur in 3.5.4 (1)
```
Using developer consent only; google app id found
```
