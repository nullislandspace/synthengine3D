#!/usr/bin/env python3
"""Generate src/internal/hershey_ext.h from Hershey's glyph database.

See README.md in this directory for the whole picture, including how to add
a language. Run from anywhere:

    python3 tools/hershey/make_hershey_ext.py

It rewrites the header in place and prints a summary. It refuses to write if

  * the 95 ASCII glyphs it regenerates from the database disagree with the
    committed `simplex` table -- the proof that the coordinate conversion
    here is the one the renderer already draws with; or
  * any letter of any alphabet in LANGUAGES below has no glyph -- the proof
    that a translation into one of those languages cannot meet a character
    this font has never heard of.

The second check is on the ALPHABET, not on any translation: a language is
supported when every letter it can write comes out, whatever anybody later
types into lang/*.txt.
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.dirname(os.path.dirname(HERE))
DATA = os.path.join(HERE, "hershey.dat")
SIMPLEX_H = os.path.join(ENGINE, "src", "internal", "hershey.h")
OUT_H = os.path.join(ENGINE, "src", "internal", "hershey_ext.h")

PEN_UP = (-1, -1)

# =============================================================================
#  What has to come out, and for whom
# =============================================================================

# Every letter each language can write, in both cases -- not merely the ones
# today's translations happen to use. Adding a language means adding its
# alphabet here and making sure the tables below cover it; the check at the
# end of this script is what turns that into a promise.
LANGUAGES = {
    "en": ("English", ""),  # ASCII, and nothing else
    "de": ("German", "äöüßÄÖÜẞ"),
    "nl": ("Dutch", "áàâäéèêëíìîïóòôöúùûüĳÁÀÂÄÉÈÊËÍÌÎÏÓÒÔÖÚÙÛÜĲ"),
    "nl-BE": ("Flemish", "áàâäéèêëíìîïóòôöúùûüĳÁÀÂÄÉÈÊËÍÌÎÏÓÒÔÖÚÙÛÜĲ"),
    "fr": ("French", "àâäæçéèêëîïôöùûüÿœÀÂÄÆÇÉÈÊËÎÏÔÖÙÛÜŸŒ"),
    "bg": ("Bulgarian", "абвгдежзийклмнопрстуфхцчшщъьюя"
                        "АБВГДЕЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЬЮЯ"),
}

# Punctuation any of them may reach for: the quotation marks German, Dutch
# and Bulgarian open low, the French guillemets, the two dashes, the
# ellipsis. ASCII's own punctuation is in `simplex` already.
COMMON_PUNCTUATION = "„“”‘’‚«»–—…" "\u00A0\u00AD"

# The cyrillic complex face, in Russian alphabet order: 32 letters, no Ё
# (which is composed below instead). Bulgarian uses all but Ы and Э; keeping
# the other two costs two rows and makes Russian a translation away.
CYRILLIC = "АБВГДЕЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯ"
CYRILLIC_LOWER = "абвгдежзийклмнопрстуфхцчшщъыьэюя"

# Single glyphs lifted straight out of the database, by its glyph number.
# The numbers are Hershey's own; README.md says how to find one.
ALIASES = {
    0x2018: 2252,  # ' left single quote
    0x2019: 2251,  # ' right single quote
    0x201A: 711,   # , low single quote (a comma, which is what it is)
    0x2013: 2231,  # - en dash
}

# Glyphs built out of other glyphs, placed left to right. Each part is
# (source, dx): a glyph number or the name of a hand-drawn shape, and how
# far to move on from where the last part ended -- so 0 means "immediately
# after", and a negative number overlaps them.
LIGATURES = {
    0x00C6: [(501, 0), (505, -6)],      # AE
    0x00E6: [(601, 0), (605, -7)],      # ae
    0x0132: [(509, 0), (510, 0)],       # IJ, the Dutch digraph
    0x0133: [(609, 0), (610, 0)],       # ij
    0x201C: [(2252, 0), (2252, 1)],     # " left double quote
    0x201D: [(2251, 0), (2251, 1)],     # " right double quote
    0x201E: [(711, 0), (711, 1)],       # ,, low double quote
    0x2026: [(710, 0), (710, 2), (710, 2)],  # ... ellipsis
    0x2014: [("emdash", 0)],            # -- em dash
}

# Letters no accent can make and no two glyphs can be joined into. Hershey's
# units: x from the left edge, y up from the baseline, cap height 21,
# x-height 14, and (-1, -1) to lift the pen.
HAND_DRAWN = {
    0x00DF: (16, [  # ß -- stem, upper bowl, lower bowl
        (2, 0), (2, 15), (3, 18), (5, 20), (8, 21), (11, 20), (13, 18), (13, 15),
        (11, 13), (8, 12), (11, 11), (14, 9), (14, 5), (12, 2), (9, 1), (6, 2),
    ]),
    0x1E9E: (20, [  # capital ß: a B whose top left is cut away
        (3, 0), (3, 18), (5, 20), (8, 21), (13, 21), (16, 19), (16, 15),
        (13, 12), (9, 12), PEN_UP,
        (9, 12), (15, 11), (17, 8), (17, 4), (15, 1), (11, 0), (3, 0),
    ]),
    0x0153: (24, [  # oe
        (9, 14), (6, 14), (3, 12), (2, 9), (2, 5), (3, 2), (6, 0), (9, 0), (11, 2), (12, 5),
        (12, 9), (11, 12), (9, 14), PEN_UP,
        (12, 7), (21, 7), (21, 9), (20, 12), (18, 14), (15, 14), (13, 12), (12, 9), (12, 5),
        (13, 2), (15, 0), (18, 0), (20, 1), (21, 3),
    ]),
    0x0152: (30, [  # OE
        (15, 21), (10, 21), (6, 19), (3, 16), (2, 11), (3, 6), (6, 2), (10, 0), (15, 0),
        (15, 21), PEN_UP,
        (15, 21), (27, 21), PEN_UP, (15, 11), (23, 11), PEN_UP, (15, 0), (27, 0),
    ]),
    0x00AB: (15, [  # << -- small chevrons, mid height, not full-height brackets
        (6, 3), (2, 7), (6, 11), PEN_UP, (12, 3), (8, 7), (12, 11),
    ]),
    0x00BB: (15, [  # >>
        (3, 3), (7, 7), (3, 11), PEN_UP, (9, 3), (13, 7), (9, 11),
    ]),
    # Hershey's dash (2231) sits at y = 7 and is 12 wide; an em dash is the
    # same stroke, twice the length.
    "emdash": (24, [(1, 7), (23, 7)]),
}

# An accent is a shape and a rule for where it sits. x is measured from the
# middle of the letter it goes over, y up from wherever the accent is placed,
# so one definition serves |a|, |A| and |i| alike.
ACCENTS = [
    ("NONE", []),
    ("ACUTE", [(-2, 0), (2, 4)]),
    ("GRAVE", [(-2, 4), (2, 0)]),
    ("CIRCUMFLEX", [(-3, 0), (0, 4), (3, 0)]),
    ("DIAERESIS", [(-3, 1), (-3, 4), PEN_UP, (3, 1), (3, 4)]),
    ("TILDE", [(-4, 1), (-2, 4), (0, 2), (2, 0), (4, 3)]),
    ("RING", [(0, 0), (-2, 2), (0, 4), (2, 2), (0, 0)]),
    ("CEDILLA", [(0, 0), (0, -2), (-3, -4)]),  # below the baseline, not above
    ("SLASH", []),                             # a bar through the letter (ø)
]
ACCENT_ID = {name: i for i, (name, _) in enumerate(ACCENTS)}

# A letter plus an accent. All of Latin-1's, the two the six languages need
# from beyond it, and the Cyrillic Ё nobody but Russian asks for.
COMPOSED = {
    0x00C0: ("A", "GRAVE"),  0x00C1: ("A", "ACUTE"),  0x00C2: ("A", "CIRCUMFLEX"),
    0x00C3: ("A", "TILDE"),  0x00C4: ("A", "DIAERESIS"), 0x00C5: ("A", "RING"),
    0x00C7: ("C", "CEDILLA"),
    0x00C8: ("E", "GRAVE"),  0x00C9: ("E", "ACUTE"),  0x00CA: ("E", "CIRCUMFLEX"),
    0x00CB: ("E", "DIAERESIS"),
    0x00CC: ("I", "GRAVE"),  0x00CD: ("I", "ACUTE"),  0x00CE: ("I", "CIRCUMFLEX"),
    0x00CF: ("I", "DIAERESIS"),
    0x00D1: ("N", "TILDE"),
    0x00D2: ("O", "GRAVE"),  0x00D3: ("O", "ACUTE"),  0x00D4: ("O", "CIRCUMFLEX"),
    0x00D5: ("O", "TILDE"),  0x00D6: ("O", "DIAERESIS"), 0x00D8: ("O", "SLASH"),
    0x00D9: ("U", "GRAVE"),  0x00DA: ("U", "ACUTE"),  0x00DB: ("U", "CIRCUMFLEX"),
    0x00DC: ("U", "DIAERESIS"),
    0x00DD: ("Y", "ACUTE"),  0x0178: ("Y", "DIAERESIS"),
    0x00E0: ("a", "GRAVE"),  0x00E1: ("a", "ACUTE"),  0x00E2: ("a", "CIRCUMFLEX"),
    0x00E3: ("a", "TILDE"),  0x00E4: ("a", "DIAERESIS"), 0x00E5: ("a", "RING"),
    0x00E7: ("c", "CEDILLA"),
    0x00E8: ("e", "GRAVE"),  0x00E9: ("e", "ACUTE"),  0x00EA: ("e", "CIRCUMFLEX"),
    0x00EB: ("e", "DIAERESIS"),
    0x00EC: ("i", "GRAVE"),  0x00ED: ("i", "ACUTE"),  0x00EE: ("i", "CIRCUMFLEX"),
    0x00EF: ("i", "DIAERESIS"),
    0x00F1: ("n", "TILDE"),
    0x00F2: ("o", "GRAVE"),  0x00F3: ("o", "ACUTE"),  0x00F4: ("o", "CIRCUMFLEX"),
    0x00F5: ("o", "TILDE"),  0x00F6: ("o", "DIAERESIS"), 0x00F8: ("o", "SLASH"),
    0x00F9: ("u", "GRAVE"),  0x00FA: ("u", "ACUTE"),  0x00FB: ("u", "CIRCUMFLEX"),
    0x00FC: ("u", "DIAERESIS"),
    0x00FD: ("y", "ACUTE"),  0x00FF: ("y", "DIAERESIS"),
    0x0401: ("Е", "DIAERESIS"),  # Ё, over the Cyrillic Е
    0x0451: ("е", "DIAERESIS"),  # ё
}

# Characters that are not letters and have an ASCII twin, or nothing to draw
# at all. Everything a translator's keyboard produces that this font would
# otherwise have to refuse.
FOLDED = {
    0x00A0: " ",  # no-break space
    0x202F: " ",  # narrow no-break space (French puts one before ! ? : ;)
    0x2009: " ",  # thin space
    0x00AD: "",   # soft hyphen: draws nothing
    0x200B: "",   # zero-width space
    0x2212: "-",  # minus sign
    0x2032: "'",  # prime
    0x2033: '"',  # double prime
}


# =============================================================================
#  Hershey's database
# =============================================================================

def load_glyphs(path):
    """glyph number -> (advance, [(x, y), ...]) in engine units."""
    raw = open(path, "r", encoding="latin-1").read().replace("\n", "")
    out, i = {}, 0
    while i + 8 <= len(raw):
        num = int(raw[i:i + 5])
        cnt = int(raw[i + 5:i + 8])
        i += 8
        d = raw[i:i + 2 * cnt]
        i += 2 * cnt
        pairs = [(ord(d[j]) - 82, ord(d[j + 1]) - 82) for j in range(0, len(d), 2)]
        left, right = pairs[0]
        pts = []
        for (x, y) in pairs[1:]:
            # ' R' decodes to (-50, 0) and means "lift the pen".
            pts.append(PEN_UP if x == -50 else (x - left, 9 - y))
        out[num] = (right - left, pts)
    return out


# `romans.hmp` from the same distribution: the glyph behind each of ASCII
# 32..127, which is how `simplex` was built in the first place.
ROMANS_HMP = """
699     714     717     733     719     2271    734     731
721     722     2219    725     711     724     710     720
700-709
712     713     2241    726     2242    715     2273
501-526
2223    804     2224    2262    999     730
601-626
2225    723     2226    2246    718
"""


def expand_hmp(text):
    nums = []
    for tok in text.split():
        if "-" in tok:
            a, b = tok.split("-")
            nums.extend(range(int(a), int(b) + 1))
        else:
            nums.append(int(tok))
    return nums


def parse_simplex(path):
    """The committed simplex[95][112] table: [(advance, [(x, y), ...]), ...]."""
    src = open(path, "r").read()
    src = src[src.index("int simplex"):]
    out = []
    for body in re.findall(r"\{([^{}]*)\}", src):
        vals = [int(v) for v in re.findall(r"-?\d+", re.sub(r"/\*.*?\*/", "", body, flags=re.S))]
        n, adv, rest = vals[0], vals[1], vals[2:]
        pts = [(rest[i], rest[i + 1]) for i in range(0, 2 * n, 2)]
        out.append((adv, pts))
    return out


def self_check(db):
    """Regenerate ASCII from the database and hold it against simplex."""
    want = parse_simplex(SIMPLEX_H)
    if len(want) != 95:
        sys.exit("hershey.h: expected 95 glyphs, found %d" % len(want))
    for i, num in enumerate(expand_hmp(ROMANS_HMP)[:95]):
        adv, pts = db[num]
        w_adv, w_pts = want[i]
        if adv != w_adv or pts != w_pts:
            sys.exit("glyph %d (ASCII %d) does not match simplex[%d]:\n  ours %s\n  theirs %s"
                     % (num, 32 + i, i, (adv, pts), (w_adv, w_pts)))


# =============================================================================
#  Building the tables
# =============================================================================

def shift(pts, dx):
    return [PEN_UP if p == PEN_UP else (p[0] + dx, p[1]) for p in pts]


def build_glyphs(db):
    """codepoint -> (advance, points), for every glyph drawn in full."""
    out = {}
    for i, ch in enumerate(CYRILLIC):
        out[ord(ch)] = db[2801 + i]
    for i, ch in enumerate(CYRILLIC_LOWER):
        out[ord(ch)] = db[2901 + i]
    for cp, num in ALIASES.items():
        out[cp] = db[num]
    for key, val in HAND_DRAWN.items():
        if isinstance(key, int):
            out[key] = val

    def source(spec):
        if isinstance(spec, int):
            return db[spec]
        return HAND_DRAWN[spec]

    for cp, parts in LIGATURES.items():
        pts, adv, cursor = [], 0, 0
        for spec, dx in parts:
            a, p = source(spec)
            at = cursor + dx
            if pts:
                pts.append(PEN_UP)
            pts.extend(shift(p, at))
            cursor = at + a
            adv = max(adv, cursor)
        out[cp] = (adv, pts)
    return out


def drawable(cp, glyphs):
    """Can the font put this codepoint on the screen?"""
    if 32 <= cp <= 126:
        return True
    return cp in glyphs or cp in COMPOSED or cp in FOLDED


def check_coverage(glyphs):
    """Every letter of every language we claim to support, and the shared
    punctuation. This is the promise; everything above is the work."""
    missing = []
    for code, (name, letters) in sorted(LANGUAGES.items()):
        for ch in letters:
            if not drawable(ord(ch), glyphs):
                missing.append("%s (%s): %s U+%04X" % (code, name, ch, ord(ch)))
    for ch in COMMON_PUNCTUATION:
        if not drawable(ord(ch), glyphs):
            missing.append("shared punctuation: %s U+%04X" % (ch, ord(ch)))
    for cp, (base, _) in COMPOSED.items():
        if not drawable(ord(base), glyphs):
            missing.append("U+%04X is composed over %s, which has no glyph" % (cp, base))
    if missing:
        sys.exit("these have no glyph:\n  " + "\n  ".join(missing))


def build():
    db = load_glyphs(DATA)
    self_check(db)
    glyphs = build_glyphs(db)
    check_coverage(glyphs)

    pool, entries = [], []
    for cp in sorted(glyphs):
        adv, pts = glyphs[cp]
        entries.append((cp, adv, len(pool), len(pts)))
        pool.extend(pts)

    acc_pool, acc_entries = [], []
    for name, pts in ACCENTS:
        acc_entries.append((name, len(acc_pool), len(pts)))
        acc_pool.extend(pts)

    for _, adv, off, n in entries:
        if off + n > 65535 or adv > 127:
            sys.exit("the stroke pool or an advance outgrew its field")
    for pts in (pool, acc_pool):
        for (x, y) in pts:
            if not (-128 <= x <= 127 and -128 <= y <= 127):
                sys.exit("a coordinate does not fit in int8: %r" % ((x, y),))

    return entries, pool, acc_entries, acc_pool


def fmt_pts(pts, indent):
    out, line = [], indent
    for (x, y) in pts:
        tok = "%d,%d, " % (x, y)
        if len(line) + len(tok) > 96:
            out.append(line.rstrip())
            line = indent
        line += tok
    if line.strip():
        out.append(line.rstrip())
    return "\n".join(out)


HEADER = """// GENERATED by tools/hershey/make_hershey_ext.py -- do not edit by hand.
// Source: Hershey's glyph database (tools/hershey/hershey.dat, public domain).
// =====================================================================
//  SynthEngine3D  --  the glyphs ASCII does not have
// ---------------------------------------------------------------------
//  `simplex` (hershey.h) covers ASCII 32..126 and nothing else, which is
//  every language written in unaccented Latin letters and no other. This
//  file is the rest of what a translated UI needs, in the same units --
//  x from the left edge, y up from the baseline, cap height 21, and
//  (-1, -1) to lift the pen -- so one renderer draws both.
//
//  Three kinds of thing live here:
//
//    * whole glyphs (SE_HERSHEY_EXT[]), sorted by codepoint and looked
//      up by bisection: Cyrillic from Hershey's cyrillic complex face,
//      the quotation marks and dashes, and the letters that are neither
//      a composition nor an ASCII twin;
//    * composed letters (SE_HERSHEY_COMPOSED[]): a base codepoint and an
//      accent to draw over it, which is how fifty accented vowels cost
//      fifty table rows instead of fifty glyphs;
//    * folded characters (SE_HERSHEY_FOLD[]): no-break spaces, soft
//      hyphens and primes, mapped to an ASCII twin or to nothing.
//
//  The generator refuses to write this file unless every letter of every
//  alphabet in its LANGUAGES table comes out of it, so what is supported
//  is the language, not the translation that happens to exist today.
//
//  Engine-internal: the tables and their shapes may change in any
//  release. Games draw text through se_text.h.
// =====================================================================

#ifndef HERSHEY_EXT_H
#define HERSHEY_EXT_H

#include <stdint.h>

// A glyph's strokes: pairs of int8 in SE_HERSHEY_EXT_PTS, (-1, -1) = pen up.
typedef struct {
    uint16_t cp;     // Unicode codepoint
    int16_t  adv;    // horizontal advance, font units
    uint16_t off;    // first point in SE_HERSHEY_EXT_PTS, in PAIRS
    uint16_t n;      // how many pairs
} se_hershey_ext_t;

// A letter drawn as another letter plus an accent.
typedef struct {
    uint16_t cp;      // Unicode codepoint
    uint16_t base;    // the letter underneath (ASCII, or an ext glyph)
    uint8_t  accent;  // SE_ACCENT_*
} se_hershey_composed_t;

// A character with an ASCII twin. `to` is 0 for one that draws nothing.
typedef struct {
    uint16_t cp;
    uint8_t  to;
} se_hershey_fold_t;
"""


def print_alphabets():
    """One line per language, for the proof sheet (README.md says how)."""
    for code, (name, letters) in sorted(LANGUAGES.items()):
        ascii_upper = "ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz 0123456789"
        print("%s %s: %s %s" % (code, name, ascii_upper if not letters else letters, ""))
    print("punctuation: " + COMMON_PUNCTUATION.replace("\u00A0", " ").replace("\u00AD", "")
          + " !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~")


def main():
    if "--alphabets" in sys.argv:
        print_alphabets()
        return
    entries, pool, acc_entries, acc_pool = build()

    out = [HEADER]
    out.append("\n// The languages the tables below are checked against, when they are")
    out.append("// generated. Adding one is tools/hershey/README.md's business.")
    for code, (name, letters) in sorted(LANGUAGES.items()):
        out.append("//     %-6s %-10s %s" % (code, name, "ASCII only" if not letters else
                                             "+%d letters" % len(letters)))

    out.append("\n// --- Accents ----------------------------------------------------------------\n")
    for i, (name, off, n) in enumerate(acc_entries):
        out.append("#define SE_ACCENT_%-12s %d" % (name, i))
    out.append("#define SE_ACCENT_COUNT      %d\n" % len(acc_entries))
    out.append("// Accent strokes: x from the MIDDLE of the letter, y up from where the")
    out.append("// accent is placed (the renderer decides that from the letter's height).")
    out.append("static int8_t const SE_HERSHEY_ACCENT_PTS[] = {")
    out.append(fmt_pts(acc_pool, "    "))
    out.append("};")
    out.append("static struct { uint16_t off, n; } const SE_HERSHEY_ACCENT[SE_ACCENT_COUNT] = {")
    out.append("    " + " ".join("{%d,%d}," % (off, n) for _, off, n in acc_entries))
    out.append("};\n")

    out.append("// --- Whole glyphs -----------------------------------------------------------\n")
    out.append("static int8_t const SE_HERSHEY_EXT_PTS[] = {")
    out.append(fmt_pts(pool, "    "))
    out.append("};")
    out.append("#define SE_HERSHEY_EXT_COUNT %d" % len(entries))
    out.append("static se_hershey_ext_t const SE_HERSHEY_EXT[SE_HERSHEY_EXT_COUNT] = {")
    for cp, adv, off, n in entries:
        out.append("    {0x%04X, %3d, %5d, %3d},  // %s" % (cp, adv, off, n, chr(cp)))
    out.append("};\n")

    out.append("// --- Composed letters -------------------------------------------------------\n")
    out.append("#define SE_HERSHEY_COMPOSED_COUNT %d" % len(COMPOSED))
    out.append("static se_hershey_composed_t const SE_HERSHEY_COMPOSED[SE_HERSHEY_COMPOSED_COUNT] = {")
    for cp in sorted(COMPOSED):
        base, accent = COMPOSED[cp]
        out.append("    {0x%04X, 0x%04X, SE_ACCENT_%s},  // %s" % (cp, ord(base), accent, chr(cp)))
    out.append("};\n")

    out.append("// --- Folded characters ------------------------------------------------------\n")
    out.append("#define SE_HERSHEY_FOLD_COUNT %d" % len(FOLDED))
    out.append("static se_hershey_fold_t const SE_HERSHEY_FOLD[SE_HERSHEY_FOLD_COUNT] = {")
    for cp in sorted(FOLDED):
        to = FOLDED[cp]
        out.append("    {0x%04X, %3d},  // U+%04X%s" % (cp, ord(to) if to else 0, cp,
                                                       " -> '%s'" % to if to else " -> nothing"))
    out.append("};\n")
    out.append("#endif // HERSHEY_EXT_H")

    open(OUT_H, "w").write("\n".join(out) + "\n")
    print("%s: %d glyphs (%d stroke pairs), %d composed, %d folded, %d accents"
          % (os.path.relpath(OUT_H, ENGINE), len(entries), len(pool),
             len(COMPOSED), len(FOLDED), len(acc_entries)))
    print("ASCII regenerated from hershey.dat matches simplex[] exactly.")
    print("alphabets covered: " + ", ".join("%s (%s)" % (c, n) for c, (n, _) in sorted(LANGUAGES.items())))


if __name__ == "__main__":
    main()
