"""Shared helpers for emitting C from AArch64 instructions."""

BASE = 0x7100000000
M64 = (1 << 64) - 1
CONDS = ["zf", "!zf", "cf", "!cf", "nf", "!nf", "vf", "!vf", "(cf && !zf)", "!(cf && !zf)",
         "(nf == vf)", "(nf != vf)", "(!zf && nf == vf)", "!(!zf && nf == vf)", "1", "1"]


class Unsupported(Exception):
    pass


def sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def const(v, sf=1):
    return f"0x{v & M64:X}ull" if sf else f"0x{v & 0xFFFFFFFF:X}u"


def ty(sf):
    return ("u64", "i64", 64) if sf else ("u32", "i32", 32)


def reg(n, sf=1, sp=False):
    if n == 31:
        if not sp:
            return "0ull" if sf else "0u"
        return "SP" if sf else "(u32)SP"
    return f"X({n})" if sf else f"(u32)X({n})"


def wr(n, sf, expr, sp=False):
    if n == 31 and not sp:
        return f"(void)({expr});"
    dst = "SP" if n == 31 else f"X({n})"
    return f"{dst} = {expr};" if sf else f"{dst} = (u32)({expr});"


def gpr_defs(w):
    """General-purpose registers an instruction writes (branches excluded: see callers)."""
    op0 = (w >> 25) & 0xF
    if op0 in (8, 9) or op0 & 7 == 5:
        return (w & 31,)
    if op0 & 5 == 4:
        group, vec, rn, rt = (w >> 27) & 7, (w >> 26) & 1, (w >> 5) & 31, w & 31
        out = []
        if group == 1:
            if vec:
                if (w >> 23) & 1:
                    out.append(rn)                      # post-indexed structure access
            elif (w >> 22) & 1:
                out.append(rt)                          # load-exclusive / load-acquire
                if (w >> 21) & 1:
                    out.append((w >> 10) & 31)
            else:
                out.append((w >> 16) & 31)              # store-exclusive status
        elif group == 3:
            if not vec:
                out.append(rt)
        elif group == 5:
            if (w >> 22) & 1 and not vec:
                out += [rt, (w >> 10) & 31]
            if (w >> 23) & 1:
                out.append(rn)
        elif group == 7:
            if not vec and (w >> 22) & 3:
                out.append(rt)
            if not (w >> 24) & 1 and not (w >> 21) & 1 and (w >> 10) & 1:
                out.append(rn)
        return tuple(out)
    if op0 & 7 == 7:
        if w & 0x7F200000 == 0x1E200000 and not (w >> 10) & 0x3F:   # fp <-> integer
            return (w & 31,) if (w >> 16) & 7 in (0, 1, 4, 5, 6) else ()
        if w & 0x7F200000 == 0x1E000000:                            # fp -> fixed point
            return (w & 31,) if (w >> 16) & 7 in (0, 1) else ()
        if w & 0xBFE08400 == 0x0E000400 and (w >> 11) & 15 in (5, 7):   # umov / smov
            return (w & 31,)
        return ()
    if w >> 20 == 0xD53:                                            # mrs
        return (w & 31,)
    return ()
