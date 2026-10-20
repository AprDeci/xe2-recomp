#!/usr/bin/env python3
"""Differential test of the lifter against a reference AArch64 emulator (unicorn).

Usage: difftest.py <nso> [--per-template K] [--states R] [--jobs N] [--only <mnemonic,...>] [--show N]

Every instruction in the module is lifted; the resulting C statements are grouped by
"template" (the statement with numbers abstracted away), and K distinct real encodings
per template are executed from R random CPU states both natively (lifted C compiled to
x64) and under unicorn. All registers, flags, the next PC and memory writes must match.
"""
import os
import random
import re
import struct
import subprocess
import sys
from collections import Counter, defaultdict
from multiprocessing import Pool

import cc
import recomp
from lift import BASE, Lifter, Unsupported, const, reg

SCRATCH, SCRATCH_SIZE = 0x7200000000, 0x20000
NUM = re.compile(r"(?<![A-Za-z_])(?:0x[0-9A-Fa-f]+(?:ull|u)?|\d+)")
VEC_NUM = re.compile(r"(?<=c->v\[)\d+|(?<=X\()\d+|0x[0-9A-Fa-f]+(?:ull|u)?")
IN_FMT, OUT_FMT = "<II31QQQ64Q", "<31QQQ64QQQQ64sQ"


def template(stmt):
    """Statement shape used to bucket encodings. Vector statements keep lane counts, lane
    indices and shift amounts, since those select different behaviour, not just operands."""
    return (VEC_NUM if "c->v[" in stmt or "rt_" in stmt else NUM).sub("#", stmt)


OUT_SIZE = struct.calcsize(OUT_FMT)
EDGE = [0, 1, 2, 3, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF, 0x10000, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF,
        0x100000000, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0xFFFFFFFFFFFFFFFF, 0xFFFFFFFF00000000,
        0xFFFFFFFF80000000, 0x5555555555555555, 0xAAAAAAAAAAAAAAAA]
F32_SPECIAL = [0.0, -0.0, 1.0, -1.0, 0.5, 2.0, 1e-40, 3.4e38, float("inf"), float("-inf"), float("nan"), 1e10, -1e10,
               2147483648.0, -2147483904.0, 4294967296.0, 0.49999997, 1.5, 2.5, -0.5, -1.5, -2.5, 16777216.0]


class TestLifter(Lifter):
    """Control transfers record the next guest PC instead of transferring."""

    def goto(self, target):
        return f"{{ t_pc = {const(self.base + target)}; goto T_done; }}"

    def call(self, target, ret):
        return f"{{ X(30) = {const(self.base + ret)}; t_pc = {const(self.base + target)}; goto T_done; }}"

    def call_indirect(self, rn, ret):
        return f"{{ t_pc = {reg(rn)}; X(30) = {const(self.base + ret)}; goto T_done; }}"

    def ret(self, rn):
        return f"{{ t_pc = {reg(rn)}; goto T_done; }}"

    def indirect_jump(self, rn):
        return f"{{ t_pc = {reg(rn)}; goto T_done; }}"

    def snippet(self, start, n):
        stmts = []
        for i in range(n):
            pc = start + i * 4
            stmt, _falls = self.insn(struct.unpack_from("<I", self.img, pc)[0], pc)
            if any(s in stmt for s in ("rt_trap", "c->tpidr", "c->fp", "rt_svc", "rt_ticks", "memset(")):
                raise Unsupported("not testable in isolation")
            if "T_done" in stmt and i != n - 1:
                raise Unsupported("control flow inside snippet")
            stmts.append(stmt)
        return stmts


def mem_regs(w):
    """(base, index) register numbers an instruction uses for addressing, if any."""
    if (w >> 25) & 5 != 4:
        return None, None
    group = (w >> 27) & 7
    if group == 3 and not (w >> 24) & 3:
        return None, None
    index = None
    if group == 7 and not (w >> 24) & 1 and (w >> 21) & 1 and (w >> 10) & 3 == 2:
        index = (w >> 16) & 31
    return (w >> 5) & 31, index


# ---------------------------------------------------------------------------- sampling
_state = {}


def _init(image_path, text_size):
    with open(image_path, "rb") as f:
        _state["img"] = f.read(text_size)
    _state["image_path"] = image_path


def _sample(args):
    funcs, per_template = args
    img = _state["img"]
    lifter = TestLifter(img, set())
    rng = random.Random(funcs[0][0])
    seen = {}
    for start, size in funcs:
        for i, (w,) in enumerate(struct.iter_unpack("<I", img[start:start + size])):
            pc = start + i * 4
            try:
                stmt = lifter.snippet(pc, 1)[0]
            except Unsupported:
                continue
            if stmt == ";" or "cas" in stmt:   # nothing to observe / lone store-exclusive
                continue
            slot = seen.setdefault(template(stmt), [0, {}])
            slot[0] += 1
            if w in slot[1]:
                continue
            if len(slot[1]) < per_template:
                slot[1][w] = pc
            elif rng.randrange(slot[0]) < per_template:
                del slot[1][rng.choice(list(slot[1]))]
                slot[1][w] = pc
    return {k: (n, list(d.items())) for k, (n, d) in seen.items()}


def exclusive_snippets(img, funcs, limit):
    """Straight-line load-exclusive ... store-exclusive sequences, to test the success path."""
    out = []
    for start, size in funcs:
        words = struct.unpack_from(f"<{size // 4}I", img, start)
        for i, w in enumerate(words):
            if w & 0x3FE08000 == 0x08408000 or w & 0x3FE08000 == 0x08400000:   # ldaxr / ldxr
                for n in range(2, 5):
                    if i + n > len(words):
                        break
                    last = words[i + n - 1]
                    if last & 0x3FE00000 == 0x08000000 and (last >> 5) & 31 == (w >> 5) & 31:
                        if all((x >> 26) & 0x1F not in (5, 13, 21) and x >> 24 != 0x54 for x in words[i + 1:i + n - 1]):
                            out.append((start + i * 4, n))
                        break
                if len(out) >= limit:
                    return out
    return out


# ---------------------------------------------------------------------------- execution
def rand_x(rng):
    r = rng.random()
    if r < 0.45:
        return rng.getrandbits(64)
    if r < 0.65:
        return rng.choice(EDGE)
    if r < 0.85:
        return rng.getrandbits(8)
    return rng.getrandbits(32) | (0xFFFFFFFF00000000 if rng.getrandbits(1) else 0)


def rand_v(rng):
    r = rng.random()
    if r < 0.35:
        return struct.unpack("<QQ", struct.pack("<4f", *(round(rng.uniform(-64, 64), rng.randrange(4)) for _ in range(4))))
    if r < 0.55:
        return struct.unpack("<QQ", struct.pack("<2d", *(round(rng.uniform(-64, 64), rng.randrange(4)) for _ in range(2))))
    if r < 0.70:
        return struct.unpack("<QQ", struct.pack("<4f", *(rng.choice(F32_SPECIAL) for _ in range(4))))
    if r < 0.80:
        return struct.unpack("<QQ", struct.pack("<2d", *(rng.choice(F32_SPECIAL) for _ in range(2))))
    if r < 0.86:
        return rng.choice(EDGE), rng.choice(EDGE)
    if r < 0.91:   # small signed 32-bit lanes
        return struct.unpack("<QQ", struct.pack("<4i", *(rng.randrange(-40, 41) for _ in range(4))))
    if r < 0.95:   # small bytes: shift counts, table indices
        return struct.unpack("<QQ", bytes(rng.randrange(-9, 40) & 0xFF for _ in range(16)))
    return rng.getrandbits(64), rng.getrandbits(64)


def is_nan32(v):
    return v & 0x7F800000 == 0x7F800000 and v & 0x7FFFFF


def is_nan64(v):
    return v & 0x7FF0000000000000 == 0x7FF0000000000000 and v & 0xFFFFFFFFFFFFF


def nan_only(a, b):
    """True if two 64-bit values differ only in lanes that are NaN on both sides."""
    if all(x == y or (is_nan32(x) and is_nan32(y))
           for x, y in ((a & 0xFFFFFFFF, b & 0xFFFFFFFF), (a >> 32, b >> 32))):
        return True
    return bool(is_nan64(a) and is_nan64(b))


def _run(args):
    from unicorn import Uc, UcError, UC_ARCH_ARM64, UC_HOOK_MEM_WRITE, UC_MODE_ARM, UC_PROT_EXEC, UC_PROT_READ
    from unicorn import arm64_const as A
    tests, states, seed, exe, scratch_path, workdir = args
    img = _state["img"]
    with open(scratch_path, "rb") as f:
        pristine = f.read()
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(BASE, (len(img) + 0xFFF) & ~0xFFF, UC_PROT_READ | UC_PROT_EXEC)
    uc.mem_write(BASE, img)
    uc.mem_map(SCRATCH, SCRATCH_SIZE)
    uc.mem_write(SCRATCH, pristine)
    dirty = [None, 0]

    def on_write(_uc, _access, addr, size, _value, _user):
        if dirty[0] is None or addr < dirty[0]:
            dirty[0] = addr
        if addr + size > dirty[1]:
            dirty[1] = addr + size

    uc.hook_add(UC_HOOK_MEM_WRITE, on_write)
    reg_ids = ([A.UC_ARM64_REG_X0 + i for i in range(29)] + [A.UC_ARM64_REG_X29, A.UC_ARM64_REG_X30,
               A.UC_ARM64_REG_SP, A.UC_ARM64_REG_NZCV] + [A.UC_ARM64_REG_V0 + i for i in range(32)])
    read_ids = reg_ids + [A.UC_ARM64_REG_PC]

    rng = random.Random(seed)
    expected, records, skipped = [], [], []
    for idx, addr, n in tests:
        words = struct.unpack_from(f"<{n}I", img, addr)
        bases, indexes = set(), set()
        for w in words:
            b, i = mem_regs(w)
            if b is not None:
                bases.add(b)
            if i is not None:
                indexes.add(i)
        if bases & indexes:
            continue
        # br / blr / ret: give the target register a real code address
        code = {(w >> 5) & 31 for w in words if w & 0xFF9FFC1F == 0xD61F0000}
        for _ in range(states):
            xs = [rand_x(rng) for _ in range(31)]
            if rng.random() < 0.5:   # correlated operands: equal and off-by-one values
                pivot = rand_x(rng)
                for _k in range(8):
                    xs[rng.randrange(31)] = (pivot + rng.choice((0, 0, 1, -1))) & 0xFFFFFFFFFFFFFFFF
            for r in code:
                if r != 31:
                    xs[r] = BASE + 4 * rng.randrange(len(img) // 4)
            sp = SCRATCH + 0x8000 + 16 * rng.randrange(256)
            for b in bases:
                if b != 31:
                    xs[b] = SCRATCH + 0x8000 + 16 * rng.randrange(256)
            for i in indexes:
                if i != 31:
                    xs[i] = rng.randrange(64)
            nzcv = rng.getrandbits(4)
            vs = [rand_v(rng) for _ in range(32)]
            if rng.random() < 0.4:   # equal vectors, for compares / min / max
                pivot = rand_v(rng)
                for _k in range(6):
                    vs[rng.randrange(32)] = pivot
            flat = [h for lo_hi in vs for h in lo_hi]
            uc.reg_write_batch(list(zip(reg_ids, xs + [sp, nzcv << 28] + [lo | hi << 64 for lo, hi in vs])))
            dirty[0], dirty[1] = None, 0
            try:
                uc.emu_start(BASE + addr, 0, count=n)
            except UcError:
                skipped.append(idx)
                if dirty[0] is not None:
                    uc.mem_write(dirty[0], pristine[dirty[0] - SCRATCH:dirty[1] - SCRATCH])
                continue
            got = uc.reg_read_batch(read_ids)
            mem = (None, 0, b"")
            if dirty[0] is not None:
                lo, hi = dirty[0] - SCRATCH, dirty[1] - SCRATCH
                now, was = bytes(uc.mem_read(dirty[0], hi - lo)), pristine[lo:hi]
                uc.mem_write(dirty[0], was)
                diff = [k for k in range(hi - lo) if now[k] != was[k]]
                if diff:
                    mem = (lo + diff[0], diff[-1] - diff[0] + 1, now[diff[0]:diff[-1] + 1][:64])
            ev = []
            for v in got[33:65]:
                ev += [v & 0xFFFFFFFFFFFFFFFF, v >> 64]
            expected.append((idx, list(got[:31]), got[31], got[32] >> 28, ev, got[65], mem,
                             (xs, sp, nzcv, flat)))
            records.append(struct.pack(IN_FMT, idx, 0, *xs, sp, nzcv, *flat))

    tag = f"{os.getpid()}-{seed}"
    in_path, out_path = os.path.join(workdir, f"in-{tag}.bin"), os.path.join(workdir, f"out-{tag}.bin")
    with open(in_path, "wb") as f:
        f.write(b"".join(records))
    r = subprocess.run([exe, _state["image_path"], scratch_path, in_path, out_path], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f"native harness failed ({r.returncode}): {r.stderr[:500]}")
    with open(out_path, "rb") as f:
        raw = f.read()
    os.remove(in_path)
    os.remove(out_path)

    hard, soft, faults = [], Counter(), 0
    for k, (idx, ex, esp, enzcv, ev, epc, emem, inputs) in enumerate(expected):
        o = struct.unpack_from(OUT_FMT, raw, k * OUT_SIZE)
        gx, gsp, gnzcv, gv, gpc, goff, glen, gbytes, fault = o[:31], o[31], o[32], o[33:97], o[97], o[98], o[99], o[100], o[101]
        if fault:
            faults += 1
            hard.append((idx, "native fault", "", inputs))
            continue
        why = []
        for i in range(31):
            if gx[i] != ex[i]:
                why.append(f"x{i}: want {ex[i]:#x} got {gx[i]:#x}")
        if gsp != esp:
            why.append(f"sp: want {esp:#x} got {gsp:#x}")
        if gnzcv != enzcv:
            why.append(f"nzcv: want {enzcv:04b} got {gnzcv:04b}")
        if gpc != epc:
            why.append(f"pc: want {epc:#x} got {gpc:#x}")
        nan = False
        for i in range(64):
            if gv[i] != ev[i]:
                if nan_only(ev[i], gv[i]):
                    nan = True
                else:
                    why.append(f"v{i // 2}.d[{i & 1}]: want {ev[i]:#018x} got {gv[i]:#018x}")
        gmem = (None, 0, b"") if goff == 0xFFFFFFFFFFFFFFFF else (goff, glen, gbytes[:min(glen, 64)])
        if gmem != emem:
            why.append(f"mem: want {emem[0]}+{emem[1]} {emem[2].hex()} got {gmem[0]}+{gmem[1]} {gmem[2].hex()}")
        if why:
            hard.append((idx, why[0], "; ".join(why[1:4]), inputs))
        elif nan:
            soft[idx] += 1
    return len(expected), skipped, faults, hard, soft


# ---------------------------------------------------------------------------- driver
def arg(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


def main():
    import capstone
    nso = sys.argv[1]
    per_template, states = int(arg("--per-template", "4")), int(arg("--states", "16"))
    jobs = int(arg("--jobs", "16"))
    only = set(filter(None, arg("--only", "").split(",")))
    work = os.path.join(cc.ROOT, "build", "difftest")
    os.makedirs(work, exist_ok=True)

    # Any module can be tested: instructions are lifted for the default base and only the
    # text is consulted, so the unrelocated image serves both sides.
    mod, found, funcs = recomp.load(nso)
    image_path = recomp.image_file(mod)
    sizes = dict(funcs)
    img = bytes(mod.image[:mod.text.end])
    cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)

    def dis(addr, n=1):
        return "; ".join(f"{m} {o}" for _a, _s, m, o in cs.disasm_lite(img[addr:addr + 4 * n], addr))

    chunks = [(funcs[i:i + 500], per_template) for i in range(0, len(funcs), 500)]
    merged = {}
    with Pool(jobs, _init, (image_path, mod.text.end)) as pool:
        for part in pool.imap_unordered(_sample, chunks):
            for key, (n, items) in part.items():
                slot = merged.setdefault(key, [0, {}])
                slot[0] += n
                slot[1].update(items)
        rng = random.Random(1)
        tests = []
        for key in sorted(merged):
            items = sorted(merged[key][1].items())
            for w, addr in rng.sample(items, min(per_template, len(items))):
                tests.append((addr, 1))
        lifter = TestLifter(img, set())
        for addr, n in exclusive_snippets(img, funcs, 200):
            try:
                lifter.snippet(addr, n)
                tests.append((addr, n))
            except Unsupported:
                pass
        if only:
            tests = [t for t in tests if dis(t[0]).split()[0] in only]
        print(f"{len(merged):,} statement templates -> {len(tests):,} test encodings x {states} states")

        per_file = 2000
        sources = []
        for f0 in range(0, len(tests), per_file):
            path = os.path.join(work, f"tests_{f0 // per_file:03d}.c")
            with open(path, "w") as f:
                f.write('#include "recomp.h"\ntypedef struct TestIO { u32 n, z, c, v; u64 pc; } TestIO;\n')
                for i in range(f0, min(f0 + per_file, len(tests))):
                    addr, n = tests[i]
                    f.write(f"void t_{i}(Cpu *c, TestIO *t) {{\n"
                            f"    u32 nf = t->n, zf = t->z, cf = t->c, vf = t->v; u64 ex_a = 0, ex_v = 0;"
                            f" u64 t_pc = {const(BASE + addr + 4 * n)};\n")
                    for stmt in lifter.snippet(addr, n):
                        f.write(f"    {stmt}\n")
                    f.write("T_done:\n    t->n = nf; t->z = zf; t->c = cf; t->v = vf; t->pc = t_pc;\n}\n")
            sources.append(path)
        table = os.path.join(work, "table.c")
        with open(table, "w") as f:
            f.write('#include "recomp.h"\ntypedef struct TestIO TestIO;\ntypedef void TestFn(Cpu *, TestIO *);\n')
            f.write("".join(f"TestFn t_{i};\n" for i in range(len(tests))))
            f.write("TestFn *const t_funcs[] = {\n" + "".join(f"    t_{i},\n" for i in range(len(tests))) + "};\n")
            f.write(f"const u32 t_count = {len(tests)};\n")
        objs = cc.compile_many(sources + [table, os.path.join(cc.RUNTIME, "difftest.c")], jobs)
        # difftest.obj lands next to its source; keep build products out of runtime/
        moved = os.path.join(work, "difftest.obj")
        os.replace(objs[-1], moved)
        exe = cc.link(objs[:-1] + [moved], os.path.join(work, "difftest.exe"))

        scratch_path = os.path.join(work, "scratch.bin")
        with open(scratch_path, "wb") as f:
            f.write(random.Random(7).randbytes(SCRATCH_SIZE))

        indexed = [(i, a, n) for i, (a, n) in enumerate(tests)]
        step = max(1, len(indexed) // (jobs * 4))
        slices = [(indexed[i:i + step], states, 1000 + i, exe, scratch_path, work) for i in range(0, len(indexed), step)]
        total = faults = 0
        skipped, hard, soft = [], [], Counter()
        for r in pool.imap_unordered(_run, slices):
            total += r[0]; skipped += r[1]; faults += r[2]
            hard += r[3]; soft.update(r[4])

    bad_tests = {h[0] for h in hard}
    print(f"ran {total:,} state comparisons; reference faulted on {len(skipped):,} (skipped); native faults: {faults:,}")
    if skipped:
        kinds = Counter(dis(tests[i][0]).split()[0] for i in skipped)
        print("  reference faults by mnemonic: " + ", ".join(f"{m} x{n}" for m, n in kinds.most_common(8)))
    print(f"encodings with mismatches: {len(bad_tests):,} / {len(tests):,}   "
          f"(NaN-payload-only differences on {len(soft):,} more)")
    by_mnem = defaultdict(list)
    for h in hard:
        by_mnem[dis(tests[h[0]][0]).split()[0]].append(h)
    show = int(arg("--show", "2"))
    for m, items in sorted(by_mnem.items(), key=lambda kv: -len({h[0] for h in kv[1]})):
        print(f"\n{m}: {len({h[0] for h in items})} encodings, {len(items)} failing states")
        shown = set()
        for idx, why, more, inputs in items:
            if idx in shown or len(shown) >= show:
                continue
            shown.add(idx)
            addr, n = tests[idx]
            print(f"  {addr:#x}  {dis(addr, n)}")
            for stmt in lifter.snippet(addr, n):
                print(f"      C: {stmt}")
            print(f"      {why}" + (f"; {more}" if more else ""))
    return 1 if hard else 0


if __name__ == "__main__":
    sys.exit(main())
