#!/usr/bin/env python3
"""Deterministische Verifikation der dokumentierten color2-Findings.

Prueft die in analysis/color2_142_0027_re.md und docs/05_color2_firmware_re.md
behaupteten Adressen/Strings/Tabellen direkt gegen die entpackten Segmente.
Exit 0 = alle Behauptungen halten, Exit 1 = Abweichung (Doku veraltet/falsch).
"""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UNP = ROOT / "firmware" / "color2_142_0027" / "unpacked"

SEGS = [
    (0x00100000, UNP / "seg1_0x00100000.bin", 12816),
    (0x1F0032D8, UNP / "seg2_0x1f0032d8.bin", 299832),
    (0x20000040, UNP / "seg3_0x20000040.bin", 4972),
]

DATA = {}
for base, path, size in SEGS:
    d = path.read_bytes()
    assert len(d) == size, f"{path.name}: {len(d)} != {size}"
    DATA[base] = d

FAILS = []


def check(name, cond, detail=""):
    print(("PASS  " if cond else "FAIL  ") + name + (f"  [{detail}]" if detail and not cond else ""))
    if not cond:
        FAILS.append(name)


def rd(addr, n):
    for base, d in DATA.items():
        if base <= addr < base + len(d):
            return d[addr - base:addr - base + n]
    raise KeyError(hex(addr))


def u32(addr):
    return struct.unpack("<I", rd(addr, 4))[0]


def cstr(addr, lim=64):
    return rd(addr, lim).split(b"\x00")[0].decode("ascii", "replace")


# --- 0. Segment-Layout / Reset-Vektor -------------------------------------
check("seg1 groesse 12816 (bis 0x00103210)", 0x00100000 + len(DATA[0x00100000]) == 0x00103210)
check("seg2 groesse 299832 (bis 0x1f04c610)", 0x1F0032D8 + len(DATA[0x1F0032D8]) == 0x1F04C610)
check("seg3 groesse 4972 (bis 0x200013ac)", 0x20000040 + len(DATA[0x20000040]) == 0x200013AC)
check("initial SP == 0x20020000", u32(0x00100000) == 0x20020000)
check("reset vector == 0x1f0032d9 (Thumb, in seg2)", u32(0x00100004) == 0x1F0032D9)

# --- 1. Dispatcher-Pools --------------------------------------------------
check("pool 0x1f00ff90 -> whitelist 0x200009e0", u32(0x1F00FF90) == 0x200009E0)
check('pool 0x1f00ff94 -> "%s_JsDlgt"', cstr(u32(0x1F00FF94)) == "%s_JsDlgt")
check("pool 0x1f00ff98 -> 0xf8 (31 Eintraege)", u32(0x1F00FF98) == 0xF8)
check("pool 0x1f00ff9c -> LAN-Tabelle 0x00103118", u32(0x1F00FF9C) == 0x00103118)
check("pool 0x1f00ffa0 -> Antwort-Callback 0x1f010629", u32(0x1F00FFA0) == 0x1F010629)

# --- 2. LAN-Whitelist ----------------------------------------------------
WL_EXPECTED = [
    "get_prop", "set_default", "set_power", "toggle", "set_bright", "start_cf",
    "stop_cf", "set_scene", "cron_add", "cron_get", "cron_del", "set_ct_abx",
    "set_rgb", "set_hsv", "set_adjust", "adjust_bright", "adjust_ct",
    "adjust_color", "set_music", "set_name",
]
wl_base = u32(0x1F00FF90)
got = []
for i in range(64):
    p = u32(wl_base + 4 * i)
    if p == 0:
        break
    got.append(cstr(p))
check("whitelist hat 20 Eintraege + NULL-Terminator", got == WL_EXPECTED, f"{len(got)} Eintraege")
check("whitelist enthaelt KEIN set_ps (Kernbefund)", "set_ps" not in got)
check("whitelist enthaelt KEIN get_ps/config_router/ota",
      not {"get_ps", "config_router", "ylk_config_router", "ota"} & set(got))

# --- 3. RPC-Tabellen ----------------------------------------------------
lan_tab, lan_n = u32(0x1F00FF9C), u32(0x1F00FF98) >> 3
lan = [(u32(lan_tab + 8 * i), cstr(u32(lan_tab + 8 * i + 4))) for i in range(lan_n)]
check("LAN-Tabelle: 31 Eintraege", len(lan) == 31)
check("LAN-Tabelle: alle Namen enden auf _JsDlgt", all(n.endswith("_JsDlgt") for _, n in lan))
check("LAN-Tabelle enthaelt set_ps_JsDlgt (Idx 10) @0x1f00d92d",
      lan[10] == (0x1F00D92D, "set_ps_JsDlgt"), str(lan[10]))
check("LAN-Tabelle enthaelt get_prop_JsDlgt (Idx 30)", lan[30][1] == "get_prop_JsDlgt")
full = [(u32(0x00103098 + 8 * i), cstr(u32(0x00103098 + 8 * i + 4))) for i in range(47)]
check("Voll-Tabelle: 47 Eintraege", len(full) == 47)
check("LAN-Tabelle == Voll-Tabelle[16:]", full[16:] == lan)
check("Voll-Tabelle Idx 11 == config_router_JsDlgt", full[11][1] == "config_router_JsDlgt")
check("Voll-Tabelle Idx 21 == ylk_config_router_JsDlgt", full[21][1] == "ylk_config_router_JsDlgt")
check("Voll-Tabelle Idx 26 == set_ps_JsDlgt", full[26][1] == "set_ps_JsDlgt")

# --- 4. Fehler-Strings (Code 1..5 in Adressreihenfolge) -------------------
errors = [(0x1F043F98, "invalid command"), (0x1F043FA8, "method not supported"),
          (0x1F043FBD, "invalid parameter"), (0x1F043FCF, "out of memory"),
          (0x1F043FDD, "client quota exceeded")]
for addr, exp in errors:
    check(f"error-string @0x{addr:08x} == {exp!r}", cstr(addr, 32) == exp)
check('Antwortformat @0x1f043f88', cstr(0x1F043F88, 40) == '{"id":%ld, %s\r\n', repr(cstr(0x1F043F88, 40)))
check('Fehler-Wrapper @0x1f043f40', cstr(0x1F043F40, 48) == 'r":{"code":-1, "message":"%s"}}\r\n')
check("lan_ctrl-Taskname @0x1f043f7a", cstr(0x1F043F7A, 16) == "lan_ctrl_task")

# --- 5. set_ps-Properties (FUN_1f00e650) --------------------------------
props = [(0x1F00E778, "cfg_save_state"), (0x1F00E78C, "cfg_lan_ctrl"),
         (0x1F00E794, "cfg_alert_ctrl"), (0x1F00E7A0, "cfg_init_power"),
         (0x1F00E790, "lan_ctrl"), (0x1F00E788, "power"), (0x1F00E784, "save_state"),
         (0x1F00E7A4, "init_power_opt"), (0x1F00E79C, "alert_ctrl")]
for lit, exp in props:
    check(f"set_ps-Literal 0x{lit:08x} -> {exp!r}", cstr(u32(lit)) == exp, cstr(u32(lit)))
check("set_ps-Format %d @0x1f04b7e0", cstr(u32(0x1F00E77C)) == "%d")
check("cfg_alert_ctrl-Format %d|%d", cstr(u32(0x1F00E798)) == "%d|%d")

# --- 6. LED/PWM-Anker ---------------------------------------------------
check("led-timer String @0x1f0425a8", cstr(0x1F0425A8, 16) == "led-timer")
check("g_state Zeiger @0x1f00e780 -> 0x20000968", u32(0x1F00E780) == 0x20000968)

print()
if FAILS:
    print(f"RESULT FAIL: {len(FAILS)} Behauptung(en) halten nicht: {FAILS}")
    sys.exit(1)
print("RESULT PASS: alle dokumentierten Adressen/Strings/Tabellen verifiziert.")
sys.exit(0)
