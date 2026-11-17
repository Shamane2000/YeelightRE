#!/usr/bin/env python3
"""Baut aus den entpackten `color2`-Segmenten ein ELF32 (ARM, EABI5).

Zweck: Ghidra 12.1.4 importiert ELF-Symbole inkl. gesetztem Thumb-Bit, d. h.
die Load-Adressen (`0x00100000`, `0x1f0032d8`, `0x20000040`) und der
Thumb-2-Modus der Handler kommen ohne manuelles Kontext-Setzen an.

Als Symbole werden die Eintraege der `*_JsDlgt`-Methodentabelle aus
`seg1` (`{handler_ptr, name_ptr}`-Paare ab `0x00103098`) uebernommen.

Aufruf:
    python3 tools/color2_make_elf.py [unpacked_dir] [out.elf]
"""
import os
import struct
import sys

SEGMENTS = [
    ("seg1", 0x00100000, "seg1_0x00100000.bin", 0x6, 0x5),  # SHF_ALLOC|EXEC / R+X
    ("seg2", 0x1F0032D8, "seg2_0x1f0032d8.bin", 0x6, 0x5),
    ("seg3", 0x20000040, "seg3_0x20000040.bin", 0x3, 0x6),  # SHF_ALLOC|WRITE / R+W
]

TABLE_START = 0x00103098  # erste {handler,name}-Paar der JsDlgt-Tabelle
EHDR_SIZE = 52
PHDR_SIZE = 32
SHDR_SIZE = 40


def cstr(blob, off):
    end = blob.find(b"\x00", off)
    if end < 0:
        return None
    raw = blob[off:end]
    try:
        text = raw.decode("ascii")
    except UnicodeDecodeError:
        return None
    if not text or any(not (32 <= ord(c) < 127) for c in text):
        return None
    return text


def load_segments(unpacked):
    blobs = []
    for name, base, fname, sh_flags, ph_flags in SEGMENTS:
        path = os.path.join(unpacked, fname)
        with open(path, "rb") as fh:
            data = fh.read()
        blobs.append({"name": name, "base": base, "data": data,
                      "sh_flags": sh_flags, "ph_flags": ph_flags, "file": fname})
    return blobs


def read_symbols(blobs):
    """Liest die JsDlgt-Methodentabelle (Paare: handler, name_ptr) aus seg1."""
    seg1 = blobs[0]["data"]
    mem = {b["base"]: b["data"] for b in blobs}

    def resolve(addr, length=48):
        for base, data in mem.items():
            if base <= addr < base + len(data):
                return base, data, addr - base
        return None, None, None

    syms = []
    off = TABLE_START - blobs[0]["base"]
    while off + 8 <= len(seg1):
        handler, name_ptr = struct.unpack_from("<II", seg1, off)
        base, data, noff = resolve(name_ptr)
        name = cstr(data, noff) if data is not None else None
        if not name or not (0x1F000000 <= handler < 0x1F100000):
            break
        if name.endswith("_JsDlgt"):
            name = name[: -len("_JsDlgt")]
        sect = 2 if resolve(handler)[0] == blobs[1]["base"] else 1
        syms.append({"name": name, "value": handler, "shndx": sect,
                     "table_off": TABLE_START + (off - (TABLE_START - blobs[0]["base"]))})
        off += 8
    return syms


def build_elf(blobs, syms):
    shstr = b"\x00"
    sh_name = {}

    def add_shname(n):
        nonlocal shstr
        sh_name[n] = len(shstr)
        shstr += n.encode() + b"\x00"

    for n in (".seg1", ".seg2", ".seg3", ".symtab", ".strtab", ".shstrtab"):
        add_shname(n)

    strtab = b"\x00"
    sym_entries = [struct.pack("<IIIBBH", 0, 0, 0, 0, 0, 0)]  # STN_UNDEF
    for s in syms:
        name_off = len(strtab)
        strtab += s["name"].encode() + b"\x00"
        info = (1 << 4) | 2  # STB_GLOBAL | STT_FUNC
        sym_entries.append(struct.pack("<IIIBBH", name_off, s["value"], 0, info, 0, s["shndx"]))
    symtab = b"".join(sym_entries)

    off = EHDR_SIZE + PHDR_SIZE * len(blobs)
    layout = []
    for b in blobs:
        layout.append(off)
        off += len(b["data"])
        off = (off + 3) & ~3
    symtab_off = off
    off += len(symtab)
    strtab_off = off
    off += len(strtab)
    shstrtab_off = off
    off += len(shstr)
    off = (off + 3) & ~3
    shoff = off
    shnum = 4 + 3  # NULL + 3 Segmente + symtab/strtab/shstrtab
    total = shoff + SHDR_SIZE * shnum
    shstrndx = shnum - 1

    ident = b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\x00" * 8
    ehdr = ident + struct.pack(
        "<HHIIIIIHHHHHH",
        2,          # ET_EXEC
        40,         # EM_ARM
        1,          # EV_CURRENT
        0x00100001, # e_entry (Reset-Handler, Thumb-Bit)
        EHDR_SIZE,  # e_phoff
        shoff,      # e_shoff
        0x05000000, # e_flags: EABI v5
        EHDR_SIZE, PHDR_SIZE, len(blobs),
        SHDR_SIZE, shnum, shstrndx,
    )

    phdrs = b""
    for b, file_off in zip(blobs, layout):
        phdrs += struct.pack("<IIIIIIII", 1, file_off, b["base"], b["base"],
                             len(b["data"]), len(b["data"]), b["ph_flags"], 4)

    image = bytearray(total)
    image[0:len(ehdr)] = ehdr
    image[EHDR_SIZE:EHDR_SIZE + len(phdrs)] = phdrs
    for b, file_off in zip(blobs, layout):
        image[file_off:file_off + len(b["data"])] = b["data"]
    image[symtab_off:symtab_off + len(symtab)] = symtab
    image[strtab_off:strtab_off + len(strtab)] = strtab
    image[shstrtab_off:shstrtab_off + len(shstr)] = shstr

    shdrs = [b"\x00" * SHDR_SIZE]
    for idx, (b, file_off) in enumerate(zip(blobs, layout), start=1):
        shdrs.append(struct.pack("<IIIIIIIIII", sh_name["." + b["name"]],
                                 1, b["sh_flags"], b["base"], file_off,
                                 len(b["data"]), 0, 0, 4, 0))
    shdrs.append(struct.pack("<IIIIIIIIII", sh_name[".symtab"], 2, 0, 0,
                             symtab_off, len(symtab), 5, 1, 4, 16))
    shdrs.append(struct.pack("<IIIIIIIIII", sh_name[".strtab"], 3, 0, 0,
                             strtab_off, len(strtab), 1, 0, 0, 0))
    shdrs.append(struct.pack("<IIIIIIIIII", sh_name[".shstrtab"], 3, 0, 0,
                             shstrtab_off, len(shstr), 1, 0, 0, 0))
    image[shoff:shoff + SHDR_SIZE * shnum] = b"".join(shdrs)
    return bytes(image)


def main():
    unpacked = sys.argv[1] if len(sys.argv) > 1 else "firmware/color2_142_0027/unpacked"
    out = sys.argv[2] if len(sys.argv) > 2 else "firmware/color2_142_0027/color2_142_0027.elf"
    blobs = load_segments(unpacked)
    syms = read_symbols(blobs)
    if not syms:
        raise SystemExit("keine JsDlgt-Symbole gefunden - TABLE_START pruefen")
    elf = build_elf(blobs, syms)
    with open(out, "wb") as fh:
        fh.write(elf)
    print("wrote %s (%d bytes, %d Symbole)" % (out, len(elf), len(syms)))
    for s in syms[:5]:
        print("  0x%08x %s" % (s["value"], s["name"]))
    print("  ... 0x%08x %s" % (syms[-1]["value"], syms[-1]["name"]))


if __name__ == "__main__":
    main()
