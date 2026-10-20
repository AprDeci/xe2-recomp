#!/usr/bin/env python3
"""NSO0 module loader: decompression, dynamic-table parsing, ELF conversion.

Usage:
  nso.py info <nso>...
  nso.py elf  <nso> <out.elf>
"""
import hashlib
import struct
import sys
from collections import Counter

DT = {1: "NEEDED", 2: "PLTRELSZ", 3: "PLTGOT", 4: "HASH", 5: "STRTAB", 6: "SYMTAB", 7: "RELA", 8: "RELASZ",
      9: "RELAENT", 10: "STRSZ", 11: "SYMENT", 12: "INIT", 13: "FINI", 14: "SONAME", 20: "PLTREL",
      23: "JMPREL", 25: "INIT_ARRAY", 26: "FINI_ARRAY", 27: "INIT_ARRAYSZ", 28: "FINI_ARRAYSZ",
      0x6FFFFEF5: "GNU_HASH", 0x6FFFFFF9: "RELACOUNT"}
R_AARCH64 = {257: "ABS64", 1024: "COPY", 1025: "GLOB_DAT", 1026: "JUMP_SLOT", 1027: "RELATIVE",
             1028: "TLS_DTPMOD", 1029: "TLS_DTPREL", 1030: "TLS_TPREL", 1031: "TLSDESC"}


def lz4_block(src, usize):
    dst = bytearray(usize)
    si = di = 0
    n = len(src)
    while si < n:
        token = src[si]
        si += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = src[si]
                si += 1
                lit += b
                if b != 255:
                    break
        if lit:
            dst[di:di + lit] = src[si:si + lit]
            si += lit
            di += lit
        if si >= n:
            break
        off = src[si] | (src[si + 1] << 8)
        si += 2
        ml = token & 15
        if ml == 15:
            while True:
                b = src[si]
                si += 1
                ml += b
                if b != 255:
                    break
        ml += 4
        ms = di - off
        if off >= ml:
            dst[di:di + ml] = dst[ms:ms + ml]
        else:  # overlapping match: the source pattern repeats
            dst[di:di + ml] = (bytes(dst[ms:di]) * (ml // off + 1))[:ml]
        di += ml
    if di != usize:
        raise ValueError(f"lz4: produced {di} bytes, expected {usize}")
    return dst


class Segment:
    def __init__(self, name, mem, size):
        self.name, self.mem, self.size = name, mem, size

    @property
    def end(self):
        return self.mem + self.size

    def __contains__(self, addr):
        return self.mem <= addr < self.end


class Module:
    """A decompressed NSO laid out at its link-time addresses (base 0)."""

    def __init__(self, path):
        self.path = path
        raw = open(path, "rb").read()
        if raw[0x10:0x14] == b"NRO0":
            self._load_nro(raw)
            return
        if raw[:4] != b"NSO0":
            raise ValueError(f"{path}: not an NSO")
        flags = struct.unpack_from("<I", raw, 0x0C)[0]
        segs = []
        for i, name in enumerate(("text", "rodata", "data")):
            foff, mem, size = struct.unpack_from("<III", raw, 0x10 + i * 0x10)
            csize = struct.unpack_from("<I", raw, 0x60 + i * 4)[0]
            blob = raw[foff:foff + csize]
            body = lz4_block(blob, size) if flags & (1 << i) else bytearray(blob)
            if flags & (8 << i) and hashlib.sha256(body).digest() != raw[0xA0 + i * 0x20:0xC0 + i * 0x20]:
                raise ValueError(f"{path}: {name} segment hash mismatch")
            segs.append((Segment(name, mem, size), body))
        self.verified = flags & 0x38 == 0x38
        self.bss_size = struct.unpack_from("<I", raw, 0x3C)[0]
        self.build_id = raw[0x40:0x60].hex().rstrip("0").upper() or "0"
        self.text, self.rodata, self.data = (s for s, _ in segs)
        self.file_end = self.data.end
        self.image = bytearray(self.file_end)
        for seg, body in segs:
            self.image[seg.mem:seg.mem + seg.size] = body
        self._parse_dynamic()

    def _load_nro(self, raw):
        """An NRO (a module loaded at run time by nn::ro) is uncompressed and already laid out."""
        segs = [Segment(name, *struct.unpack_from("<II", raw, 0x20 + i * 8))
                for i, name in enumerate(("text", "rodata", "data"))]
        self.verified = True
        self.bss_size = struct.unpack_from("<I", raw, 0x38)[0]
        self.build_id = raw[0x40:0x60].hex().rstrip("0").upper() or "0"
        self.text, self.rodata, self.data = segs
        self.file_end = self.data.end
        self.image = bytearray(raw[:self.file_end].ljust(self.file_end, b"\0"))
        self._parse_dynamic()

    # --- raw accessors -------------------------------------------------
    def u32(self, a):
        return struct.unpack_from("<I", self.image, a)[0]

    def u64(self, a):
        return struct.unpack_from("<Q", self.image, a)[0]

    def cstr(self, a):
        return self.image[a:self.image.index(b"\0", a)].decode("utf-8", "replace")

    # --- dynamic section -----------------------------------------------
    def _parse_dynamic(self):
        mod = self.u32(4)
        if self.image[mod:mod + 4] != b"MOD0":
            raise ValueError(f"{self.path}: MOD0 not found")
        rel = struct.unpack_from("<6i", self.image, mod + 4)
        self.dynamic_addr = mod + rel[0]
        self.bss_start, self.bss_end = mod + rel[1], mod + rel[2]
        self.eh_frame_hdr = (mod + rel[3], mod + rel[4])
        self.dyn = {}
        self.dyn_count = 0
        a = self.dynamic_addr
        while True:
            tag, val = struct.unpack_from("<QQ", self.image, a)
            a += 16
            self.dyn_count += 1
            if tag == 0:
                break
            self.dyn.setdefault(tag, val)
        self.mem_end = max(self.bss_end, self.file_end + self.bss_size)

        symtab, strtab = self.dyn.get(6), self.dyn.get(5)
        nsyms = self.u32(self.dyn[4] + 4) if 4 in self.dyn else (strtab - symtab) // 24
        self.symbols = []
        for i in range(nsyms):
            name, info, other, shndx, value, size = struct.unpack_from("<IBBHQQ", self.image, symtab + i * 24)
            self.symbols.append((self.cstr(strtab + name) if name else "", value, size, info, shndx))
        self.imports = [s for s in self.symbols if s[0] and s[4] == 0]
        self.exports = [s for s in self.symbols if s[0] and s[4] != 0]

        self.relocs = []  # (offset, type, symidx, addend)
        for tab, szt in ((7, 8), (23, 2)):
            if tab in self.dyn:
                base, size = self.dyn[tab], self.dyn[szt]
                for o in range(base, base + size, 24):
                    off, info, addend = struct.unpack_from("<QQq", self.image, o)
                    self.relocs.append((off, info & 0xFFFFFFFF, info >> 32, addend))

    def module_name(self):
        n = self.u32(self.rodata.mem + 4)
        if 0 < n < 0x200:
            return self.image[self.rodata.mem + 8:self.rodata.mem + 8 + n].decode("utf-8", "replace")
        return ""

    # --- ELF export ----------------------------------------------------
    def write_elf(self, out):
        shift = 0x1000  # file offset = vaddr + shift; page 0 holds the headers
        loads = [(self.text, 5, self.text.size), (self.rodata, 4, self.rodata.size),
                 (self.data, 6, self.mem_end - self.data.mem)]
        phdrs = b""
        for seg, flags, memsz in loads:
            phdrs += struct.pack("<IIQQQQQQ", 1, flags, seg.mem + shift, seg.mem, seg.mem, seg.size, memsz, 0x1000)
        dynsz = self.dyn_count * 16
        phdrs += struct.pack("<IIQQQQQQ", 2, 6, self.dynamic_addr + shift, self.dynamic_addr, self.dynamic_addr,
                             dynsz, dynsz, 8)
        ehdr = struct.pack("<16sHHIQQQIHHHHHH", b"\x7fELF\x02\x01\x01" + b"\0" * 9, 3, 183, 1, self.text.mem,
                           64, 0, 0, 64, 56, len(loads) + 1, 64, 0, 0)
        with open(out, "wb") as f:
            f.write((ehdr + phdrs).ljust(shift, b"\0"))
            f.write(self.image)

    def summary(self):
        rc = Counter(R_AARCH64.get(t, str(t)) for _, t, _, _ in self.relocs)
        lines = [f"{self.path}",
                 f"  name      {self.module_name()!r}",
                 f"  build id  {self.build_id}  (segment hashes {'verified' if self.verified else 'not present'})"]
        for s in (self.text, self.rodata, self.data):
            lines.append(f"  {s.name:<9} {s.mem:#010x} - {s.end:#010x}  ({s.size / 1048576:7.2f} MiB)")
        lines.append(f"  bss       {self.bss_start:#010x} - {self.bss_end:#010x}  ({(self.bss_end - self.bss_start) / 1048576:7.2f} MiB)")
        lines.append(f"  symbols   {len(self.symbols)} total, {len(self.imports)} imported, {len(self.exports)} exported")
        lines.append("  relocs    " + ", ".join(f"{k}={v}" for k, v in rc.most_common()))
        lines.append("  dynamic   " + ", ".join(f"{DT.get(t, hex(t))}={v:#x}" for t, v in sorted(self.dyn.items())))
        return "\n".join(lines)


if __name__ == "__main__":
    if len(sys.argv) < 3 or sys.argv[1] not in ("info", "elf"):
        sys.exit(__doc__)
    if sys.argv[1] == "info":
        for p in sys.argv[2:]:
            print(Module(p).summary())
    else:
        m = Module(sys.argv[2])
        m.write_elf(sys.argv[3])
        with open(sys.argv[3][:-4] + ".bin" if sys.argv[3].endswith(".elf") else sys.argv[3] + ".bin", "wb") as f:
            f.write(m.image)
        print(f"wrote {sys.argv[3]} (+ raw image)")
