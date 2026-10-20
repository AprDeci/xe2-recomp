"""LUT 3D self-check: check corresponding R/G/B channel ranges along the three input axes.
Cross-channel variation is reported but is not an independent assertion.

Input: the per-texel dump written by gpu_d3d11.c when SSBU_T24_SELFCHECK is set
(<dir>/lut3d_asm_<fmt>_<w>x<h>x<d>.txt, one "R G B A" line per texel in v[z][y][x] order).

A correct neutral grading volume sampled at input (r,g,b):
  - the red output rises with r  (x axis through the volume centre),
  - the green output rises with g (y axis),
  - the blue output rises with b  (z axis).
If the z (blue) axis collapsed - e.g. a 2D SRV bound to a Texture3D sampler - "B out vs B in"
is flat, which is exactly the green/yellow tint bug. This script exits non-zero on any flat axis,
so it is a fail-able test, not a rubber stamp.

Usage: check_lut3d.py <dump.txt> [<dump2.txt> ...]
"""
import sys
from pathlib import Path


def load(path):
    vals = []
    for line in Path(path).read_text().splitlines():
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        p = line.split()
        vals.append(tuple(int(v) for v in p[:4]))
    return vals


def rng(xs):
    return max(xs) - min(xs)


def check(path):
    v = load(path)
    # recover dimensions from the header comment, else from the file name (<w>x<h>x<d>)
    w = h = d = None
    import re, os
    for line in Path(path).read_text().splitlines():
        if line.startswith('#') and 'x' in line:
            m = re.search(r'(\d+)x(\d+)x(\d+)', line)
            if m:
                w, h, d = (int(g) for g in m.groups())
                break
    if not w:
        m = re.search(r'(\d+)x(\d+)x(\d+)', os.path.basename(path))
        if m:
            w, h, d = (int(g) for g in m.groups())
    if not w:
        print(f"ERR {path}: cannot read dimensions")
        return False
    n = w * h * d
    if len(v) != n:
        print(f"ERR {path}: {len(v)} texels, expected {n} ({w}x{h}x{d})")
        return False
    def at(z, y, x):
        return v[(z * h + y) * w + x]
    cz, cy, cx = d // 2, h // 2, w // 2
    # output channel vs its own input axis, along the axis through the volume centre
    var_r = [at(cz, cy, x)[0] for x in range(w)]   # Red out vs Red in  (x)
    var_g = [at(cz, y, cx)[1] for y in range(h)]   # Green out vs Green in (y)
    var_b = [at(z, cy, cx)[2] for z in range(d)]   # Blue out vs Blue in (z)
    cross_rb = [at(z, cy, cx)[0] for z in range(d)]  # Red out vs Blue in (should be ~flat for neutral)
    print(f"== {path}  ({w}x{h}x{d})")
    print(f"  R out vs R in (x axis): range {rng(var_r):3d}  {var_r}")
    print(f"  G out vs G in (y axis): range {rng(var_g):3d}  {var_g}")
    print(f"  B out vs B in (z axis): range {rng(var_b):3d}  {var_b}")
    print(f"  R out vs B in (cross) : range {rng(cross_rb):3d}  (expect small)")
    ok = rng(var_r) > 30 and rng(var_g) > 30 and rng(var_b) > 30
    print(f"  -> {'PASS' if ok else 'FAIL'}: corresponding channel ranges exceed 30 on all three axes")
    return ok


def main():
    if len(sys.argv) < 2:
        print("usage: check_lut3d.py <dump.txt> [...]")
        return 2
    allok = True
    for p in sys.argv[1:]:
        try:
            allok &= check(p)
        except Exception as e:
            print(f"ERR {p}: {e}")
            allok = False
    return 0 if allok else 1


if __name__ == '__main__':
    sys.exit(main())
