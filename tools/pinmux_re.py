#!/usr/bin/env python3
"""Pinmux/UART/GPIO-Extraktion aus dem Yeelight color1_0061 Referenz-ELF.

Loest PC-relative Literal-Loads gegen die MW300-Speicherkarte auf, markiert
jede Code-Stelle die einen Peripherie-Basiszeiger laedt, und listet alle
Aufrufstellen der PINMUX-/GPIO-Accessor-Helper inklusive der konstanten
Pin-/Funktionsargumente (register-Rueckwaerts-Propagation).

Aufruf:
    python3 tools/pinmux_re.py firmware/color1_142_0061/color1_0061.elf

Ausgabe: Markdown auf stdout (deterministisch, keine Zeitstempel).
"""
from __future__ import annotations

import argparse
import sys
from typing import Iterable

from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_MCLASS
from capstone.arm import ARM_OP_IMM, ARM_OP_MEM, ARM_OP_REG
from elftools.elf.elffile import ELFFile

# MW300 Speicherkarte (sdk/vendor/.../drivers/mw300/mw300.h)
BASES = {
    0x46040000: "UART0",
    0x460C0000: "UART1",
    0x48020000: "UART2",
    0x46060000: "GPIO",
    0x48010000: "PINMUX",
    0x44000000: "RCU",
    0x46010000: "CSU",
    0x44010000: "TIMERS",
    0x4B000000: "APMU",
    0x40000000: "CRYPT_ENGINE",
    0x46080000: "SSP0",
    0x46090000: "SSP1",
    0x49000000: "AES",
}

# Accessor-Helper, in Phase 3.2 aus dem Disasm identifiziert.
HELPERS = {
    0x1F036584: "pinmux_fsel(pin, func)",
    0x1F0365DC: "pinmux_cfg(pin, mode)",
    0x1F03669C: "gpio_out(pin, level)",
    0x1F0366C8: "gpio_dir(pin, out)",
    0x1F0366F2: "gpio_read(pin)",
    0x1F036714: "gpio_cfg_table(pin)",
}

UART_REGS = {
    0x00: "RBR/THR", 0x04: "IER", 0x08: "IIR/FCR", 0x0C: "LCR",
    0x10: "MCR", 0x14: "LSR", 0x18: "MSR", 0x1C: "SCR",
    0x24: "USR", 0x28: "TFL", 0x2C: "RFL", 0x30: "SRR", 0x34: "SRTS",
    0x38: "SBCR", 0x3C: "SFE", 0x40: "SRT", 0x44: "STET", 0x48: "HTX",
    0x4C: "DMASA", 0x50: "DLH", 0x54: "DLL", 0x58: "SRR2", 0x5C: "BRR",
    0x60: "UART_CTRL", 0x64: "UART_IIR2", 0x68: "UART_WER", 0x74: "UART_FAR",
}


def load(path: str):
    fh = open(path, "rb")
    elf = ELFFile(fh)
    return fh, elf


def word_map(elf) -> dict[int, int]:
    """addr -> 32-bit word fuer alle belegten, allokierten Bereiche."""
    words: dict[int, int] = {}
    for sec in elf.iter_sections():
        if sec["sh_type"] not in ("SHT_PROGBITS", "SHT_INIT_ARRAY", "SHT_FINI_ARRAY"):
            continue
        flags = sec["sh_flags"]
        if not (flags & 0x2):  # SHF_ALLOC
            continue
        data = sec.data()
        base = sec["sh_addr"]
        for off in range(0, len(data) - 3, 4):
            words[base + off] = int.from_bytes(data[off:off + 4], "little")
    return words


def code_sections(elf) -> Iterable[tuple[int, bytes]]:
    for sec in elf.iter_sections():
        if sec["sh_type"] != "SHT_PROGBITS":
            continue
        if not (sec["sh_flags"] & 0x4):  # SHF_EXECINSTR
            continue
        yield sec["sh_addr"], sec.data()


def disasm(elf):
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_MCLASS)
    md.detail = True
    md.skipdata = True  # lineare Analyse: konstante Dateninseln nicht abbrechen lassen
    for addr, data in code_sections(elf):
        yield from md.disasm(data, addr)


def literal_of(insn, words: dict[int, int]):
    """Fuer `ldr rX, [pc, #imm]` die Zieladresse + Wortwert liefern."""
    if insn.id == 0:  # SKIPDATA-Platzhalter
        return None, None
    for op in insn.operands:
        if op.type != ARM_OP_MEM or op.mem.base == 0:
            continue
        base_reg = insn.reg_name(op.mem.base)
        if base_reg != "pc":
            continue
        target = insn.address + 4 + op.mem.disp
        return target, words.get(target & ~3)
    return None, None


def const_reg(prev: dict[str, int], reg: str):
    return prev.get(reg)


def collect_calls(insns: list, words) -> list[dict]:
    """Alle Helper-Aufrufe mit propagierten konstanten Argumenten."""
    out = []
    for idx, insn in enumerate(insns):
        if insn.id == 0 or insn.mnemonic not in ("bl", "blx", "b.w", "b"):
            continue
        if not insn.operands or insn.operands[0].type != ARM_OP_IMM:
            continue
        target = insn.operands[0].imm & ~1
        if target not in HELPERS:
            continue
        # Rueckwaerts-Propagation: letzte movs/mov-Immediate je Register
        prev: dict[str, int] = {}
        for back in insns[max(0, idx - 10):idx]:
            if back.id == 0:
                continue
            if (len(back.operands) == 2
                    and back.operands[0].type == ARM_OP_REG
                    and back.operands[1].type == ARM_OP_IMM
                    and back.mnemonic in ("movs", "mov", "mov.w", "movw")):
                prev[back.reg_name(back.operands[0].reg)] = back.operands[1].imm
            else:
                for op in back.operands:
                    if op.type == ARM_OP_REG and back.mnemonic not in ("cmp", "tst"):
                        prev.pop(back.reg_name(op.reg), None)
        out.append({
            "addr": insn.address,
            "helper": HELPERS[target],
            "r0": const_reg(prev, "r0"),
            "r1": const_reg(prev, "r1"),
        })
    return out


def window(insns: list, center: int, before: int = 14, after: int = 18) -> str:
    lines = []
    for i, insn in enumerate(insns):
        if insn.address != center:
            continue
        lo = max(0, i - before)
        hi = min(len(insns), i + after)
        for j in range(lo, hi):
            mark = ">" if insns[j].address == center else " "
            lines.append(f"{mark}{insns[j].address:08x}: {insns[j].mnemonic}\t{insns[j].op_str}")
        break
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--full", action="store_true", help="komplette Fenster dumpen")
    args = ap.parse_args()

    fh, elf = load(args.elf)
    words = word_map(elf)
    insns = list(disasm(elf))
    print(f"# Pinmux/UART/GPIO-Analyse: {args.elf.split('/')[-1]}\n")
    print(f"Instruktionen (Thumb, EXEC-Sektionen): {len(insns)}")
    print(f"Literal-Woerter gemappt: {len(words)}\n")

    # 1) Peripherie-Basis-Loads
    hits: dict[str, list] = {}
    for insn in insns:
        target, value = literal_of(insn, words)
        if value in BASES:
            hits.setdefault(BASES[value], []).append((insn.address, value, target))
    print("## Periphere Basiszeiger-Loads\n")
    for name in sorted(hits):
        print(f"### {name} (0x{hits[name][0][1]:08x}) — {len(hits[name])} Load(s)")
        for addr, _, tgt in hits[name]:
            print(f"  - code 0x{addr:08x} (literal @ 0x{tgt:08x})")
        print()

    # 2) Accessor-Aufrufe mit Argumenten
    calls = collect_calls(insns, words)
    print(f"## Accessor-Aufrufe ({len(calls)})\n")
    print("| code | helper | pin (r0) | arg (r1) |")
    print("|---|---|---|---|")
    for c in calls:
        pin = c["r0"] if c["r0"] is not None else "-"
        arg = c["r1"] if c["r1"] is not None else "-"
        print(f"| 0x{c['addr']:08x} | {c['helper']} | {pin} | {arg} |")
    print()

    # 3) UART-Registerzugriffe (Basis + Offset im selben Fenster)
    print("## UART-Registerfenster\n")
    uart_addrs = {a for n in ("UART0", "UART1", "UART2") for a, _, _ in hits.get(n, [])}
    for addr in sorted(uart_addrs):
        print(f"### ab 0x{addr:08x}\n```")
        print(window(insns, addr, before=20, after=40))
        print("```\n")

    return 0


if __name__ == "__main__":
    sys.exit(main())
