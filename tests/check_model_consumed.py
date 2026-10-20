#!/usr/bin/env python3
"""t26 self-check: the model's 5 render targets ARE consumed by the scene composite,
and the dumped G-buffer surfaces actually carry real signal.

Background: t22 claimed "5-MRT sampled 0 times => missing deferred divide/lighting pass".
t25 §0 showed that claim compared guest RT addresses (0x740e...) against GPU-VA texture
addresses (0x22...), which are different address spaces and cannot be compared by number.
This check translates each model-MRT guest to the GPU VA a *sampling* draw actually uses
(the [t26] probe: `tex va <VA> -> guest <guest> ... surface guest <guest> ...`).

Two hard criteria (exit != 0 on ANY failure -- FAIL-CLOSED, never "inconclusive PASS"):
  C1 CONSUMPTION -- every one of the 5 model-MRT guest addresses is sampled by some draw
     through a translated GPU VA. (If the model MRTs were really never read, this fails.)
  C2 POPULATED   -- the 5 dumped surfaces are READ AND MEASURED; at least 2 of the 5 carry
     substantial real signal (nonblack>40% or max>150), i.e. the G-buffer was actually
     rendered. Per-surface fill is reported for all five.
     C2 FAILS (nonzero exit) if ANY required surface dump is missing, unreadable, or in an
     unsupported format -- "could not check" is NOT "pass".

Image reading uses a MINIMAL built-in BMP decoder (BI_RGB, 24/32-bit, either row order),
so NO external dependency (PIL/numpy/etc.) is required, and a missing dependency can never
silently turn C2 into a pass. A 1-bit/4-bit/8-bit or RLE-compressed BMP is rejected, not
guessed.

Usage: check_model_consumed.py <err.txt> <surf_dir>
Exit:  0 = both criteria verified PASS
       1 = at least one criterion FAILED or could not be verified
       2 = bad usage
"""
import sys
import re
import os
import glob
import struct

MODEL_GUESTS = ["740e5b0000", "740e970000", "740ed30000", "740f0f0000", "740f4b0000"]


def read_bmp_metrics(path):
    """Minimal BMP reader that computes per-channel stats in ONE pass (no pixel list).

    Supports uncompressed BI_RGB BMPs with 24 or 32 bits per pixel, either row order.
    Rejects anything else rather than guess. Returns a metrics dict, or raises ValueError.
    """
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 54 or data[0:2] != b"BM":
        raise ValueError("not a BMP (bad signature or truncated header)")
    bf_off = struct.unpack_from("<I", data, 10)[0]
    hdr_size = struct.unpack_from("<I", data, 14)[0]
    if hdr_size < 40:
        raise ValueError(f"unsupported BMP header size {hdr_size}")
    w = struct.unpack_from("<i", data, 18)[0]
    h = struct.unpack_from("<i", data, 22)[0]
    bpp = struct.unpack_from("<H", data, 28)[0]
    compression = struct.unpack_from("<I", data, 30)[0]
    if bpp not in (24, 32):
        raise ValueError(f"unsupported bit depth {bpp} (only 24/32-bit BI_RGB)")
    if compression != 0:
        raise ValueError(f"unsupported compression {compression} (only BI_RGB=0)")
    if w <= 0 or h == 0:
        raise ValueError(f"bad dimensions {w}x{h}")
    height = abs(h)
    nbytes = bpp // 8
    row_stride = ((w * nbytes + 3) // 4) * 4
    need = bf_off + row_stride * height
    if len(data) < need:
        raise ValueError(f"truncated pixel data: have {len(data)}, need {need}")

    n = w * height
    sr = sg = sb = 0
    maxr = maxg = maxb = 0
    nonblack = 0
    mv = memoryview(data)
    for row in range(height):
        base = bf_off + row * row_stride
        for col in range(w):
            p = base + col * nbytes
            b = mv[p]
            g = mv[p + 1]
            r = mv[p + 2]
            sr += r; sg += g; sb += b
            if r > maxr: maxr = r
            if g > maxg: maxg = g
            if b > maxb: maxb = b
            if (r + g + b) > 20:
                nonblack += 1
    if n == 0:
        raise ValueError("zero pixels")
    maxc = max(maxr, maxg, maxb)
    return {
        "w": w, "h": height,
        "mean": (round(sr / n, 1), round(sg / n, 1), round(sb / n, 1)),
        "max": (maxr, maxg, maxb),
        "maxc": maxc,
        "nonblack_pct": round(100.0 * nonblack / n, 1),
    }


def main():
    if len(sys.argv) < 3:
        print("usage: check_model_consumed.py <err.txt> <surf_dir>")
        return 2
    err, surf_dir = sys.argv[1], sys.argv[2]
    if not os.path.isfile(err):
        print(f"FAIL: err log not found: {err}")
        return 1
    with open(err, "r", errors="ignore") as source:
        text = source.read()

    # ---- C1: consumption via alias GPU VA ----
    sampled = {}
    pat = (r"\[t26\] tex va (0x[0-9a-fA-F]+) -> guest (0x[0-9a-fA-F]+) \| TIC \d+x\d+ "
           r"fmt (0x[0-9a-fA-F]+) type \d+ \| surface guest (0x[0-9a-fA-F]+) \d+x\d+ "
           r"fmt (0x[0-9a-fA-F]+) depth \d+ \| (\S+)")
    for m in re.finditer(pat, text):
        va, guest, _tf, _sg, _sf, _status = m.groups()
        sampled.setdefault(guest[2:].lower(), []).append(va)

    fail = []
    print("== C1 CONSUMPTION: each model MRT sampled through an alias GPU VA ==")
    for g in MODEL_GUESTS:
        hits = sampled.get(g)
        if not hits:
            fail.append(f"C1 {g}: never sampled by any draw (no [t26] alias line)")
            print(f"  {g}: FAIL - not consumed")
        else:
            print(f"  {g}: consumed via VA(s) {', '.join(sorted(set(hits)))}")

    # ---- C2: surfaces read + measured (FAIL-CLOSED) ----
    print("== C2 POPULATED: G-buffer surfaces carry real signal (decoded from BMP) ==")
    if not os.path.isdir(surf_dir):
        fail.append(f"C2: surface dir not found: {surf_dir}")
        print(f"  FAIL - surface dir not found: {surf_dir}")
    else:
        strong = 0
        for g in MODEL_GUESTS:
            fs = sorted(glob.glob(os.path.join(surf_dir, f"surf_000_{g}_*.bmp")))
            if not fs:
                fail.append(f"C2 {g}: no surface dump in {surf_dir} (cannot verify)")
                print(f"  {g}: FAIL - no surface dump (cannot verify)")
                continue
            try:
                met = read_bmp_metrics(fs[0])
            except (ValueError, OSError) as e:
                fail.append(f"C2 {g}: cannot read {os.path.basename(fs[0])}: {e}")
                print(f"  {g}: FAIL - cannot read {os.path.basename(fs[0])}: {e}")
                continue
            is_strong = met["nonblack_pct"] > 40.0 or met["maxc"] > 150
            strong += 1 if is_strong else 0
            print(f"  {g}: {met['w']}x{met['h']} mean {met['mean']} max {met['max']} "
                  f"nonblack% {met['nonblack_pct']} -> {'strong' if is_strong else 'sparse/aux'}")
        if strong < 2:
            fail.append(f"C2: only {strong} of 5 G-buffer surfaces carry real signal (need >=2)")

    print()
    if fail:
        print("FAIL:")
        for f in fail:
            print("  -", f)
        return 1
    print("PASS: all five model MRTs consumed via alias GPU VAs; G-buffer surfaces measured and populated.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
