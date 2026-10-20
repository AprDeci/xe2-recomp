#!/usr/bin/env python3
"""t29 failing check: the title/menu background must not have a hard-edged
top-left black quadrant.

The defect (see ../docs/STATUS.md): on the title + menu background scene the game's own
presented surface (guest 0x740d880000 / 0x740dc50000, 1280x720) carries an exact
top-left quarter (x<W/2 and y<H/2) that is pure black, while the other three
quarters show the sky gradient. It is already present in the game's own render
target (present-source), so it is not a capture artifact or a present-blit bug.

This check FAILS while the defect exists and PASSES once the quadrant is filled
with sky like the rest of the frame. No third-party deps: minimal BMP decoder.

Usage:  python check_title_quarter.py <frame.bmp> [<frame2.bmp> ...]
Exit 0 = every frame's top-left quarter carries sky (no black quadrant).
Exit 1 = at least one frame has the black quadrant (or a file cannot be read).
"""
import struct, sys

def read_bmp(p):
    with open(p, 'rb') as source:
        d = source.read()
    if d[:2] != b'BM':
        raise ValueError('%s: not a BMP' % p)
    off = struct.unpack_from('<I', d, 10)[0]
    w, h = struct.unpack_from('<ii', d, 18)
    bpp = struct.unpack_from('<H', d, 28)[0]
    if bpp not in (24, 32):
        raise ValueError('%s: unsupported %d bpp' % (p, bpp))
    B = bpp // 8
    H = abs(h); topdown = h < 0
    stride = (w * B + 3) & ~3
    def px(x, y):
        yy = y if topdown else (H - 1 - y)
        i = off + yy * stride + x * B
        return d[i + 2], d[i + 1], d[i]
    return px, w, H

def quarter_stats(px, w, H, x0, x1, y0, y1):
    n = 0; black = 0; s = [0, 0, 0]
    step = max(1, min(w // 64, H // 64))
    for y in range(y0, y1, step):
        for x in range(x0, x1, step):
            r, g, b = px(x, y)
            n += 1
            if r < 8 and g < 8 and b < 8:
                black += 1
            s[0] += r; s[1] += g; s[2] += b
    return black / max(1, n), (s[0] / max(1, n), s[1] / max(1, n), s[2] / max(1, n))

def check(p):
    px, w, H = read_bmp(p)
    bl, mean = quarter_stats(px, w, H, 0, w // 2, 0, H // 2)
    # sky carries colour (blue dominated); a black quadrant is ~all zero
    is_black_quadrant = bl >= 0.90 and max(mean) < 12
    import os
    print('%-40s TL-quarter black=%.2f mean=(%.0f,%.0f,%.0f) %s'
          % (os.path.basename(p), bl, mean[0], mean[1], mean[2],
             'BLACK-QUADRANT (defect)' if is_black_quadrant else 'ok'))
    return not is_black_quadrant

def main(argv):
    if len(argv) < 2:
        print('usage: check_title_quarter.py <frame.bmp> [...]'); return 2
    ok = True
    for p in argv[1:]:
        try:
            ok = check(p) and ok
        except Exception as e:
            print('FAIL %s: %s' % (p, e)); ok = False
    print('RESULT:', 'PASS (no black quadrant)' if ok else 'FAIL (black quadrant present)')
    return 0 if ok else 1

if __name__ == '__main__':
    sys.exit(main(sys.argv))
