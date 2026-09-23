#!/usr/bin/env python3
"""Draw a numbered block of Hershey's database, to find a glyph by eye.

    python3 tools/hershey/contact_sheet.py 2200 2300 > sheet.pgm

Ten glyphs a row, each row labelled with the number of its first. That is how
the quotation marks, the comma and the dash in make_hershey_ext.py's ALIASES
were identified -- Hershey numbered his glyphs but never said, in the data,
which character each one is.

Writes a PGM to stdout and needs nothing but Python. The row labels are drawn
with the roman simplex digits out of the same database, so the sheet has no
dependency the font itself does not.
"""

import sys

from make_hershey_ext import DATA, PEN_UP, load_glyphs

CELL = 46      # pixels per glyph cell
LABEL_W = 70   # room for the row number
PER_ROW = 10
SIZE = 30      # glyph height in pixels, 21 font units


def main():
    lo = int(sys.argv[1]) if len(sys.argv) > 1 else 2200
    hi = int(sys.argv[2]) if len(sys.argv) > 2 else lo + 100
    db = load_glyphs(DATA)

    rows = (hi - lo + PER_ROW - 1) // PER_ROW
    w = LABEL_W + PER_ROW * CELL
    h = rows * CELL + 20
    img = bytearray(b"\xff" * (w * h))

    def plot(x, y):
        x, y = int(x), int(y)
        if 0 <= x < w and 0 <= y < h:
            img[y * w + x] = 0

    def line(x0, y0, x1, y1):
        n = int(max(abs(x1 - x0), abs(y1 - y0))) + 1
        for i in range(n + 1):
            t = i / n
            plot(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t)

    def draw(num, ox, oy, size):
        if num not in db:
            return 0
        adv, pts = db[num]
        scale = size / 21.0
        pen, px, py = False, 0.0, 0.0
        for (x, y) in pts:
            if (x, y) == PEN_UP:
                pen = False
                continue
            sx, sy = ox + x * scale, oy - y * scale
            if pen:
                line(px, py, sx, sy)
            px, py, pen = sx, sy, True
        return adv * scale

    # ASCII 48..57 are glyphs 700..709 (romans.hmp), which is how a number
    # gets written without a font library.
    def label(n, ox, oy, size):
        for ch in str(n):
            ox += draw(700 + int(ch), ox, oy, size) + 1

    for r in range(rows):
        base = lo + r * PER_ROW
        y = 16 + r * CELL + SIZE
        label(base, 4, y, 16)
        for c in range(PER_ROW):
            draw(base + c, LABEL_W + c * CELL + 6, y, SIZE)

    out = sys.stdout.buffer
    out.write(b"P5\n%d %d\n255\n" % (w, h))
    out.write(bytes(img))


if __name__ == "__main__":
    main()
