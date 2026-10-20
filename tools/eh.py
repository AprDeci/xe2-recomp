#!/usr/bin/env python3
"""C++ exception landing pads of a module, from .eh_frame and the LSDAs it points to.

A function compiled with exceptions has an FDE whose augmentation carries a pointer to its
language-specific data area. The LSDA's call-site table says, for each range of call
instructions, where control lands (a catch or cleanup block) when an exception unwinds
through that call. The personality routine matches a frame by its return address minus one,
so a call at `pc` belongs to the range holding pc + 3.

Usage: eh.py <module>       (prints a summary)
"""
import struct
import sys

from funcs import eh_frame_functions  # noqa: F401  (same table, same assumptions)

OMIT = 0xFF


def uleb(b, p):
    v = shift = 0
    while True:
        x = b[p]
        p += 1
        v |= (x & 0x7F) << shift
        shift += 7
        if not x & 0x80:
            return v, p


def sleb(b, p):
    v = shift = 0
    while True:
        x = b[p]
        p += 1
        v |= (x & 0x7F) << shift
        shift += 7
        if not x & 0x80:
            if x & 0x40:
                v -= 1 << shift
            return v, p


def encoded(b, p, enc, func_start=0):
    """Read a DW_EH_PE-encoded value at p; returns (value, next p). Offsets are image offsets."""
    if enc == OMIT:
        return None, p
    fmt, app = enc & 0x0F, enc & 0x70
    start = p
    if fmt == 0x00:
        v = struct.unpack_from("<Q", b, p)[0]; p += 8
    elif fmt == 0x01:
        v, p = uleb(b, p)
    elif fmt == 0x02:
        v = struct.unpack_from("<H", b, p)[0]; p += 2
    elif fmt == 0x03:
        v = struct.unpack_from("<I", b, p)[0]; p += 4
    elif fmt == 0x04:
        v = struct.unpack_from("<Q", b, p)[0]; p += 8
    elif fmt == 0x09:
        v, p = sleb(b, p)
    elif fmt == 0x0A:
        v = struct.unpack_from("<h", b, p)[0]; p += 2
    elif fmt == 0x0B:
        v = struct.unpack_from("<i", b, p)[0]; p += 4
    elif fmt == 0x0C:
        v = struct.unpack_from("<q", b, p)[0]; p += 8
    else:
        raise NotImplementedError(f"pointer encoding {enc:#x}")
    if app == 0x10:
        v += start
    elif app == 0x40:
        v += func_start
    elif app not in (0x00,):
        raise NotImplementedError(f"pointer application {enc:#x}")
    return v, p


def _cie(b, cie):
    """(lsda encoding, fde pointer encoding) of the CIE at cie."""
    p = cie + 8
    version = b[p]; p += 1
    end = b.index(0, p)
    aug = bytes(b[p:end]).decode(); p = end + 1
    if version >= 4:
        p += 2
    _, p = uleb(b, p)
    _, p = sleb(b, p)
    if version == 1:
        p += 1
    else:
        _, p = uleb(b, p)
    lsda_enc, fde_enc = OMIT, 0x00
    if aug.startswith("z"):
        _, p = uleb(b, p)
        for ch in aug[1:]:
            if ch == "L":
                lsda_enc = b[p]; p += 1
            elif ch == "R":
                fde_enc = b[p]; p += 1
            elif ch == "P":
                penc = b[p]; p += 1
                _, p = encoded(b, p, penc)
            elif ch in "SB":
                pass
            else:
                break
    return lsda_enc, fde_enc


def _fdes(mod):
    """Yield (function start, size, lsda offset or None) for each FDE of .eh_frame_hdr's table."""
    start, end = mod.eh_frame_hdr
    b = mod.image
    if end <= start or b[start] != 1:
        return
    count = struct.unpack_from("<I", b, start + 8)[0]
    cies = {}
    for i in range(count):
        _loc, fde = struct.unpack_from("<ii", b, start + 12 + i * 8)
        fde += start
        length, cie_delta = struct.unpack_from("<Ii", b, fde)
        if length in (0, 0xFFFFFFFF) or cie_delta == 0:
            continue
        cie = fde + 4 - cie_delta
        if cie not in cies:
            cies[cie] = _cie(b, cie)
        lsda_enc, fde_enc = cies[cie]
        p = fde + 8
        pc_begin, p = encoded(b, p, fde_enc)
        pc_range, p = encoded(b, p, fde_enc & 0x0F)
        lsda = None
        if lsda_enc != OMIT:
            _, p = uleb(b, p)
            lsda, p = encoded(b, p, lsda_enc)
            if lsda == 0 and lsda_enc & 0x70 == 0:
                lsda = None
        yield pc_begin, pc_range, lsda


def _call_sites(b, lsda, func_start):
    """Yield (start, end, landing pad) image offsets from an LSDA's call-site table."""
    p = lsda
    lp_enc = b[p]; p += 1
    lp_start = func_start
    if lp_enc != OMIT:
        lp_start, p = encoded(b, p, lp_enc)
    tt_enc = b[p]; p += 1
    if tt_enc != OMIT:
        _, p = uleb(b, p)
    cs_enc = b[p]; p += 1
    cs_len, p = uleb(b, p)
    end = p + cs_len
    while p < end:
        s, p = encoded(b, p, cs_enc)
        n, p = encoded(b, p, cs_enc)
        lp, p = encoded(b, p, cs_enc)
        _, p = uleb(b, p)
        if lp:
            yield func_start + s, func_start + s + n, lp_start + lp


def landing_pads(mod):
    """{function start: [(range start, range end, landing pad)]} for functions with landing pads."""
    out = {}
    for start, size, lsda in _fdes(mod):
        if lsda is None or not 0 < lsda < len(mod.image):
            continue
        try:
            sites = list(_call_sites(mod.image, lsda, start))
        except (NotImplementedError, IndexError, struct.error):
            continue
        sites = [s for s in sites if start <= s[2] < start + size]
        if sites:
            out[start] = sites
    return out


def main():
    sys.path.insert(0, "tools")
    from nso import Module
    import os
    name = sys.argv[1]
    path = "dump/exefs/" + name if os.path.exists("dump/exefs/" + name) else "dump/nro/" + name + ".nro"
    pads = landing_pads(Module(path))
    sites = sum(len(v) for v in pads.values())
    print(f"{name}: {len(pads)} functions with landing pads, {sites} call-site ranges")
    for f in sorted(pads)[:5]:
        print(f"  {f:#x}: " + ", ".join(f"[{a:#x},{e:#x})->{lp:#x}" for a, e, lp in pads[f][:6]))


if __name__ == "__main__":
    main()
