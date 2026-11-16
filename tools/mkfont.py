#!/usr/bin/env python3
"""
mkfont.py — src/gfx/font.c ni yaratadi (v0.7).

Manba: X11 "misc-fixed" 8x16 shrifti (public domain — mualliflik huquqi yo'q),
Debian/Ubuntu dagi console-setup paketida Lat15-Fixed16.psf.gz nomi bilan keladi.
Faqat ASCII (0-127) belgilar olinadi.

Ishlatish:  python3 tools/mkfont.py /usr/share/consolefonts/Lat15-Fixed16.psf.gz > src/gfx/font.c
"""
import gzip, struct, sys

def load_psf(path):
    d = gzip.open(path).read()
    if d[:2] == b'\x36\x04':                       # PSF1
        mode, h = d[2], d[3]
        n = 512 if mode & 1 else 256
        glyphs = [d[4 + i*h:4 + (i+1)*h] for i in range(n)]
        table = {}
        if mode & 2:
            uni = d[4 + n*h:]
            vals = struct.unpack('<%dH' % (len(uni)//2), uni[:len(uni)//2*2])
            g = 0
            for v in vals:
                if v == 0xFFFF: g += 1; continue
                if v != 0xFFFE: table.setdefault(v, g)
        return h, glyphs, table
    raise SystemExit("faqat PSF1 qo'llab-quvvatlanadi")

h, glyphs, table = load_psf(sys.argv[1])
assert h == 16
out = []
out.append("/*")
out.append(" * font.c — 8x16 piksellik shrift (v0.7). tools/mkfont.py yaratgan, qo'lda o'zgartirmang.")
out.append(" *")
out.append(" * Manba: X11 \"misc-fixed\" 8x16 (public domain). Har bir belgi 16 bayt:")
out.append(" * har bayt — bitta qator, yuqori bit — eng chap piksel. Masalan 'A':")
a = glyphs[table.get(ord('A'), ord('A'))]
for row in a:
    out.append(" *     " + "".join('#' if row & (0x80 >> x) else '.' for x in range(8)))
out.append(" */")
out.append('#include "gfx.h"')
out.append("")
out.append("const uint8_t font8x16[128][16] = {")
for c in range(128):
    if c < 32 or c == 127:
        g = bytes(16)
    else:
        g = glyphs[table.get(c, table.get(ord('?')))]
    label = repr(chr(c)) if 32 <= c < 127 else "0x%02X" % c
    out.append("    { " + ", ".join("0x%02X" % b for b in g) + " },  /* %s */" % label.replace('*/', '* /'))
out.append("};")
print("\n".join(out))
