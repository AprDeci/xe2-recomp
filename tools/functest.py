#!/usr/bin/env python3
"""Whole-function differential test of the recompiler against unicorn.

Usage: functest.py <nso> [--functions N] [--states R] [--jobs J] [--max-insns M] [--show N]

Picks leaf functions (no calls out, so they can run in isolation), builds them exactly as
the production recompiler emits them, and runs each from R random states both natively and
under the reference emulator, comparing every register and all memory written at return.
This exercises what single-instruction tests cannot: labels, loops, jump tables, flag
lifetimes across instructions and stack frames. A share of the sample is reserved for
functions containing jump tables.

Memory is arranged so that pointers loaded from arguments usually point somewhere valid.
Runs the reference side cannot finish (fault, write to a global, too long) are skipped.
"""
import os
import random
import struct
import subprocess
import sys
import zlib
from collections import Counter
from multiprocessing import Pool

import cc
from a64 import sx
from difftest import nan_only, rand_v, rand_x
from funcs import discover
from lift import BASE, Lifter
from nso import Module

SCRATCH, SCRATCH_SIZE, STACK_TOP = 0x7200000000, 0x40000, 0x38000
DIRTY_KEEP = 1024
IN_FMT, OUT_FMT = "<II31QQ64Q", f"<31QQ64QQQQQ{DIRTY_KEEP}s"
OUT_SIZE = struct.calcsize(OUT_FMT)
_state = {}


def scratch_pointer(rng):
    return SCRATCH + 0x1000 + 16 * rng.randrange(0x1E00)


def make_scratch():
    """Heap-like region whose words are mostly pointers back into it, then a stack region."""
    rng = random.Random(11)
    out = bytearray()
    for _ in range(0x20000 // 8):
        r = rng.random()
        if r < 0.55:
            out += struct.pack("<Q", scratch_pointer(rng))
        elif r < 0.75:
            out += struct.pack("<II", rng.randrange(17), rng.randrange(17))
        elif r < 0.90:
            out += struct.pack("<ff", round(rng.uniform(-8, 8), 2), round(rng.uniform(-8, 8), 2))
        else:
            out += bytes(8)
    return bytes(out) + rng.randbytes(SCRATCH_SIZE - 0x20000)


def is_leaf(words, start, size):
    """No calls and no branches leaving the function (cheap pre-filter on raw words)."""
    for i, w in enumerate(words):
        if w & 0xFC000000 == 0x94000000 or w & 0xFFFFFC1F == 0xD63F0000:
            return False
        if w & 0xFC000000 == 0x14000000 and not 0 <= i + sx(w & 0x3FFFFFF, 26) < size // 4:
            return False
        if (w & 0x7E000000 == 0x34000000 or w & 0xFF000010 == 0x54000000) and \
                not 0 <= i + sx((w >> 5) & 0x7FFFF, 19) < size // 4:
            return False
        if w & 0x7E000000 == 0x36000000 and not 0 <= i + sx((w >> 5) & 0x3FFF, 14) < size // 4:
            return False
    return True


def _init(image_path):
    _state["image_path"] = image_path


def _run(args):
    from unicorn import Uc, UcError, UC_ARCH_ARM64, UC_HOOK_MEM_WRITE, UC_MODE_ARM, UC_PROT_EXEC, UC_PROT_READ
    from unicorn import arm64_const as A
    tests, states, seed, exe, scratch_path, workdir, max_insns = args
    with open(_state["image_path"], "rb") as f:
        image = f.read()
    with open(scratch_path, "rb") as f:
        pristine = f.read()
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(BASE, (len(image) + 0xFFF) & ~0xFFF, UC_PROT_READ | UC_PROT_EXEC)   # globals are read-only
    uc.mem_write(BASE, image)
    uc.mem_map(SCRATCH, SCRATCH_SIZE)
    uc.mem_write(SCRATCH, pristine)
    dirty = [None, 0]

    def on_write(_uc, _access, addr, size, _value, _user):
        if not SCRATCH <= addr <= SCRATCH + SCRATCH_SIZE - size:
            return   # a write elsewhere faults, and the run is skipped
        if dirty[0] is None or addr < dirty[0]:
            dirty[0] = addr
        if addr + size > dirty[1]:
            dirty[1] = addr + size

    uc.hook_add(UC_HOOK_MEM_WRITE, on_write)
    reg_ids = ([A.UC_ARM64_REG_X0 + i for i in range(29)] + [A.UC_ARM64_REG_X29, A.UC_ARM64_REG_X30,
               A.UC_ARM64_REG_SP] + [A.UC_ARM64_REG_V0 + i for i in range(32)])
    read_ids = reg_ids + [A.UC_ARM64_REG_PC]
    ret = BASE   # never executed: emulation stops on reaching it

    rng = random.Random(seed)
    expected, records, skipped = [], [], Counter()
    for idx, addr in tests:
        for _ in range(states):
            xs = [rand_x(rng) for _ in range(31)]
            for r in range(8):
                if rng.random() < 0.7:
                    xs[r] = scratch_pointer(rng)
            if rng.random() < 0.5:
                xs[8] = scratch_pointer(rng)
            xs[30] = ret
            sp = SCRATCH + STACK_TOP
            vs = [rand_v(rng) for _ in range(32)]
            flat = [h for lo_hi in vs for h in lo_hi]
            uc.reg_write_batch(list(zip(reg_ids, xs + [sp] + [lo | hi << 64 for lo, hi in vs])))
            dirty[0], dirty[1] = None, 0
            why = None
            try:
                uc.emu_start(BASE + addr, ret, count=max_insns)
            except UcError:
                why = "fault"
            got = uc.reg_read_batch(read_ids)
            mem = (0xFFFFFFFFFFFFFFFF, 0, 0, b"")
            if dirty[0] is not None:
                lo, hi = dirty[0] - SCRATCH, dirty[1] - SCRATCH
                now, was = bytes(uc.mem_read(dirty[0], hi - lo)), pristine[lo:hi]
                uc.mem_write(dirty[0], was)
                if now != was:
                    first = next(k for k in range(hi - lo) if now[k] != was[k])
                    last = next(k for k in range(hi - lo - 1, -1, -1) if now[k] != was[k])
                    a, b = (lo + first) & ~7, (lo + last + 8) & ~7   # whole 8-byte lanes, as the native side
                    full, origin = pristine[a:lo] + now + pristine[hi:b], min(a, lo)
                    region = full[a - origin:b - origin]
                    mem = (a, b - a, zlib.crc32(region), region[:DIRTY_KEEP])
            if why is None and got[64] != ret:
                why = "too long"
            if why:
                skipped[why] += 1
                continue
            ev = []
            for v in got[32:64]:
                ev += [v & 0xFFFFFFFFFFFFFFFF, v >> 64]
            expected.append((idx, list(got[:31]), got[31], ev, mem))
            records.append(struct.pack(IN_FMT, idx, 0, *xs, sp, *flat))

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

    bad, ran, nan_runs = [], Counter(), Counter()
    for k, (idx, ex, esp, ev, emem) in enumerate(expected):
        o = struct.unpack_from(OUT_FMT, raw, k * OUT_SIZE)
        gx, gsp, gv, gmem, fault = o[:31], o[31], o[32:96], (o[96], o[97], o[98], o[100]), o[99]
        ran[idx] += 1
        why = []
        if fault:
            why.append("native fault")
        else:
            why += [f"x{i}: want {ex[i]:#x} got {gx[i]:#x}" for i in range(31) if gx[i] != ex[i]]
            if gsp != esp:
                why.append(f"sp: want {esp:#x} got {gsp:#x}")
            why += [f"v{i // 2}.d[{i & 1}]: want {ev[i]:#018x} got {gv[i]:#018x}"
                    for i in range(64) if gv[i] != ev[i] and not nan_only(ev[i], gv[i])]
            if gmem[:2] != emem[:2]:
                why.append(f"memory extent: want +{emem[0]:#x} len {emem[1]}, got +{gmem[0]:#x} len {gmem[1]}")
            elif emem[1] and gmem[2] != emem[2]:
                keep = min(emem[1], DIRTY_KEEP)
                want = struct.unpack(f"<{keep // 8}Q", emem[3][:keep])
                got = struct.unpack(f"<{keep // 8}Q", gmem[3][:keep])
                lanes = [k for k in range(keep // 8) if want[k] != got[k] and not nan_only(want[k], got[k])]
                if lanes:
                    k = lanes[0]
                    why.append(f"memory +{emem[0] + 8 * k:#x}: want {want[k]:#018x} got {got[k]:#018x}")
                elif emem[1] > DIRTY_KEEP and want == got:
                    why.append(f"memory beyond the first {DIRTY_KEEP} bytes written differs")
                else:
                    nan_runs[idx] += 1
        if why:
            bad.append((idx, "; ".join(why[:3])))
    return ran, skipped, bad, nan_runs


def arg(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


def main():
    nso = sys.argv[1]
    want, states = int(arg("--functions", "3000")), int(arg("--states", "8"))
    jobs, max_insns = int(arg("--jobs", "8")), int(arg("--max-insns", "4000"))
    work = os.path.join(cc.ROOT, "build", "functest")
    os.makedirs(work, exist_ok=True)
    image_path = os.path.join(cc.ROOT, "dump", "modules", "main.reloc.bin")

    mod = Module(nso)
    found, sizes = discover(mod)
    funcs = [(a, sizes[a]) for a, s in found.items() if "eh" in s and 8 <= sizes[a] <= 4 * 1500]
    random.Random(5).shuffle(funcs)
    lifter = Lifter(mod.image, set(found))
    chosen, with_tables = [], 0
    for start, size in funcs:
        if len(chosen) >= want:
            break
        words = struct.unpack_from(f"<{size // 4}I", mod.image, start)
        if not is_leaf(words, start, size):
            continue
        has_br = any(w & 0xFFFFFC1F == 0xD61F0000 for w in words)
        if not has_br and len(chosen) - with_tables >= want * 2 // 3:
            continue   # keep a third of the sample for jump-table functions
        code, why, callees = lifter.lift_function(start, size)
        if why or callees or "rt_lookup" in code or "could not be resolved" in code:
            continue
        with_tables += "switch (" in code
        chosen.append((start, code))
    print(f"{len(chosen):,} leaf functions selected ({with_tables:,} with jump tables) x {states} states")

    sources = []
    for f0 in range(0, len(chosen), 400):
        path = os.path.join(work, f"funcs_{f0 // 400:03d}.c")
        with open(path, "w", newline="\n") as f:
            f.write('#include "recomp.h"\n\n' + "\n\n".join(code for _a, code in chosen[f0:f0 + 400]) + "\n")
        sources.append(path)
    table = os.path.join(work, "table.c")
    with open(table, "w", newline="\n") as f:
        f.write('#include "recomp.h"\n' + "".join(f"GuestFn f_{BASE + a:x};\n" for a, _c in chosen))
        f.write("const struct RtFunc { u64 addr; GuestFn *fn; } rt_funcs[] = {\n")
        f.write("".join(f"    {{0x{BASE + a:X}ull, f_{BASE + a:x}}},\n" for a, _c in chosen))
        f.write(f"}};\nconst u32 rt_func_count = {len(chosen)};\n")
    harness = os.path.join(work, "functest.obj")
    cc.compile_one(os.path.join(cc.RUNTIME, "functest.c"), harness)
    exe = cc.link(cc.compile_many(sources + [table], jobs) + [harness], os.path.join(work, "functest.exe"))

    scratch_path = os.path.join(work, "scratch.bin")
    with open(scratch_path, "wb") as f:
        f.write(make_scratch())
    indexed = [(i, a) for i, (a, _c) in enumerate(chosen)]
    step = max(1, len(indexed) // (jobs * 3))
    slices = [(indexed[i:i + step], states, 2000 + i, exe, scratch_path, work, max_insns)
              for i in range(0, len(indexed), step)]
    ran, skipped, bad, nan_runs = Counter(), Counter(), [], Counter()
    with Pool(jobs, _init, (image_path,)) as pool:
        for r, s, b, nr in pool.imap_unordered(_run, slices):
            ran.update(r)
            skipped.update(s)
            bad += b
            nan_runs.update(nr)

    total = sum(ran.values())
    tables_run = sum(1 for i in ran if "switch (" in chosen[i][1])
    print(f"compared {total:,} complete runs across {len(ran):,} functions ({tables_run:,} with jump tables); "
          f"skipped: " + (", ".join(f"{k} x{v:,}" for k, v in skipped.items()) or "none"))
    failing = sorted({i for i, _w in bad})
    print(f"functions with mismatches: {len(failing):,}   "
          f"(stored NaNs differing only in sign/payload: {sum(nan_runs.values())} runs in {len(nan_runs)} functions)")
    for i in failing[:int(arg("--show", "5"))]:
        addr = chosen[i][0]
        reasons = [w for j, w in bad if j == i]
        print(f"  f_{addr:08x} ({sizes[addr] // 4} instructions, {len(reasons)}/{ran[i]} runs differ): {reasons[0]}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
