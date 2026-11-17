# Live Lab Findings (2026-11-17)

## Active Devices Observed in Home Assistant
- `SZ_Lampe_Decke_01` (`192.168.1.166`, MAC `04:cf:8c:90:94:b9`, `did=07dd258b`, `model=color`, `fw_ver=65` / `2.0.6_0065`, HomeKit `_hap._tcp` `YeelightColorBulb-94B9`) — LAN Control active (TCP `55443`, SSDP `1982`).
- `Bad Deckenlicht` (`192.168.1.168`, MAC `7c:49:eb:ac:ca:ff`, `did=05e0dd1d`, `model=color`, `fw_ver=65`) — LAN Control active.
- `Yeelight Color 0x371f839` (`192.168.1.176`, `did=0371f839`, `model=color`, `fw_ver=76`) — LAN Control active; miIO hello returns `token=0000...0000`.
- Test Bulb (`did=05eb8034`, MAC `7C:49:EB:B7:6E:16`, `yeelink.light.color2`, `hw_ver=mw302`, `fw_ver=2.0.6_0065`, `wifi_fw_ver=SD878x-14.76.36.p103-702.1.0-WM`):
  - **AP Mode (`192.168.10.1`):**
    - `miIO hello` returns cleartext 16-byte token.
    - `miIO.info` succeeds.
    - `set_developer_mode [1]` returns `{"error": {"code": -9999, "message": "user ack timeout"}}`.
    - `miIO.config_router` with `{"ssid": "...", "passwd": "...", "uid": 0}` returns `{"result": ["ok"]}` and connects bulb to home Wi-Fi (`192.168.1.115`).
  - **Station Mode (`192.168.1.115`):**
    - At `stamp=9` after boot, `miIO hello` already returns `ffff...ffff`.
    - Previously captured AP token times out on `miIO.info`, `set_developer_mode`, `set_ps`, `get_prop`.
    - TCP `55443` returns `Errno 111 Connection refused` (LAN Control disabled after factory reset).
    - mDNS `_hap._tcp` advertises `YeelightColorBulb-6E16` (`md=YLDP06YL`, `sf=1`).
