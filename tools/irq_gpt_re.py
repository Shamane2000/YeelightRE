#!/usr/bin/env python3
"""IRQ-/GPT-Ebene des Yeelight-color-Referenz-ELF (Phase 3.3).

Das Referenz-Binary `color1_0061.elf` ist "not stripped", hat aber nur 47
globale App-Symbole (RPC-Handler) -- die Vendor-Treiber aus `libwmsdk.a`
(GPIO/Interrupt/GPT) sind darin **ohne** Symbolnamen.  Dieses Werkzeug holt
sich die Objektdateien aus dem gepinnten SDK-Archiv, entnimmt die exakten
Instruktionsbytes der Ziel-Funktionen und sucht sie im gelinkten ELF wieder
(Byte-Pattern mit maskierten Relocation-Feldern).  Damit sind die
Vendor-Funktionen im ELF *belegt* statt geraten, und ihre Aufrufstellen
lassen sich mit konstanten Argumenten auswerten.

Aufrufe (auf devastator, /home/jarvis/projects/yeelight-re/.venv):

    python tools/irq_gpt_re.py locate
    python tools/irq_gpt_re.py calls --fn gpio_drv_set_cb
    python tools/irq_gpt_re.py vec
    python tools/irq_gpt_re.py at 0x1f0110a0 --before 8 --after 90

Ausgabe: Markdown auf stdout (deterministisch, keine Zeitstempel/Pfade).
"""
from __future__ import annotations

import argparse
import io
import sys
from pathlib import Path

from capstone import Cs, CS_ARCH_ARM, CS_MODE_MCLASS, CS_MODE_THUMB
from capstone.arm import ARM_OP_IMM, ARM_OP_MEM, ARM_OP_REG
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection

REPO = Path(__file__).resolve().parent.parent
LIB = REPO / "sdk/vendor/ez-connect-lite/sdk/libs/libwmsdk.a"
REF_ELF = REPO / "firmware/color1_142_0061/color1_0061.elf"

# Vendor-Objekte, deren Funktionen in Phase 3.3 gebraucht werden.
OBJECTS = ("mw300_gpio.o", "mdev_gpio.o", "mw300_driver.o", "mw300_gpt.o", "mdev_gpt.o")

AR_MAGIC = b"!<arch>\n"


# ---------------------------------------------------------------- ar-Archiv
def ar_members(blob: bytes):
    """GNU-ar-Archiv -> (name, bytes).  Lange Namen via '//'-Tabelle."""
    if not blob.startswith(AR_MAGIC):
        raise ValueError("kein ar-Archiv")
    pos = len(AR_MAGIC)
    longnames = b""
    while pos + 60 <= len(blob):
        hdr = blob[pos:pos + 60]
        raw_name = hdr[0:16].decode("latin-1").rstrip()
        try:
            size = int(hdr[48:58].decode("latin-1").strip())
        except ValueError:
            break
        data = blob[pos + 60:pos + 60 + size]
        pos += 60 + size + (size & 1)
        if raw_name == "//":
            longnames = data
            continue
        if raw_name.startswith("/") and raw_name[1:].isdigit():
            off = int(raw_name[1:])
            end = longnames.find(b"/\n", off)
            name = longnames[off:end].decode("latin-1")
        else:
            name = raw_name.rstrip("/")
        yield name, data


# ------------------------------------------------------- Muster-Extraktion
def _reloc_offsets(elf: ELFFile, shndx: int, base: int, size: int) -> set[int]:
    """Byte-Offsets (relativ zur Funktion), die eine Relocation tragen."""
    offs: set[int] = set()
    for sec in elf.iter_sections():
        if not isinstance(sec, RelocationSection):
            continue
        if sec["sh_info"] != shndx:
            continue
        for rel in sec.iter_relocations():
            o = rel["r_offset"] - base
            if 0 <= o < size:
                offs.update(range(o, min(o + 4, size)))
    return offs


def func_template(obj: bytes, name: str):
    """(instruktionsbytes, maske) einer Funktion aus einer Objektdatei."""
    elf = ELFFile(io.BytesIO(obj))
    symtab = elf.get_section_by_name(".symtab")
    if symtab is None:
        return None
    sym = None
    for s in symtab.iter_symbols():
        if s.name == name and s["st_info"]["type"] == "STT_FUNC" \
                and s["st_shndx"] != "SHN_UNDEF" and s["st_size"]:
            sym = s
            break
    if sym is None:
        return None
    shndx = sym["st_shndx"]
    sec = elf.get_section(shndx)
    # ARM-EABI setzt bei STT_FUNC in Thumb-Objekten das Bit0 in st_value --
    # ohne Maskierung wuerde das Muster um ein Byte verschoben starten.
    base = sym["st_value"] & ~1
    size = sym["st_size"]
    off = base - (sec["sh_addr"] or 0)
    data = sec.data()[off:off + size]
    dead = _reloc_offsets(elf, shndx, base, size)
    return data, dead, sym["st_info"]["bind"]


def locate(elf: ELFFile, tmpl: bytes, dead: set[int]):
    """Alle Fundstellen des Musters in den EXEC-Sektionen des ELF."""
    if not tmpl:
        return []
    # Anker: erster 4-Byte-Block ohne Relocation; er ist im gelinkten Image
    # unveraendert (nur PC-relative Felder werden beim Linken gepatcht).
    anchor_off = 0
    for i in range(0, len(tmpl) - 3, 4):
        if not (set(range(i, i + 4)) & dead):
            anchor_off = i
            break
    anchor = tmpl[anchor_off:anchor_off + 4]
    hits = []
    for sec in elf.iter_sections():
        if sec["sh_type"] != "SHT_PROGBITS" or not (sec["sh_flags"] & 0x4):
            continue
        data = sec.data()
        base = sec["sh_addr"]
        start = 0
        while True:
            idx = data.find(anchor, start)
            if idx < 0:
                break
            start = idx + 1
            cand = idx - anchor_off
            if cand < 0 or cand + len(tmpl) > len(data):
                continue
            ok = True
            for i, b in enumerate(tmpl):
                if i in dead:
                    continue
                if data[cand + i] != b:
                    ok = False
                    break
            if ok:
                hits.append(base + cand)
    return hits


# ------------------------------------------------------------------ Disasm
def md() -> Cs:
    cs = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_MCLASS)
    cs.detail = True
    cs.skipdata = True
    return cs


def code_insns(elf: ELFFile):
    cs = md()
    for sec in elf.iter_sections():
        if sec["sh_type"] != "SHT_PROGBITS" or not (sec["sh_flags"] & 0x4):
            continue
        yield from cs.disasm(sec.data(), sec["sh_addr"])


def word_map(elf: ELFFile) -> dict[int, int]:
    words: dict[int, int] = {}
    for sec in elf.iter_sections():
        if sec["sh_type"] != "SHT_PROGBITS" or not (sec["sh_flags"] & 0x2):
            continue
        data = sec.data()
        base = sec["sh_addr"]
        for off in range(0, len(data) - 3, 4):
            words[base + off] = int.from_bytes(data[off:off + 4], "little")
    return words


def is_data(val: int) -> bool:
    """Sieht der Wert nach einem Code-/Datenzeiger im XIP-Fenster aus?"""
    return 0x1F000001 <= val <= 0x1FFFFFFF


def back_prop(insns: list, idx: int, words: dict[int, int], depth: int = 24):
    """Konstante Registerwerte unmittelbar vor Aufruf `idx` (vereinfacht)."""
    regs: dict[str, int] = {}
    for back in reversed(insns[max(0, idx - depth):idx]):
        if back.id == 0:
            continue
        mn = back.mnemonic
        ops = back.operands
        if mn in ("movs", "mov", "mov.w", "movw") and len(ops) == 2 and \
                ops[0].type == ARM_OP_REG and ops[1].type == ARM_OP_IMM:
            regs[back.reg_name(ops[0].reg)] = ops[1].imm
            continue
        if mn == "ldr" and len(ops) == 2 and ops[0].type == ARM_OP_REG \
                and ops[1].type == ARM_OP_MEM and ops[1].mem.base:
            if back.reg_name(ops[1].mem.base) == "pc":
                tgt = back.address + 4 + ops[1].mem.disp
                if tgt in words:
                    regs[back.reg_name(ops[0].reg)] = words[tgt]
                    continue
        if mn in ("add.w", "adds", "add", "sub.w", "subs", "sub") and len(ops) == 3 \
                and ops[0].type == ARM_OP_REG and ops[1].type == ARM_OP_REG \
                and ops[2].type == ARM_OP_IMM:
            src = regs.get(back.reg_name(ops[1].reg))
            if src is not None:
                v = src + ops[2].imm if mn.startswith("add") else src - ops[2].imm
                regs[back.reg_name(ops[0].reg)] = v
            else:
                regs.pop(back.reg_name(ops[0].reg), None)
            continue
        if mn in ("and.w", "and") and len(ops) == 3 and ops[0].type == ARM_OP_REG \
                and ops[1].type == ARM_OP_REG and ops[2].type == ARM_OP_IMM:
            src = regs.get(back.reg_name(ops[1].reg))
            if src is not None:
                regs[back.reg_name(ops[0].reg)] = src & ops[2].imm
            else:
                regs.pop(back.reg_name(ops[0].reg), None)
            continue
        if mn == "lsls" and len(ops) == 3 and ops[0].type == ARM_OP_REG \
                and ops[1].type == ARM_OP_REG and ops[2].type == ARM_OP_IMM:
            src = regs.get(back.reg_name(ops[1].reg))
            if src is not None:
                regs[back.reg_name(ops[0].reg)] = (src << ops[2].imm) & 0xFFFFFFFF
            else:
                regs.pop(back.reg_name(ops[0].reg), None)
            continue
        # Schreibzugriff auf ein Register ohne bekannte Konstante.
        for op in ops:
            if op.type == ARM_OP_REG and mn not in ("cmp", "tst", "cbz", "cbnz"):
                regs.pop(back.reg_name(op.reg), None)
    return regs


def fmt(v) -> str:
    if v is None:
        return "-"
    if is_data(v):
        return f"0x{v:08x} (code?)"
    return f"0x{v:x}" if v > 9 else str(v)


def call_sites(insns: list, words: dict[int, int], targets: dict[int, str]):
    out = []
    for idx, insn in enumerate(insns):
        if insn.id == 0 or insn.mnemonic not in ("bl", "blx", "b.w", "b"):
            continue
        if not insn.operands or insn.operands[0].type != ARM_OP_IMM:
            continue
        tgt = insn.operands[0].imm & ~1
        if tgt not in targets:
            continue
        out.append({
            "addr": insn.address,
            "target": tgt,
            "name": targets[tgt],
            "regs": back_prop(insns, idx, words),
        })
    return out


# ------------------------------------------------------------------- main
def load_ref():
    fh = open(REF_ELF, "rb")
    return fh, ELFFile(fh)


def resolve(names: list[str]) -> dict[str, tuple[bytes, set[int], str]]:
    """name -> (template, dead_offsets, object).

    Bei Mehrfachdefinition (GPIO_IRQHandler: schwach in mw300_gpio.o, stark in
    mdev_gpio.o) gewinnt die **starke** Definition -- genau die zieht der Linker.
    """
    blob = LIB.read_bytes()
    members = dict(ar_members(blob))
    out: dict[str, tuple[bytes, set[int], str]] = {}
    for obj_name in OBJECTS:
        obj = members.get(obj_name)
        if obj is None:
            continue
        for name in names:
            tpl = func_template(obj, name)
            if tpl is None:
                continue
            data, dead, bind = tpl
            prev = out.get(name)
            if prev is None or (bind != "STB_WEAK" and prev[2] == "weak"):
                out[name] = (data, dead, "weak" if bind == "STB_WEAK" else obj_name)
    return out


def _default_names() -> list[str]:
    return [
        # Interrupt-/GPIO-Ebene
        "GPIO_IntConfig", "GPIO_IntMask", "GPIO_GetIntStatus", "GPIO_IntClr",
        "GPIO_IRQHandler", "install_int_callback",
        "gpio_drv_init", "gpio_drv_open", "gpio_drv_set_cb", "gpio_drv_get_io_domain",
        # GPT (Lampen-PWM)
        "GPT_Init", "GPT_Start", "GPT_Stop", "GPT_ChannelOutputConfig",
        "GPT_ChannelFuncSelect", "GPT_ChannelInputConfig", "GPT_SetChannelMatchVal0",
        "GPT_SetChannelMatchVal1", "GPT_SetChannelBothMatchVal",
        "GPT_SetCounterUppVal", "GPT_IRQHandler", "GPT_IntMask", "GPT_StatusClr",
        "GPT_ChannelReset", "GPT_CounterReset", "GPT_GetCounterVal",
        "gpt_drv_open", "gpt_drv_init", "gpt_drv_pwm", "gpt_drv_set",
        "gpt_drv_start", "gpt_drv_stop", "gpt_drv_close", "gpt_drv_setcb",
    ]


def cmd_locate(args) -> int:
    fh, elf = load_ref()
    names = args.fn or _default_names()
    resolved = resolve(names)
    print("# Vendor-Funktionen im Referenz-ELF (Byte-Pattern-Match)\n")
    print("| Funktion | Objekt | Groesse | ELF-Adresse(n) |")
    print("|---|---|---|---|")
    found: dict[int, str] = {}
    for name in names:
        if name not in resolved:
            print(f"| {name} | ? | ? | **nicht im Archiv** |")
            continue
        tpl, dead, obj = resolved[name]
        hits = locate(elf, tpl, dead)
        for h in hits:
            found.setdefault(h, name)
        addr = ", ".join(f"0x{h:08x}" for h in hits) if hits else "**nicht gefunden**"
        print(f"| {name} | {obj} | {len(tpl)} B | {addr} |")
    print()
    return 0


def cmd_calls(args) -> int:
    fh, elf = load_ref()
    words = word_map(elf)
    names = args.fn or _default_names()
    resolved = resolve(names)
    targets: dict[int, str] = {}
    print("# Aufrufstellen mit konstanten Argumenten\n")
    for name in names:
        if name not in resolved:
            continue
        tpl, dead, obj = resolved[name]
        for h in locate(elf, tpl, dead):
            targets.setdefault(h, name)
    insns = list(code_insns(elf))
    calls = call_sites(insns, words, targets)
    if args.fn:
        calls = [c for c in calls if c["name"] in set(names)]
    print(f"Instruktionen: {len(insns)} | Aufrufe gefunden: {len(calls)}\n")
    for c in calls:
        r = c["regs"]
        print(f"## {c['name']} @ 0x{c['target']:08x}  <-  call 0x{c['addr']:08x}")
        print(f"   r0={fmt(r.get('r0'))}  r1={fmt(r.get('r1'))}  "
              f"r2={fmt(r.get('r2'))}  r3={fmt(r.get('r3'))}")
        if args.window:
            print("```")
            print("\n".join(window(insns, c["addr"], args.before, 2)))
            print("```")
        print()
    return 0


def window(insns: list, center: int, before: int, after: int) -> str:
    for i, insn in enumerate(insns):
        if insn.address != center:
            continue
        lo = max(0, i - before)
        hi = min(len(insns), i + after + 1)
        lines = []
        for j in range(lo, hi):
            mark = ">" if insns[j].address == center else " "
            lines.append(f"{mark}{insns[j].address:08x}: {insns[j].mnemonic}\t{insns[j].op_str}")
        return "\n".join(lines)
    return "(Adresse nicht im Code)"


def cmd_vec(args) -> int:
    fh, elf = load_ref()
    words = word_map(elf)
    # Die erste geladene PROGBITS-Sektion (.seg1 im Referenz-ELF, .init beim
    # eigenen Build) enthaelt die Vektortabelle (siehe docs/07/08).
    init = None
    for sec in elf.iter_sections():
        if sec["sh_type"] == "SHT_PROGBITS" and (sec["sh_flags"] & 0x4):
            init = sec
            break
    base = init["sh_addr"]
    names = {
        0: "initial MSP", 1: "Reset", 2: "NMI", 3: "HardFault", 4: "MemManage",
        5: "BusFault", 6: "UsageFault", 11: "SVCall", 12: "DebugMon",
        14: "PendSV", 15: "SysTick",
    }
    irq = {0: "ExtPin0", 1: "ExtPin1", 2: "RTC", 3: "CRC", 4: "AES", 5: "I2C0",
           6: "I2C1", 8: "DMA", 9: "GPIO", 10: "SSP0", 11: "SSP1", 12: "SSP2",
           13: "QSPI", 14: "GPT0", 15: "GPT1", 16: "GPT2", 17: "GPT3",
           18: "UART0", 19: "UART1", 20: "UART2", 22: "WDT", 24: "ADC0",
           25: "DAC", 26: "ACOMPWKUP", 27: "ACOMP", 28: "SDIO", 29: "USB"}
    print(f"# Vektortabelle @ 0x{base:08x} (color1_0061.elf, .init)\n")
    print("| Slot | Bedeutung | Handler |")
    print("|---|---|---|")
    for slot in range(16 + 48):
        val = words.get(base + 4 * slot)
        if val is None:
            continue
        if slot == 0:
            print(f"| 0 | initial MSP | 0x{val:08x} |")
            continue
        if slot < 16:
            label = names.get(slot, "reserved")
            n = ""
        else:
            n = slot - 16
            label = f"IRQ {n} {irq.get(n, '')}".strip()
        print(f"| {slot} | {label} | 0x{val:08x}{' |' if not n else ''}"[:200])
    print()
    return 0


def cmd_at(args) -> int:
    fh, elf = load_ref()
    insns = list(code_insns(elf))
    print(f"# Disasm @ 0x{args.addr:08x}\n```")
    print(window(insns, args.addr, 0, args.after))
    print("```")
    return 0


def cmd_xref(args) -> int:
    """Aufrufstellen einer beliebigen Adresse inkl. konstanter Argumente."""
    fh, elf = load_ref()
    words = word_map(elf)
    insns = list(code_insns(elf))
    targets = {a: f"0x{a:08x}" for a in args.addr}
    calls = call_sites(insns, words, targets)
    print(f"# Xref-Aufrufe auf {', '.join(f'0x{a:08x}' for a in args.addr)}"
          f" -- {len(calls)} Treffer\n")
    for c in calls:
        r = c["regs"]
        print(f"| 0x{c['addr']:08x} | {c['name']} | "
              f"r0={fmt(r.get('r0'))} r1={fmt(r.get('r1'))} "
              f"r2={fmt(r.get('r2'))} r3={fmt(r.get('r3'))} |")
        if args.window:
            print()
            print("```")
            print(window(insns, c["addr"], args.before, 2))
            print("```")
            print()
    return 0


def cmd_refs(args) -> int:
    """Code-Stellen, die eine absolute Adresse als Literal laden."""
    fh, elf = load_ref()
    words = word_map(elf)
    cs = md()
    print(f"# Referenzen auf 0x{args.addr:08x}\n")
    print("| Fundstelle | Literal-Adresse | Kontext |")
    print("|---|---|---|")
    n = 0
    for sec in elf.iter_sections():
        if sec["sh_type"] != "SHT_PROGBITS" or not (sec["sh_flags"] & 0x4):
            continue
        for insn in cs.disasm(sec.data(), sec["sh_addr"]):
            if insn.id == 0 or insn.mnemonic not in ("ldr", "ldr.w"):
                continue
            ops = insn.operands
            if len(ops) < 2 or ops[1].type != ARM_OP_MEM or not ops[1].mem.base:
                continue
            if insn.reg_name(ops[1].mem.base) != "pc":
                continue
            lit = insn.address + 4 + ops[1].mem.disp
            if words.get(lit) == args.addr:
                print(f"| 0x{insn.address:08x} | 0x{lit:08x} | "
                      f"`{insn.mnemonic} {insn.op_str}` |")
                n += 1
    print(f"\nGesamt: {n}")
    return 0


def cmd_range(args) -> int:
    fh, elf = load_ref()
    insns = [i for i in code_insns(elf) if args.start <= i.address < args.end]
    print(f"# Disasm 0x{args.start:08x} - 0x{args.end:08x}\n```")
    for insn in insns:
        print(f"{insn.address:08x}: {insn.mnemonic}\t{insn.op_str}")
    print("```")
    return 0


def cmd_words(args) -> int:
    """Worte/Bytes aus dem Referenz-ELF lesen (Literalpools, Konfigtabellen)."""
    fh, elf = load_ref()
    words = word_map(elf)
    print(f"# Speicher 0x{args.addr:08x} ({args.count} Worte)\n")
    for i in range(args.count):
        a = args.addr + 4 * i
        v = words.get(a)
        print(f"0x{a:08x}: {v if v is None else f'0x{v:08x}'}  "
              f"{'' if v is None else repr(v.to_bytes(4, 'little'))}")
    return 0


def cmd_table(args) -> int:
    """Konfigtabelle mit fester Stride ausgeben (Bytes/Worte gemischt)."""
    fh, elf = load_ref()
    words = word_map(elf)
    print(f"# Tabelle 0x{args.addr:08x}, {args.count} x {args.stride} B\n")
    hdr = args.layout.split(",")
    print("| # | " + " | ".join(hdr) + " |")
    print("|---|" + "---|" * len(hdr))
    for i in range(args.count):
        base = args.addr + i * args.stride
        cells = []
        off = 0
        for spec in hdr:
            if spec.startswith("u8"):
                cells.append(f"0x{(words.get(base - base % 4) or 0) >> (8 * ((base + off) % 4)) & 0xFF:02x}"
                             if False else str(args_bytes(elf, base + off, 1)[0]))
                off += 1
            else:
                cells.append(f"0x{int.from_bytes(args_bytes(elf, base + off, 4), 'little'):08x}")
                off += 4
        print(f"| {i} | " + " | ".join(cells) + " |")
    return 0


_BYTES: dict = {}


def args_bytes(elf: ELFFile, addr: int, n: int) -> bytes:
    if not _BYTES:
        for sec in elf.iter_sections():
            if sec["sh_type"] != "SHT_PROGBITS":
                continue
            base = sec["sh_addr"]
            _BYTES[base] = (base + len(sec.data()), sec.data())
    for base, (end, data) in sorted(_BYTES.items()):
        if base <= addr <= end - n:
            return data[addr - base:addr - base + n]
    return b"\x00" * n


def cmd_hexdump(args) -> int:
    fh, elf = load_ref()
    data = args_bytes(elf, args.addr, args.count)
    print(f"# Hexdump 0x{args.addr:08x} ({args.count} B)\n```")
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        hexs = " ".join(f"{b:02x}" for b in chunk)
        text = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        print(f"0x{args.addr + i:08x}  {hexs:<47}  {text}")
    print("```")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("locate")
    p.add_argument("--fn", nargs="*")
    p.set_defaults(func=cmd_locate)
    p = sub.add_parser("calls")
    p.add_argument("--fn", nargs="*")
    p.add_argument("--window", action="store_true")
    p.add_argument("--before", type=int, default=10)
    p.set_defaults(func=cmd_calls)
    p = sub.add_parser("vec")
    p.set_defaults(func=cmd_vec)
    p = sub.add_parser("at")
    p.add_argument("addr", type=lambda s: int(s, 0))
    p.add_argument("--before", type=int, default=0)
    p.add_argument("--after", type=int, default=60)
    p.set_defaults(func=cmd_at)
    p = sub.add_parser("xref")
    p.add_argument("addr", nargs="+", type=lambda s: int(s, 0))
    p.add_argument("--window", action="store_true")
    p.add_argument("--before", type=int, default=12)
    p.set_defaults(func=cmd_xref)
    p = sub.add_parser("refs")
    p.add_argument("addr", type=lambda s: int(s, 0))
    p.set_defaults(func=cmd_refs)
    p = sub.add_parser("range")
    p.add_argument("start", type=lambda s: int(s, 0))
    p.add_argument("end", type=lambda s: int(s, 0))
    p.set_defaults(func=cmd_range)
    p = sub.add_parser("words")
    p.add_argument("addr", type=lambda s: int(s, 0))
    p.add_argument("--count", type=lambda s: int(s, 0), default=16)
    p.set_defaults(func=cmd_words)
    p = sub.add_parser("table")
    p.add_argument("addr", type=lambda s: int(s, 0))
    p.add_argument("--count", type=lambda s: int(s, 0), default=8)
    p.add_argument("--stride", type=int, default=0x18)
    p.add_argument("--layout", default="u8,u8,u8,u8,ptr,ptr,ptr")
    p.set_defaults(func=cmd_table)
    p = sub.add_parser("hexdump")
    p.add_argument("addr", type=lambda s: int(s, 0))
    p.add_argument("--count", type=lambda s: int(s, 0), default=64)
    p.set_defaults(func=cmd_hexdump)
    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
