# Marvell 88MW30x Architecture Notes (DEFCON 26 Reference)

Extracted from Dennis Giese ("How-to modify ARM Cortex-M based firmware: A step-by-step approach for Xiaomi IoT Devices", DEFCON 26 IoT Village):

- **Applies explicitly to:** `yeelink.light.color2` (`YLDP06YL`), `yeelink.light.ct2`, `yeelink.light.mono1`, `yeelink.light.color1`, `lumi.gateway.v3`.
- **SoC:** Marvell `88MW30x` (ARM Cortex-M4F @ 200 MHz, 512 KB SRAM, 4 MB external SPI Flash on bulbs, OTP for Device ID + Cloud Key).
- **Memory Map (XIP + RAM):**
  - Code RAM: `0x00100000`
  - Flash XIP: `0x1F000000` (appfw usually starts around `0x1F003718` / `0x1F003768`)
  - SRAM Data: `0x20000040`
- **Keys:**
  - `Cloud Key` (16 byte): static in OTP, used for `ot.io.mi.com:8053` / `ott.io.mi.com:80`.
  - `App Token` (16 byte): dynamic, regenerated on Wi-Fi provisioning / cloud sync.
- **Security Posture (from talk):**
  - SWD enabled by default on PCB test points; UART CLI supports `psm-get`, `psm-set`, `psm-dump`, `updatefw <http_url>`.
  - Firmware OTA updates (`miIO.ota`) are **not cryptographically signed** and lack MD5/cert verification on `88MW30x` (custom/patched binaries can be rebuilt with `mrvl-88mw30x-firmware-tools` / `axf2firmware`).
