"""AArch64 -> C lifter. One guest function becomes one C function over a Cpu context.

Addresses inside this module are module-relative offsets; BASE is added only when a
value is materialised into guest-visible state (adr/adrp, link register, table cases).
"""
import bisect
import struct
from collections import Counter

from a64 import BASE, CONDS, M64, Unsupported, const, gpr_defs, reg, sx, ty, wr  # noqa: F401 (re-exported)
from lift_simd import SimdMixin


def decode_bit_masks(n, imms, immr, size):
    length = ((n << 6) | (~imms & 0x3F)).bit_length() - 1
    if length < 1:
        raise Unsupported("reserved logical immediate")
    levels = (1 << length) - 1
    s, r, esize = imms & levels, immr & levels, 1 << length
    if s == levels:
        raise Unsupported("reserved logical immediate")
    welem = (1 << (s + 1)) - 1
    welem = ((welem >> r) | (welem << (esize - r))) & ((1 << esize) - 1)
    mask = 0
    for i in range(0, size, esize):
        mask |= welem << i
    return mask


def shifted(expr, kind, amount, sf):
    t, s, size = ty(sf)
    if amount == 0:
        return expr
    if kind == 0:
        return f"({expr} << {amount})"
    if kind == 1:
        return f"({expr} >> {amount})"
    if kind == 2:
        return f"({t})(({s}){expr} >> {amount})"
    return f"(({expr} >> {amount}) | ({expr} << {size - amount}))"


def extended(rm, option, shift, sf):
    v = reg(rm, 1)
    e = [f"(u64)(u8){v}", f"(u64)(u16){v}", f"(u64)(u32){v}", v,
         f"(u64)(i64)(i8){v}", f"(u64)(i64)(i16){v}", f"(u64)(i64)(i32){v}", v][option]
    e = f"(({e}) << {shift})" if shift else f"({e})"
    return e if sf else f"(u32){e}"


def addsub(sf, sub, setflags, a, b, rd, rd_sp):
    t, _, size = ty(sf)
    if not setflags:
        return wr(rd, sf, f"{a} {'-' if sub else '+'} {b}", sp=rd_sp)
    if sub:
        fl = f"cf = a_ >= b_; vf = ((a_ ^ b_) & (a_ ^ r_)) >> {size - 1};"
    else:
        fl = f"cf = r_ < a_; vf = (~(a_ ^ b_) & (a_ ^ r_)) >> {size - 1};"
    return (f"{{ {t} a_ = {a}, b_ = {b}, r_ = a_ {'-' if sub else '+'} b_; nf = r_ >> {size - 1}; zf = r_ == 0; "
            f"{fl} {wr(rd, sf, 'r_')} }}")


def addcarry(sf, setflags, a, b, rd):
    t, _, size = ty(sf)
    if not setflags:
        return wr(rd, sf, f"{a} + {b} + cf")
    return (f"{{ {t} a_ = {a}, b_ = {b}, r_ = a_ + b_ + cf; nf = r_ >> {size - 1}; zf = r_ == 0; "
            f"vf = (~(a_ ^ b_) & (a_ ^ r_)) >> {size - 1}; cf = r_ < a_ || (cf && r_ == a_); {wr(rd, sf, 'r_')} }}")


def logical(sf, opc, a, b, rd, rd_sp):
    t, _, size = ty(sf)
    if opc == 3:
        return f"{{ {t} r_ = {a} & {b}; nf = r_ >> {size - 1}; zf = r_ == 0; cf = 0; vf = 0; {wr(rd, sf, 'r_')} }}"
    return wr(rd, sf, f"{a} {'&|^'[opc]} {b}", sp=rd_sp)


def fp_imm8(imm8):
    b6 = (imm8 >> 6) & 1
    exp = ((b6 ^ 1) << 10) | ((0xFF if b6 else 0) << 2) | ((imm8 >> 4) & 3)
    bits = ((imm8 >> 7) << 63) | (exp << 52) | ((imm8 & 0xF) << 48)
    return struct.unpack("<d", struct.pack("<Q", bits))[0]


class Lifter(SimdMixin):
    def __init__(self, image, entries, base=BASE, forward=None, setjmp=None, pads=None):
        self.img = image
        self.base = base         # guest address the module is loaded at
        self.forward = forward or {}   # entry offset -> native function that replaces it
        self.setjmp = setjmp or set()  # offsets of setjmp (or its import stub): calls become host setjmp
        self.pads = pads or {}   # function offset -> [(call range start, end, landing pad)] (eh.py)
        self.entries = entries   # set of known function entry offsets
        self.tables = 0          # jump tables resolved
        self.indirect_tail = 0   # br sites treated as indirect tail calls
        self.table_like = 0      # br sites fed by a table load, resolved or not
        self.table_at = {}       # per function: index of `br` -> set of target offsets
        self.unresolved = []     # table-style `br` sites whose table could not be found
        self.assumed = 0         # tables accepted by validation rather than proof

    # ------------------------------------------------------------------ function
    def lift_function(self, start, size):
        self.fstart, self.fend = start, start + size
        self.leaders, self.callees = set(), set()
        native = self.forward.get(start)
        if isinstance(native, str):   # replaced by a native implementation: every caller reaches it
            return (f"GuestFn {native};\nvoid f_{self.base + start:x}(Cpu *c) {{ {native}(c); }}",
                    Counter(), set())
        name, wrapper = f"f_{self.base + start:x}", ""
        if isinstance(native, tuple):   # wrapped natively: callers reach the wrapper, which may call this one
            wrapper = f"GuestFn {native[1]};\nvoid {name}(Cpu *c) {{ {native[1]}(c); }}\n"
            name += "_orig"
        self.words = struct.unpack_from(f"<{size // 4}I", self.img, start)
        self.sites = self.pads.get(start, [])
        self.leaders.update(lp for _s, _e, lp in self.sites)   # entered from an exception
        self.analyze_tables()
        reasons = Counter()
        body, falls = [], True
        for i, w in enumerate(self.words):
            self.i = i
            pc = start + i * 4
            try:
                stmt, falls = self.insn(w, pc)
            except Unsupported as e:
                reasons[(str(e), w)] += 1
                stmt, falls = f'rt_trap(c, {const(self.base +pc)}, "unsupported");', True
            body.append((pc, stmt))
        out = [f"void {name}(Cpu *c) {{",
               "    u32 nf = 0, zf = 0, cf = 0, vf = 0; u64 ex_a = 0, ex_v = 0;"]
        for pc, stmt in body:
            if pc in self.leaders:
                out.append(f"L_{pc:x}:;")
            out.append("    " + stmt)
        if falls and self.fend in self.entries:   # assembly routines may run on into the next one
            self.callees.add(self.fend)
            out.append(f"    TAIL(f_{self.base + self.fend:x})")
        elif falls:
            out.append(f'    rt_trap(c, {const(self.base + self.fend)}, "fell off the end of a function");')
        out.append("}")
        return wrapper + "\n".join(out), reasons, self.callees

    def goto(self, target):
        if self.fstart <= target < self.fend:
            self.leaders.add(target)
            return f"goto L_{target:x};"
        if target in self.entries:
            self.callees.add(target)
            return f"TAIL(f_{self.base + target:x})"
        raise Unsupported("branch into another function's body")

    def call(self, target, ret):
        if target in self.setjmp:
            # setjmp returns twice: the second time by longjmp, into this very frame. The
            # host's setjmp does that for the C function; rt_setjmp saves the guest's
            # callee-saved registers, which the native longjmp restores (and sets X(0)).
            return "if (!setjmp(*rt_setjmp(c))) X(0) = 0;"
        self.callees.add(target)
        return self.landing(ret, f"X(30) = {const(self.base +ret)}; f_{self.base + target:x}(c);")

    def call_indirect(self, rn, ret):
        return self.landing(ret, f"{{ u64 t_ = {reg(rn)}; X(30) = {const(self.base +ret)}; rt_lookup(t_)(c); }}")

    def landing(self, ret, call):
        """A call an exception may unwind through into a landing pad of this function: the
        native unwinder (rt_unwind_jumpto) raises a host exception naming the pad and the stack
        pointer of the frame it resumes, and the frame whose call that is takes it."""
        pc = ret - 4
        for s, e, lp in self.sites:
            if s <= pc + 3 < e and self.fstart <= lp < self.fend:
                return (f"{{ u64 sp_ = SP; int lp_ = 0; __try {{ {call} }} "
                        f"__except (rt_eh_filter(GetExceptionInformation(), sp_, {const(self.base + lp)})) {{ lp_ = 1; }} "
                        f"if (lp_) goto L_{lp:x}; }}")
        return call

    def ret(self, rn):
        return "return;"

    # ------------------------------------------------------------------ dispatch
    def insn(self, w, pc):
        """Return (C statement, falls_through)."""
        if w == 0xD503201F:
            return ";", True
        op0 = (w >> 25) & 0xF
        if op0 in (8, 9):
            return self.dp_imm(w, pc), True
        if op0 in (10, 11):
            return self.branch(w, pc)
        if op0 & 5 == 4:
            return self.load_store(w, pc), True
        if op0 & 7 == 5:
            return self.dp_reg(w), True
        if op0 & 7 == 7:
            return self.fp(w), True
        if w >> 16 == 0 or w == 0xE7FFDEFE:   # udf, and the SDK's permanently-undefined abort word
            return f'rt_trap(c, {const(self.base +pc)}, "udf");', False
        raise Unsupported("unallocated encoding")

    # ------------------------------------------------------------------ data processing (immediate)
    def dp_imm(self, w, pc):
        op = (w >> 23) & 7
        sf, rd, rn = w >> 31, w & 31, (w >> 5) & 31
        t, s, size = ty(sf)
        if op in (0, 1):
            imm = sx((((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3), 21)
            val = (pc & ~0xFFF) + (imm << 12) if sf else pc + imm
            return wr(rd, 1, const(self.base +val))
        if op == 2:
            imm = ((w >> 10) & 0xFFF) << (12 if (w >> 22) & 1 else 0)
            setflags = (w >> 29) & 1
            return addsub(sf, (w >> 30) & 1, setflags, reg(rn, sf, sp=True), const(imm, sf), rd, not setflags)
        immr, imms, n = (w >> 16) & 63, (w >> 10) & 63, (w >> 22) & 1
        if op == 4:
            opc = (w >> 29) & 3
            if n and not sf:
                raise Unsupported("reserved logical immediate")
            mask = decode_bit_masks(n, imms, immr, size)
            return logical(sf, opc, reg(rn, sf), const(mask, sf), rd, opc != 3)
        if op == 5:
            opc, shift, imm16 = (w >> 29) & 3, ((w >> 21) & 3) * 16, (w >> 5) & 0xFFFF
            if shift >= size or opc == 1:
                raise Unsupported("reserved move-wide")
            if opc == 0:
                return wr(rd, sf, const(~(imm16 << shift), sf))
            if opc == 2:
                return wr(rd, sf, const(imm16 << shift, sf))
            return wr(rd, sf, f"({reg(rd, sf)} & {const(~(0xFFFF << shift), sf)}) | {const(imm16 << shift, sf)}")
        if op == 6:
            opc = (w >> 29) & 3
            if n != sf or opc == 3 or (not sf and (immr > 31 or imms > 31)):
                raise Unsupported("reserved bitfield")
            src, dst = reg(rn, sf), reg(rd, sf)
            if imms >= immr:
                width = imms - immr + 1
                m = (1 << width) - 1
                field = f"(({src} >> {immr}) & {const(m, sf)})"
                if opc == 2:
                    return wr(rd, sf, field)
                if opc == 0:
                    return wr(rd, sf, f"({t})(({s})(({t}){src} << {size - 1 - imms}) >> {size - width})")
                return wr(rd, sf, f"({dst} & {const(~m, sf)}) | {field}")
            width, lsb = imms + 1, size - immr
            m = (1 << width) - 1
            field = f"(({src} & {const(m, sf)}) << {lsb})"
            if opc == 2:
                return wr(rd, sf, field)
            if opc == 0:
                return wr(rd, sf, f"({t})(({s})(({t}){src} << {size - width}) >> {size - width - lsb})")
            return wr(rd, sf, f"({dst} & {const(~(m << lsb), sf)}) | {field}")
        if op == 7:
            rm = (w >> 16) & 31
            if (w >> 29) & 3 or (w >> 21) & 1 or n != sf or imms >= size:
                raise Unsupported("reserved extract")
            if imms == 0:
                return wr(rd, sf, reg(rm, sf))
            return wr(rd, sf, f"({reg(rm, sf)} >> {imms}) | ({reg(rn, sf)} << {size - imms})")
        raise Unsupported("dp-imm: tagged add/sub")

    # ------------------------------------------------------------------ data processing (register)
    def dp_reg(self, w):
        sf, rm, rn, rd = w >> 31, (w >> 16) & 31, (w >> 5) & 31, w & 31
        t, s, size = ty(sf)
        a = reg(rn, sf)
        if not (w >> 28) & 1:
            kind, imm6 = (w >> 22) & 3, (w >> 10) & 63
            if not (w >> 24) & 1:
                if imm6 >= size:
                    raise Unsupported("reserved shift amount")
                b = shifted(reg(rm, sf), kind, imm6, sf)
                if (w >> 21) & 1:
                    b = f"~{b}"
                return logical(sf, (w >> 29) & 3, a, b, rd, False)
            sub, setflags = (w >> 30) & 1, (w >> 29) & 1
            if not (w >> 21) & 1:
                if kind == 3 or imm6 >= size:
                    raise Unsupported("reserved shift")
                return addsub(sf, sub, setflags, a, shifted(reg(rm, sf), kind, imm6, sf), rd, False)
            if kind:
                raise Unsupported("reserved add/sub extended")
            b = extended(rm, (w >> 13) & 7, (w >> 10) & 7, sf)
            return addsub(sf, sub, setflags, reg(rn, sf, sp=True), b, rd, not setflags)

        b = reg(rm, sf)
        if (w >> 24) & 1:   # three-source
            op31, o0, ra = (w >> 21) & 7, (w >> 15) & 1, (w >> 10) & 31
            if (w >> 29) & 3:
                raise Unsupported("reserved dp3")
            acc, sign = reg(ra, sf), "-" if o0 else "+"
            if op31 == 0:
                return wr(rd, sf, f"{acc} {sign} {a} * {b}")
            if not sf:
                raise Unsupported("reserved dp3")
            if op31 in (1, 5):
                c = "(u64)(i64)(i32)" if op31 == 1 else "(u64)(u32)"
                return wr(rd, 1, f"{reg(ra)} {sign} {c}{reg(rn)} * {c}{reg(rm)}")
            if op31 in (2, 6) and not o0:
                return wr(rd, 1, f"{'smulh64' if op31 == 2 else 'umulh64'}({a}, {b})")
            raise Unsupported("reserved dp3")
        op2 = (w >> 21) & 0xF
        setflags = (w >> 29) & 1
        if op2 == 0 and not (w >> 10) & 63:   # adc / sbc
            return addcarry(sf, setflags, a, f"~{b}" if (w >> 30) & 1 else b, rd)
        if op2 == 2 and setflags and not (w >> 10) & 1 and not (w >> 4) & 1:   # ccmp / ccmn
            cond, nzcv = CONDS[(w >> 12) & 15], w & 15
            other = const(rm, sf) if (w >> 11) & 1 else b
            cmp_ = addsub(sf, (w >> 30) & 1, 1, a, other, 31, False)
            return (f"if ({cond}) {cmp_} else {{ nf = {nzcv >> 3}; zf = {(nzcv >> 2) & 1}; "
                    f"cf = {(nzcv >> 1) & 1}; vf = {nzcv & 1}; }}")
        if op2 == 4 and not setflags and not (w >> 11) & 1:   # csel family
            cond = CONDS[(w >> 12) & 15]
            alt = [b, f"{b} + 1", f"~{b}", f"0 - {b}"][((w >> 30) & 1) * 2 + ((w >> 10) & 1)]
            return wr(rd, sf, f"{cond} ? {a} : {alt}")
        if op2 == 6 and not setflags:
            opcode = (w >> 10) & 63
            n = "64" if sf else "32"
            if (w >> 30) & 1:   # one-source
                if rm:
                    raise Unsupported("dp1: pointer authentication")
                if opcode == 0:
                    return wr(rd, sf, f"__builtin_bitreverse{n}({a})")
                if opcode == 1:
                    return wr(rd, sf, f"rev16_{n}({a})")
                if opcode == 2:
                    return wr(rd, sf, f"rev32_64({a})" if sf else f"__builtin_bswap32({a})")
                if opcode == 3 and sf:
                    return wr(rd, sf, f"__builtin_bswap64({a})")
                if opcode == 4:
                    return wr(rd, sf, f"clz{n}({a})")
                if opcode == 5:
                    return wr(rd, sf, f"cls{n}({a})")
                raise Unsupported("reserved dp1")
            if opcode == 2:
                return wr(rd, sf, f"udiv{n}({a}, {b})")
            if opcode == 3:
                return wr(rd, sf, f"sdiv{n}({a}, {b})")
            if opcode == 8:
                return wr(rd, sf, f"{a} << ({b} & {size - 1})")
            if opcode == 9:
                return wr(rd, sf, f"{a} >> ({b} & {size - 1})")
            if opcode == 10:
                return wr(rd, sf, f"({t})(({s}){a} >> ({b} & {size - 1}))")
            if opcode == 11:
                return wr(rd, sf, f"ror{n}({a}, (u32){b})")
            raise Unsupported("dp2: crc32 / other")
        raise Unsupported("reserved dp-reg")

    # ------------------------------------------------------------------ branches / system
    def branch(self, w, pc):
        if (w >> 26) & 0x1F == 5:
            target = pc + (sx(w & 0x3FFFFFF, 26) << 2)
            if w >> 31:
                return self.call(target, pc + 4), True
            return self.goto(target), False
        if (w >> 25) & 0x3F == 0x1A:
            target = pc + (sx((w >> 5) & 0x7FFFF, 19) << 2)
            return f"if ({reg(w & 31, w >> 31)} {'!=' if (w >> 24) & 1 else '=='} 0) {self.goto(target)}", True
        if (w >> 25) & 0x3F == 0x1B:
            bit = ((w >> 31) << 5) | ((w >> 19) & 31)
            target = pc + (sx((w >> 5) & 0x3FFF, 14) << 2)
            return (f"if ((({reg(w & 31)} >> {bit}) & 1) {'!=' if (w >> 24) & 1 else '=='} 0) "
                    f"{self.goto(target)}"), True
        if w >> 24 == 0x54 and not (w >> 4) & 1:
            target = pc + (sx((w >> 5) & 0x7FFFF, 19) << 2)
            cond = w & 15
            if cond >= 14:
                return self.goto(target), False
            return f"if ({CONDS[cond]}) {self.goto(target)}", True
        if w & 0xFFE0001F == 0xD4000001:   # svc: handled by the native kernel, then execution continues
            return f"rt_svc(c, {(w >> 5) & 0xFFFF:#x});", True
        if w >> 24 == 0xD4:
            return f'rt_trap(c, {const(self.base + pc)}, "exception-generating instruction");', False
        if w & 0xFFFFF01F == 0xD503201F:   # hint space (yield, pac hints, ...)
            return ";", True
        if w & 0xFFFFF01F == 0xD503301F:   # barriers / clrex
            op2 = (w >> 5) & 7
            return ("ex_a = 0;" if op2 == 2 else "__atomic_thread_fence(__ATOMIC_SEQ_CST);"), True
        if w & 0xFFFFFC1F == 0xD63F0000:   # blr
            return self.call_indirect((w >> 5) & 31, pc + 4), True
        if w & 0xFFFFFC1F == 0xD65F0000:   # ret
            return self.ret((w >> 5) & 31), False
        if w & 0xFFFFFC1F == 0xD61F0000:   # br
            return self.indirect_jump((w >> 5) & 31), False
        if w >> 20 == 0xD53 or w >> 20 == 0xD51:
            return self.sysreg(w), True
        if w >> 19 == 0x1AA1:              # sys: cache maintenance
            if (w >> 5) & 0x3FFF == 0x1BA1:    # dc zva: zero the 64-byte block containing the address
                return f"memset((void *)({reg(w & 31)} & ~63ull), 0, 64);", True
            return ";", True               # cache flushes / invalidations mean nothing here
        raise Unsupported("system instruction")

    # Identification registers report a Cortex-A57-like core, as on the console.
    SYSREG_READ = {0x5E82: "c->tpidr", 0x5E83: "c->tpidrro", 0x5A20: "c->fpcr", 0x5A21: "c->fpsr",
                   0x5F01: "rt_ticks()", 0x5F02: "rt_ticks()", 0x5F00: "19200000ull",
                   0x5807: "4ull", 0x5801: "0x8444C004ull"}
    SYSREG_WRITE = {0x5E82: "c->tpidr", 0x5A20: "c->fpcr", 0x5A21: "c->fpsr"}

    def sysreg(self, w):
        key, rt, read = (w >> 5) & 0x7FFF, w & 31, (w >> 21) & 1
        field = (self.SYSREG_READ if read else self.SYSREG_WRITE).get(key)
        if field is None:
            raise Unsupported(f"system register {key:#x}")
        return wr(rt, 1, f"(u64){field}") if read else f"{field} = {reg(rt)};"

    def indirect_jump(self, rn):
        """`br xN`: a compiler jump table if analyze_tables() resolved one, else a tail call."""
        targets = self.table_at.get(self.i)
        if targets:
            self.tables += 1
            cases = " ".join(f"case {const(self.base +t)}: goto L_{t:x};" for t in sorted(targets))
            self.leaders |= targets
            return f'switch ({reg(rn)}) {{ {cases} default: rt_trap(c, {reg(rn)}, "jump table miss"); }}'
        pc = self.fstart + self.i * 4
        if pc in self.unresolved[-8:]:
            return f'rt_trap(c, {const(self.base +pc)}, "jump table could not be resolved statically");'
        self.indirect_tail += 1
        ws = self.words
        if rn == 17 and self.i == 3 and len(ws) == 4 and ws[0] & 0x9F00001F == 0x90000010 and ws[1] & 0xFFC003FF == 0xF9400211:
            # an import stub (adrp x16; ldr x17, [x16, #got]; add x16, x16, #got; br x17): its
            # target is the same every time, so it keeps it rather than looking it up per call
            return f"{{ static RtCache k_; GuestFn *t_ = rt_target({reg(rn)}, &k_); __attribute__((musttail)) return t_(c); }}"
        return f"{{ GuestFn *t_ = rt_lookup({reg(rn)}); __attribute__((musttail)) return t_(c); }}"

    def find_def(self, r, i):
        """Index of the nearest instruction before index i that writes xR, or None if
        straight-line control cannot be followed back that far."""
        ws = self.words
        for j in range(i - 1, max(i - 256, -1), -1):
            w = ws[j]
            if (w >> 25) & 0xE == 0xA and w >> 20 != 0xD53:   # branches, exceptions, system
                if w & 0xFC000000 == 0x14000000 or w & 0xFFFFFC1F in (0xD65F0000, 0xD61F0000):
                    return None                           # b / ret / br: nothing falls through
                if (w & 0xFC000000 == 0x94000000 or w & 0xFFFFFC1F == 0xD63F0000) and (r <= 18 or r == 30):
                    return None                           # calls clobber caller-saved registers
            elif r in gpr_defs(w):
                return j
        return None

    def analyze_tables(self):
        """Resolve each `br` of the current function that is a compiler jump-table dispatch:

            ldrsw off, [table, idx, lsl #2] ; add t, off, table ; br t

        Entries are 32-bit offsets from the table. The table address is often hoisted into a
        callee-saved register far from the dispatch, so it is found by walking definitions
        backwards over the function's direct control-flow edges: every definition reached
        must evaluate to the same link-time constant. Table edges themselves are not added to
        the graph - whatever lies behind a case block is the dispatch that jumped to it, which
        the walk reaches anyway. A table's length is bounded by the next table of the same
        function (the compiler lays them out back to back) and by its entries staying inside
        the function; an over-long table only adds cases that are never taken."""
        ws, n = self.words, len(self.words)
        self.table_at = {}
        brs = [i for i, w in enumerate(ws) if w & 0xFFFFFC1F == 0xD61F0000]
        if not brs:
            return
        falls, preds = [True] * n, {}
        for i, w in enumerate(ws):
            t = None
            if w & 0x7C000000 == 0x14000000:
                if not w >> 31:
                    falls[i], t = False, i + sx(w & 0x3FFFFFF, 26)
            elif w & 0x7E000000 == 0x34000000 or w & 0xFF000010 == 0x54000000:
                t = i + sx((w >> 5) & 0x7FFFF, 19)
            elif w & 0x7E000000 == 0x36000000:
                t = i + sx((w >> 5) & 0x3FFF, 14)
            elif w & 0xFFFFFC1F in (0xD65F0000, 0xD61F0000) or w >> 16 == 0 or w == 0xE7FFDEFE:
                falls[i] = False
            if t is not None and 0 <= t < n:
                preds.setdefault(t, []).append(i)

        def before(i):
            return ([i - 1] if i and falls[i - 1] else []) + preds.get(i, [])

        def reach(r, i, strict, depth=0):
            """Link-time constants xR can hold on entry to instruction i.

            Strict: None unless every definition reaching i is such a constant. Lenient: just
            the constants found, ignoring paths that end in a call clobber, a non-constant
            definition or the function entry - the graph has infeasible paths (e.g. the
            fall-through after a call that never returns), so callers must validate."""
            if depth > 3 or i == 0:
                return None if strict else set()
            values, seen, stack = set(), set(), before(i)
            while stack:
                j = stack.pop()
                if j in seen:
                    continue
                seen.add(j)
                w = ws[j]
                if (w >> 25) & 0xE == 0xA and w >> 20 != 0xD53:
                    if (w & 0xFC000000 == 0x94000000 or w & 0xFFFFFC1F == 0xD63F0000) and (r <= 18 or r == 30):
                        if strict:
                            return None
                        continue
                elif r in gpr_defs(w):
                    pc = self.fstart + j * 4
                    if w & 0x1F000000 == 0x10000000:              # adr / adrp
                        imm = sx((((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3), 21)
                        values.add((pc & ~0xFFF) + (imm << 12) if w >> 31 else pc + imm)
                        continue
                    if w & 0xFFC00000 == 0x91000000:              # add xR, xS, #imm
                        src, add = reach((w >> 5) & 31, j, strict, depth + 1), (w >> 10) & 0xFFF
                    elif w & 0xFFE0FFE0 == 0xAA0003E0:            # mov xR, xS
                        src, add = reach((w >> 16) & 31, j, strict, depth + 1), 0
                    else:
                        src = None
                    if strict and (src is None or len(src) != 1):
                        return None
                    values.update(v + add for v in src or ())
                    continue
                if j == 0:
                    if strict:
                        return None
                    continue
                stack += before(j)
            return values

        def entry(table, k):
            """Target of table entry k, if it is a plausible case label of this function."""
            at = table + 4 * k
            if table & 3 or at < 0 or at + 4 > len(self.img):
                return None
            t = table + struct.unpack_from("<i", self.img, at)[0]
            return t if self.fstart <= t < self.fend and not t & 3 else None

        def resolve(i, first):
            """The table address the dispatch at index i reads, or None."""
            j = self.find_def((ws[i] >> 5) & 31, i)
            if j is None or ws[j] & 0xFFE0FC00 != 0x8B000000:     # add xT, xA, xB
                return None
            r1, r2 = (ws[j] >> 5) & 31, (ws[j] >> 16) & 31
            for off, other in ((r1, r2), (r2, r1)):
                k = self.find_def(off, j)
                if k is None:
                    continue
                load = ws[k]
                if load & 0xFFE00C00 != 0xB8A00800 or not (load >> 12) & 1:
                    continue
                base = (load >> 5) & 31   # the table, as the load addressed it
                self.table_like += first
                looked.add(i)
                tables = reach(base, k, True)
                strict = bool(tables) and len(tables) == 1
                if not strict:
                    tables = {t for t in reach(base, k, False) if entry(t, 0) is not None}
                if len(tables) == 1:
                    table = next(iter(tables))
                    # The add's other operand must hold that same table address. Usually it is
                    # the load's base register itself; sometimes a copy made before the load
                    # overwrote the original.
                    again = self.find_def(other, j)
                    same = other == base and off != base and (again is None or again < k)
                    if not same:
                        held = reach(other, j, True)
                        if not held or len(held) != 1:
                            held = reach(other, j, False)
                        same = held == {table}
                    if same and entry(table, 0) is not None:
                        self.assumed += not strict
                        return table
                return None
            return None

        # Case blocks are reached only through their table, so a dispatch inside a case of
        # another table cannot see the definitions before that table until its edges exist:
        # resolve in passes, adding the edges of each table found to the graph.
        bases, pending, looked = {}, list(brs), set()
        for attempt in range(6):
            found = {}
            for i in pending:
                table = resolve(i, attempt == 0)
                if table is not None:
                    found[i] = table
            if not found:
                break
            bases.update(found)
            pending = [i for i in pending if i not in found]
            starts = sorted(set(bases.values()))
            for i, table in found.items():
                nxt = bisect.bisect_right(starts, table)
                limit = min(starts[nxt] if nxt < len(starts) else len(self.img), table + 4 * 4096)
                for e in range((limit - table) // 4):
                    t = entry(table, e)
                    if t is None:
                        break
                    ti = (t - self.fstart) // 4
                    if 0 <= ti < n:
                        preds.setdefault(ti, []).append(i)
            if not pending:
                break
        for i in pending:
            if i in looked:
                self.unresolved.append(self.fstart + i * 4)

        starts = sorted(set(bases.values()))
        for i, table in bases.items():
            nxt = bisect.bisect_right(starts, table)
            limit = min(starts[nxt] if nxt < len(starts) else len(self.img), len(self.img), table + 4 * 4096)
            targets = set()
            for e in range((limit - table) // 4):
                t = entry(table, e)
                if t is None:
                    break
                targets.add(t)
            self.table_at[i] = targets

    # ------------------------------------------------------------------ loads / stores
    def load_store(self, w, pc):
        group, vec = (w >> 27) & 7, (w >> 26) & 1
        size, rn, rt = w >> 30, (w >> 5) & 31, w & 31
        base = reg(rn, 1, sp=True)
        if group == 1:
            if vec:
                return self.simd_ldst(w, base)
            if (w >> 24) & 3:
                raise Unsupported("reserved load/store")
            return self.exclusive(w, size, base, rt)
        if group == 3 and not (w >> 24) & 3:   # literal
            addr = pc + (sx((w >> 5) & 0x7FFFF, 19) << 2)
            if vec:
                if size > 2:
                    raise Unsupported("reserved literal load")
                return f"ldv{32 << size}(c, {rt}, {const(self.base +addr)});"
            if size == 3:
                return ";"
            if not 0 <= addr <= len(self.img) - (8 if size == 1 else 4):   # data taken for code
                raise Unsupported("literal outside the module")
            v = struct.unpack_from("<I" if size == 0 else "<i" if size == 2 else "<Q", self.img, addr)[0]
            return wr(rt, 1, const(v))
        if group == 5:   # pair
            mode, load, rt2 = (w >> 23) & 3, (w >> 22) & 1, (w >> 10) & 31
            if vec:
                if size > 2:
                    raise Unsupported("reserved pair")
                bits, signed = 32 << size, False
            else:
                if size == 3 or (size == 1 and not load):
                    raise Unsupported("reserved pair")
                bits, signed = (64, False) if size == 2 else (32, size == 1)
            imm = sx((w >> 15) & 0x7F, 7) * (bits // 8)
            first = self.access(vec, load, bits, signed, 1, rt, "a_")
            second = self.access(vec, load, bits, signed, 1, rt2, f"a_ + {bits // 8}")
            if mode == 1:
                return f"{{ u64 a_ = {base}; {first} {second} {base} = a_ + {const(imm)}; }}"
            if mode == 3:
                return f"{{ u64 a_ = {base} + {const(imm)}; {first} {second} {base} = a_; }}"
            return f"{{ u64 a_ = {base} + {const(imm)}; {first} {second} }}"
        if group != 7:
            raise Unsupported("reserved load/store")

        opc = (w >> 22) & 3
        if vec:
            if opc >= 2 and size:
                raise Unsupported("reserved simd load/store")
            bits, load, signed, to64 = (128 if opc >= 2 else 8 << size), opc & 1, False, 1
        else:
            bits, load = 8 << size, opc != 0
            signed, to64 = opc >= 2, opc != 3
            if opc == 2 and size == 3:   # prfm
                return ";"
            if opc == 3 and size >= 2:
                raise Unsupported("reserved load/store")
        scale = bits.bit_length() - 4
        if (w >> 24) & 1:
            return self.access(vec, load, bits, signed, to64, rt, f"{base} + {const(((w >> 10) & 0xFFF) << scale)}")
        mode = (w >> 10) & 3
        if not (w >> 21) & 1:
            imm = const(sx((w >> 12) & 0x1FF, 9))
            if mode == 0:
                return self.access(vec, load, bits, signed, to64, rt, f"{base} + {imm}")
            if mode == 2:
                raise Unsupported("unprivileged load/store")
            act = self.access(vec, load, bits, signed, to64, rt, "a_")
            if mode == 1:
                return f"{{ u64 a_ = {base}; {act} {base} = a_ + {imm}; }}"
            return f"{{ u64 a_ = {base} + {imm}; {act} {base} = a_; }}"
        if mode != 2:
            raise Unsupported("atomic memory operation (LSE)" if mode == 0 else "pointer-auth load")
        option = (w >> 13) & 7
        if not option & 2:
            raise Unsupported("reserved register-offset extend")
        off = extended((w >> 16) & 31, option, scale if (w >> 12) & 1 else 0, 1)
        return self.access(vec, load, bits, signed, to64, rt, f"{base} + {off}")

    @staticmethod
    def access(vec, load, bits, signed, to64, rt, addr):
        if vec:
            return f"{'ldv' if load else 'stv'}{bits}(c, {rt}, {addr});"
        if not load:
            return f"st{bits}({addr}, (u{bits}){reg(rt)});"
        if not signed:
            return wr(rt, 1, f"ld{bits}({addr})")
        if bits == 64:
            return wr(rt, 1, f"ld64({addr})")
        return wr(rt, 1, f"({'u64)(i64' if to64 else 'u32)(i32'})(i{bits})ld{bits}({addr})")

    def exclusive(self, w, size, base, rt):
        o2, load, o1, rs = (w >> 23) & 1, (w >> 22) & 1, (w >> 21) & 1, (w >> 16) & 31
        bits = 8 << size
        if o1:
            raise Unsupported("exclusive pair / compare-and-swap")
        if o2:   # ldar / stlr: ordinary accesses with ordering guarantees
            if load:
                return wr(rt, 1, f"ld{bits}({base})")
            return f"sta{bits}({base}, (u{bits}){reg(rt)});"
        if load:
            return f"{{ ex_a = {base}; ex_v = ld{bits}(ex_a); {wr(rt, 1, 'ex_v')} }}"
        ok = f"({base} == ex_a && cas{bits}(ex_a, ex_v, {reg(rt)}))"
        return f"{{ u32 ok_ = {ok}; ex_a = 0; {wr(rs, 0, '!ok_')} }}"

    # ------------------------------------------------------------------ scalar floating point
    def fp(self, w):
        rm, rn, rd = (w >> 16) & 31, (w >> 5) & 31, w & 31
        ftype = (w >> 22) & 3
        if w & 0xFF000000 == 0x1F000000:   # fmadd family
            if ftype > 1:
                raise Unsupported("fp: half precision")
            p = "SD"[ftype]
            n, m, a = f"R{p}({rn})", f"R{p}({rm})", f"R{p}({(w >> 10) & 31})"
            o1, o0 = (w >> 21) & 1, (w >> 15) & 1
            if o1 != o0:
                n = f"-{n}"
            if o1:
                a = f"-{a}"
            return f"w{p}(c, {rd}, {'fmaf' if ftype == 0 else 'fma'}({n}, {m}, {a}));"
        if w & 0x7F200000 != 0x1E200000:
            if w & 0x7F200000 == 0x1E000000:
                return self.fp_fixed(w)
            return self.simd(w)
        sf = w >> 31
        if (w >> 10) & 0x3F == 0:
            return self.fp_int(w, sf, ftype, rn, rd)
        low = (w >> 10) & 0x3F
        if not sf and low & 0x1F == 0x10 and (ftype == 3 or (w >> 15) & 0x3F == 7):
            return self.fp_half(ftype, (w >> 15) & 0x3F, rn, rd)
        if sf or ftype > 1:
            raise Unsupported("fp: half precision" if ftype == 3 and not sf else "simd: reserved")
        p, f = "SD"[ftype], ("f32", "f64")[ftype]
        sfx = "f" if ftype == 0 else ""
        a, b = f"R{p}({rn})", f"R{p}({rm})"
        if low & 0x1F == 0x10:   # one-source
            opcode = (w >> 15) & 0x3F
            if opcode == 0:
                return f"w{p}(c, {rd}, {a});"
            simple = {1: f"fabs{sfx}({a})", 2: f"-{a}", 3: f"sqrt{sfx}({a})", 8: f"__builtin_roundeven{sfx}({a})",
                      9: f"ceil{sfx}({a})", 10: f"floor{sfx}({a})", 11: f"trunc{sfx}({a})",
                      12: f"round{sfx}({a})", 14: f"rint{sfx}({a})", 15: f"nearbyint{sfx}({a})"}
            if opcode in simple:
                return f"w{p}(c, {rd}, {simple[opcode]});"
            if opcode == 4 and ftype == 1:
                return f"wS(c, {rd}, (f32){a});"
            if opcode == 5 and ftype == 0:
                return f"wD(c, {rd}, (f64){a});"
            raise Unsupported("fp: one-source op")
        if low & 0xF == 8:       # compare
            if (w >> 14) & 3 or w & 7:
                raise Unsupported("reserved fp compare")
            other = "0" if (w >> 3) & 1 else b
            return (f"{{ {f} a_ = {a}, b_ = {other}; nf = a_ < b_; zf = a_ == b_; cf = !(a_ < b_); "
                    f"vf = (a_ != a_) | (b_ != b_); }}")
        if low & 7 == 4:         # immediate
            if w & 0x3E0:
                raise Unsupported("reserved fp immediate")
            return f"w{p}(c, {rd}, {fp_imm8((w >> 13) & 0xFF).hex()}{sfx});"
        cond = CONDS[(w >> 12) & 15]
        if low & 3 == 1:         # fccmp
            nzcv = w & 15
            return (f"if ({cond}) {{ {f} a_ = {a}, b_ = {b}; nf = a_ < b_; zf = a_ == b_; cf = !(a_ < b_); "
                    f"vf = (a_ != a_) | (b_ != b_); }} else {{ nf = {nzcv >> 3}; zf = {(nzcv >> 2) & 1}; "
                    f"cf = {(nzcv >> 1) & 1}; vf = {nzcv & 1}; }}")
        if low & 3 == 3:         # fcsel
            return f"w{p}(c, {rd}, {cond} ? {a} : {b});"
        if low & 3 != 2:
            raise Unsupported("reserved fp encoding")
        opcode = (w >> 12) & 15
        ops = {0: f"{a} * {b}", 1: f"{a} / {b}", 2: f"{a} + {b}", 3: f"{a} - {b}", 4: f"fmax{p}({a}, {b})",
               5: f"fmin{p}({a}, {b})", 6: f"fmaxnm{p}({a}, {b})", 7: f"fminnm{p}({a}, {b})", 8: f"-({a} * {b})"}
        if opcode not in ops:
            raise Unsupported("reserved fp two-source")
        return f"w{p}(c, {rd}, {ops[opcode]});"

    def fp_int(self, w, sf, ftype, rn, rd):
        rmode, opcode = (w >> 19) & 3, (w >> 16) & 7
        isz = "64" if sf else "32"
        if opcode in (6, 7):   # fmov between general and fp registers
            if rmode == 0 and (sf, ftype) in ((0, 0), (1, 1)):
                lane = "d[0]" if sf else "s[0]"
                if opcode == 6:
                    return wr(rd, 1, f"(u64)c->v[{rn}].{lane}")
                return f"{{ c->v[{rd}].d[0] = {reg(rn, sf)}; c->v[{rd}].d[1] = 0; }}"
            if rmode == 1 and sf and ftype == 2:
                return wr(rd, 1, f"c->v[{rn}].d[1]") if opcode == 6 else f"c->v[{rd}].d[1] = {reg(rn)};"
            raise Unsupported("fp: fmov variant")
        if ftype > 1:
            raise Unsupported("fp: half precision")
        p = "SD"[ftype]
        f = ("f32", "f64")[ftype]
        if rmode == 0 and opcode in (2, 3):
            src = f"({'i' if opcode == 2 else 'u'}{isz}){reg(rn, sf)}"
            return f"w{p}(c, {rd}, ({f}){src});"
        rounding = {(3, 0): "{}", (3, 1): "{}", (0, 0): "__builtin_roundeven({})", (0, 1): "__builtin_roundeven({})",
                    (1, 0): "ceil({})", (1, 1): "ceil({})", (2, 0): "floor({})", (2, 1): "floor({})",
                    (0, 4): "round({})", (0, 5): "round({})"}.get((rmode, opcode))
        if rounding is None:
            raise Unsupported("fp: conversion variant")
        fn = f"f2{'u' if opcode & 1 else 'i'}{isz}"
        return wr(rd, sf, f"({'u64' if sf else 'u32'}){fn}({rounding.format(f'(f64)R{p}({rn})')})")
