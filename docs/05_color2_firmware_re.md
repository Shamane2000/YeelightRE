# 05 — Firmware-RE `yeelink.light.color2` v1.4.2_0027 (88MW302)

Statische Analyse in Ghidra 12.1.4 (ARM Cortex-M4, Thumb-2, LE).
**Detaildokument:** [`analysis/color2_142_0027_re.md`](../analysis/color2_142_0027_re.md)
**Artefakte:** `tools/color2_make_elf.py` (ELF32 aus den 3 Segmenten), `tools/color2_xref.py`
(Literal-Pool-Referenzen), `analysis/ExportAll.java` (Decompiler-Export → `ALL.c`).

## TL;DR

| Frage | Antwort |
| :--- | :--- |
| Ist `set_ps`/`cfg_lan_ctrl` über LAN erreichbar? | **Nein.** LAN-Whitelist (`0x200009e0`, 20 Methoden) enthält `set_ps` nicht → `{"code":-1,"message":"method not supported"}` |
| Gibt es eine AP-Modus-Sonderregel? | **Nein.** Die Whitelist wird in `FUN_1f00fec4` **unbedingt** geprüft; kein Provisioning-/AP-Flag im LAN-Pfad |
| Wie wird LAN-Control aktiviert? | Nur über den Cloud/App-Kanal (`set_ps {"cfg_lan_ctrl":1}`) oder persistiertes `state+0x190=1`, das beim Netz-Init via `FUN_1f00aa3c` → `FUN_1f01069e()` den Listener startet |
| Was macht `set_ps` genau? | 4 Properties: `cfg_save_state` (`+0x188`), `cfg_lan_ctrl` (`+0x190`), `cfg_alert_ctrl` (`+0x1d4` Bitmaske), `cfg_init_power` (`+0x1e0`) |
| LED/PWM | Apply-Kette `FUN_1f0110a0` → Modus 1/2/3 (`FUN_1f010f20`/`_c6c`/`_cf8`) → Treiber-Op `+0x44`; `led-timer` 200 ms |

## Adresslandkarte

| Symbol | Adresse | Rolle |
| :--- | :--- | :--- |
| Voll-RPC-Tabelle (47) | `0x00103098` | Cloud/Grid-Dispatch |
| LAN-RPC-Tabelle (31) | `0x00103118` | = Voll-Tabelle ab Idx. 16 |
| LAN-Whitelist (20, NULL-term.) | `0x200009e0` | Zugriffsfilter, **ohne `set_ps`** |
| `"%s_JsDlgt"` | `0x1f043f27` | Handler-Namensschema |
| `FUN_1f00fec4` | `0x1f00fec4` | LAN-Dispatcher: Whitelist → Tabelle → Handler |
| `FUN_1f010094` | `0x1f010094` | LAN-TCP-Request-Parser + Quota |
| `FUN_1f0105f8` | `0x1f0105f8` | Queue-Send an `lan_ctrl_task` |
| `FUN_1f01069e` / `FUN_1f010684` | | LAN-Control **an** / **aus** |
| `FUN_1f00aa3c` | `0x1f00aa3c` | Netz-Init: re-arm aus `state+0x190` |
| `FUN_1f00e650` | `0x1f00e650` | `set_ps`-Handler (4 Properties) |
| `set_ps_JsDlgt` | `0x1f00d92d` | Stub, LAN-Tabelle Idx. 10 |
| `FUN_1f0110a0` | `0x1f0110a0` | zentrale Lampen-Apply-Funktion |
| `FUN_1f010f20` / `_1f010c6c` / `_1f010cf8` | | RGB / CT / HSV → Treiber-Op `+0x44` |
| `led-timer` | `0x1f0425a8` | LED-Update-Timer, 200 ms |

## Fehler-Codes (LAN-Antworten)

`1 invalid command` (`0x1f043f98`) · `2 method not supported` (`0x1f043fa8`) ·
`3 invalid parameter` (`0x1f043fbd`) · `4 out of memory` (`0x1f043fcf`) ·
`5 client quota exceeded` (`0x1f043fdd`).

## Patch-Ansätze

1. **`cfg_lan_ctrl`-Freischaltung per LAN (kleinster Eingriff):** `set_ps` in die Whitelist
   aufnehmen. Die Liste liegt als SRAM-Copy unter `0x200009e0`; im Flash muss der
   Initialisierungs-Block (Quelle der Copy, `.data`-Init in seg1/seg3) um einen 21. Zeiger
   erweitert werden — es muss ein freier NULL-Slot vorhanden sein, sonst kein Inplace-Patch.
2. **Whitelist generell umgehen:** in `FUN_1f00fec4` den Scan überspringen (Rückgabepfad `2`
   entfernen). Folge: alle 31 LAN-Handler werden erreichbar, inkl. `suicide`, `ylk_config_router`
   und `miio_test` — Sicherheitsrisiko, nur für Labor-Geräte.
3. **Dauerhaft offen:** `state+0x190` im Persistenz-/Init-Pfad hart auf 1, dann startet
   `FUN_1f00aa3c` den Listener bei jedem Boot.
4. **LED-Verhalten:** Helligkeits-/Farbberechnung liegt in `FUN_1f010f20`/`_c6c`/`_cf8`
   (12-Byte-Light-State → Treiber-Op `+0x44`); Limits in `FUN_1f00e844`
   (Brightness 1..100, Hue < 360, Duration < 101).

## Verifikations-Stand

* Alle Adressen per Ghidra-Decompilat **und** Byte-Readback der Segmente geprüft
  (Property-Namen, Whitelist-Strings, Tabelleninhalte, Fehler-Strings).
* Kein dynamischer Nachweis (kein Gerät geflasht, kein JTAG/UART-Trace) — reine Statik.
* Offen: Ziel der Treiber-Vtable (`+0x44`) bzw. die PWM-Registerbasis des 88MW302-LED-Blocks;
  statischer Caller des `config_router`-Pfads (UDP-Seite).
