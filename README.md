# YeelightRE

Reverse engineering of legacy **Yeelight Color Bulb V2 (`YLDP06YL` / `yeelink.light.color2`)** firmware (Marvell `88MW302` ARM Cortex-M4F) and legacy Android app (`com.yeelight.cherry`), aiming for **100% autonomous, app-free local provisioning & LAN Control (TCP `55443`)** as well as potential custom/patched firmware improvements.

## Repository Layout
- `tools/` — Standalone tools (`yeelight_standalone.py`, Marvell `88MW30x` firmware parser)
- `docs/` — Protocol notes, live device measurements, and reverse-engineering findings
- `analysis/` — Decompilation exports, Ghidra notes, symbol maps, JADX findings
- `firmware/` — Local scratch directory for firmware images/unpacks (binaries ignored in git)
- `apk/` — Local scratch directory for legacy APK analysis (APKs ignored in git)
- `re_sources/` — Reference material (DEFCON 26 Xiaomi Cortex-M paper, chip docs)

## Current Status (2026-11-17)
- **Tested Hardware:** `yeelink.light.color2` (`hw_ver: mw302`, `fw_ver: 2.0.6_0065` / `76`)
- **AP Wi-Fi Provisioning without App:** Working via `tools/yeelight_standalone.py` (`miIO.config_router`).
- **Open Research Questions:**
  1. Why `set_developer_mode` returns `-9999 user ack timeout` in AP mode and how legacy `com.yeelight.cherry` (e.g. `3.3.04` / `3.2.x`) sets `cfg_lan_ctrl` / handles token rotation after `miIO.config_router`.
  2. Exact `miio_client` / `set_ps` / `cfg_lan_ctrl` / OTA signature bypass analysis on `88MW302` (`DEFCON26` showed `88MW30x` OTA images lack cryptographic verification).
