#!/usr/bin/env python3
"""Recover function entry points for a module without heuristics on code bytes.

Sources, in decreasing order of authority:
  eh    .eh_frame_hdr binary-search table (one FDE per function, with exact extents)
  sym   exported dynamic symbols of type FUNC
  rel   relocations whose target lies in text (address-taken code: vtables, callbacks)
  init  .init_array entries
  bl    direct call targets
  plt   import trampolines (found by shape; lifted like any other 16-byte function)

Usage: funcs.py <nso> [--out funcs.tsv]
"""
import re
import struct
import sys

from nso import Module


def bl_targets(text, base, lo, hi):
    """Targets of every direct call in a text image. Single process on purpose: this is
    imported by many tools, and a worker pool here hangs when the caller is not a script."""
    out = set()
    for m in re.finditer(rb"[\x94-\x97]", text[3::4]):   # top byte of each word: BL imm26
        off = m.start() * 4
        imm = struct.unpack_from("<I", text, off)[0] & 0x3FFFFFF
        if imm & 0x2000000:
            imm -= 0x4000000
        t = base + off + imm * 4
        if lo <= t < hi:
            out.add(t)
    return out


def import_stubs(mod):
    """{stub address: GOT slot address} for the module's import trampolines:

        adrp x16, slot@page ; ldr x17, [x16, slot@lo] ; add x16, x16, slot@lo ; br x17

    The stubs sit after the last compiled function. Some are only ever reached by a tail
    `b`, so they have to be found by shape rather than through call targets."""
    lo = max((a + s for a, s in eh_frame_functions(mod)), default=mod.text.mem)
    slots = {off for off, typ, _sym, _add in mod.relocs if typ == 1026}   # R_AARCH64_JUMP_SLOT
    stubs = {}
    for a in range(lo & ~3, mod.text.end - 15, 4):
        w0, w1, w2, w3 = struct.unpack_from("<4I", mod.image, a)
        if (w0 & 0x9F00001F == 0x90000010 and w1 & 0xFFC003FF == 0xF9400211
                and w2 & 0xFFC003FF == 0x91000210 and w3 == 0xD61F0220):
            imm = (((w0 >> 5) & 0x7FFFF) << 2) | ((w0 >> 29) & 3)
            if imm & 0x100000:
                imm -= 0x200000
            slot = (a & ~0xFFF) + (imm << 12) + ((w1 >> 10) & 0xFFF) * 8
            if slot in slots:   # excludes the lazy-binding header, which has the same shape
                stubs[a] = slot
    return stubs


def eh_frame_functions(mod):
    """Yield (start, size) for every FDE reachable from .eh_frame_hdr."""
    start, end = mod.eh_frame_hdr
    img = mod.image
    if end <= start or img[start] != 1:
        return
    ptr_enc, cnt_enc, tab_enc = img[start + 1], img[start + 2], img[start + 3]
    if (ptr_enc, cnt_enc, tab_enc) != (0x1B, 0x03, 0x3B):  # pcrel|sdata4, udata4, datarel|sdata4
        raise NotImplementedError(f"eh_frame_hdr encodings {ptr_enc:#x} {cnt_enc:#x} {tab_enc:#x}")
    count = struct.unpack_from("<I", img, start + 8)[0]
    for i in range(count):
        loc, fde = struct.unpack_from("<ii", img, start + 12 + i * 8)
        fde += start
        length, cie_delta = struct.unpack_from("<Ii", img, fde)
        if length == 0 or length == 0xFFFFFFFF or cie_delta == 0:
            continue
        # FDE body for the usual 'zR' CIE with pcrel|sdata4 pointers: pc_begin, pc_range
        pc_begin, pc_range = struct.unpack_from("<iI", img, fde + 8)
        yield fde + 8 + pc_begin, pc_range


def discover(mod):
    """Return ({entry: set of sources}, {entry: size}) for a module."""
    lo, hi = mod.text.mem, mod.text.end
    found = {}
    sizes = {}

    for addr, size in eh_frame_functions(mod):
        if lo <= addr < hi:
            found.setdefault(addr, set()).add("eh")
            sizes[addr] = size
    for name, value, size, info, shndx in mod.exports:
        if info & 0xF == 2 and lo <= value < hi:
            found.setdefault(value, set()).add("sym")
            sizes.setdefault(value, size)
    for off, typ, sym, addend in mod.relocs:
        if typ == 1027 and lo <= addend < hi and addend % 4 == 0:
            found.setdefault(addend, set()).add("rel")
    ia, n = mod.dyn.get(25, 0), mod.dyn.get(27, 0) // 8
    relmap = {off: addend for off, typ, _s, addend in mod.relocs if typ == 1027}
    for i in range(n):
        t = relmap.get(ia + i * 8, mod.u64(ia + i * 8))
        if lo <= t < hi:
            found.setdefault(t, set()).add("init")

    for t in bl_targets(bytes(mod.image[lo:hi]), lo, lo, hi):
        found.setdefault(t, set()).add("bl")
    for addr in import_stubs(mod):
        found.setdefault(addr, set()).add("plt")
        sizes[addr] = 16
    return found, sizes


def main():
    mod = Module(sys.argv[1])
    lo, hi = mod.text.mem, mod.text.end
    found, sizes = discover(mod)

    def count(src):
        return sum(1 for v in found.values() if src in v)

    eh = count("eh")
    print(f"function entries: {len(found):,}")
    for src in ("eh", "sym", "rel", "init", "bl", "plt"):
        print(f"  {src:<5}{count(src):>9,}")
    if eh:
        covered = sum(sizes[a] for a, v in found.items() if "eh" in v)
        outside = sum(1 for a, v in found.items() if "eh" not in v)
        print(f"unwind table covers {covered:,} of {hi - lo:,} text bytes ({covered * 100 / (hi - lo):.2f}%)")
        print(f"entries not described by the unwind table: {outside:,}")
    else:
        print("no usable .eh_frame_hdr table")

    if "--out" in sys.argv:
        path = sys.argv[sys.argv.index("--out") + 1]
        with open(path, "w") as f:
            for a in sorted(found):
                f.write(f"{a:#x}\t{sizes.get(a, 0)}\t{','.join(sorted(found[a]))}\n")
        print(f"wrote {path}")


if __name__ == "__main__":
    main()
