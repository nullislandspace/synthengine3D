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
_DUTCH = "áàâäéèêëíìîïóòôöúùûüĳÁÀÂÄÉÈÊËÍÌÎÏÓÒÔÖÚÙÛÜĲ"
_CYR_RU = ("абвгдежзийклмнопрстуфхцчшщъыьэюяё"
           "АБВГДЕЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯЁ")

LANGUAGES = {
    "en": ("English", ""),  # ASCII, and nothing else
    "sq": ("Albanian", "çëÇË"),
    "bg": ("Bulgarian", "абвгдежзийклмнопрстуфхцчшщъьюя"
                        "АБВГДЕЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЬЮЯ"),
    "ca": ("Catalan", "àèéíïòóúüç·ÀÈÉÍÏÒÓÚÜÇ"),
    "hr": ("Croatian", "čćđšžČĆĐŠŽ"),
    "cs": ("Czech", "áčďéěíňóřšťúůýžÁČĎÉĚÍŇÓŘŠŤÚŮÝŽ"),
    "da": ("Danish", "æøåÆØÅ"),
    "nl": ("Dutch", _DUTCH),
    "et": ("Estonian", "õäöüšžÕÄÖÜŠŽ"),
    "fi": ("Finnish", "äöåÄÖÅ"),
    "nl-BE": ("Flemish", _DUTCH),
    "fr": ("French", "àâäæçéèêëîïôöùûüÿœÀÂÄÆÇÉÈÊËÎÏÔÖÙÛÜŸŒ"),
    "de": ("German", "äöüßÄÖÜẞ"),
    "el": ("Greek", "αβγδεζηθικλμνξοπρστυφχψωςάέήίόύώϊϋΐΰ"
                    "ΑΒΓΔΕΖΗΘΙΚΛΜΝΞΟΠΡΣΤΥΦΧΨΩΆΈΉΊΌΎΏΪΫ"),
    "hu": ("Hungarian", "áéíóöőúüűÁÉÍÓÖŐÚÜŰ"),
    "is": ("Icelandic", "áéíóúýþðæöÁÉÍÓÚÝÞÐÆÖ"),
    "ga": ("Irish", "áéíóúÁÉÍÓÚ"),
    "it": ("Italian", "àèéìòùÀÈÉÌÒÙ"),
    "lv": ("Latvian", "āčēģīķļņšūžĀČĒĢĪĶĻŅŠŪŽ"),
    "lt": ("Lithuanian", "ąčęėįšųūžĄČĘĖĮŠŲŪŽ"),
    "no": ("Norwegian", "æøåÆØÅ"),
    "pl": ("Polish", "ąćęłńóśźżĄĆĘŁŃÓŚŹŻ"),
    "pt": ("Portuguese", "ãõçáéíóúâêôàÃÕÇÁÉÍÓÚÂÊÔÀ"),
    "ro": ("Romanian", "ăâîșțĂÂÎȘȚ"),
    "ru": ("Russian", _CYR_RU),
    "sr": ("Serbian", "абвгдђежзијклљмнњопрстћуфхцчџш"
                      "АБВГДЂЕЖЗИЈКЛЉМНЊОПРСТЋУФХЦЧЏШ"),
    "sk": ("Slovak", "áäčďéíĺľňóôŕšťúýžÁÄČĎÉÍĹĽŇÓÔŔŠŤÚÝŽ"),
    "sl": ("Slovenian", "čšžČŠŽ"),
    "es": ("Spanish", "áéíóúüñ¿¡ÁÉÍÓÚÜÑ"),
    "sv": ("Swedish", "åäöÅÄÖ"),
    "tr": ("Turkish", "çğıİöşüÇĞÖŞÜ"),
    "uk": ("Ukrainian", "абвгґдеєжзиіїйклмнопрстуфхцчшщьюя"
                        "АБВГҐДЕЄЖЗИІЇЙКЛМНОПРСТУФХЦЧШЩЬЮЯ"),
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

# Greek, from Hershey's greek SIMPLEX face -- the same weight as the Latin
# beside it, which the Cyrillic (complex, the only one he drew) is not.
# 527-550 and 627-650 are the 24 letters in alphabet order.
GREEK = "ΑΒΓΔΕΖΗΘΙΚΛΜΝΞΟΠΡΣΤΥΦΧΨΩ"
GREEK_LOWER = "αβγδεζηθικλμνξοπρστυφχψω"

# Single glyphs lifted straight out of the database, by its glyph number.
# The numbers are Hershey's own; README.md says how to find one.
# A glyph that is another one flipped left to right. Hershey drew the
# Russian Э; the Ukrainian Є is its mirror image, and taking it that way
# keeps the two identical in weight and shape.
MIRRORED = {
    0x0404: 2830,  # |Ye| , the mirror of Э
    0x0454: 2930,  # |ye| , the mirror of э
}

# A glyph turned upside down: the Spanish inverted marks, which is what
# they are.
ROTATED = {
    0x00A1: 714,  # inverted !
    0x00BF: 715,  # inverted ?
}

ALIASES = {
    0x2018: 2252,  # ' left single quote
    0x2019: 2251,  # ' right single quote
    0x201A: 711,   # , low single quote (a comma, which is what it is)
    0x2013: 2231,  # - en dash
    0x0406: 509,   # Ukrainian I -- the Latin letter, shape for shape
    0x0456: 609,   # Ukrainian i
    0x0408: 510,   # Serbian J
    0x0458: 610,   # Serbian j
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
    # Serbian |Lje| and |Nje| are Л and Н joined to Ь, which is what they
    # were made from; Hershey's own Cyrillic supplies all three.
    # -5 is not a guess: it lands |Soft sign|'s stem exactly on the upright
    # the first letter ends with, so the two SHARE that stroke, which is
    # what makes the pair one letter instead of two side by side.
    0x0409: [(2812, 0), (2829, -5)],    # |Lje|
    0x0459: [(2912, 0), (2929, -5)],    # |lje|
    0x040A: [(2814, 0), (2829, -5)],    # |Nje|
    0x045A: [(2914, 0), (2929, -5)],    # |nje|
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
    0x00B7: (10, [(4, 7), (5, 8), (6, 7), (5, 6), (4, 7)]),  # the Catalan middle dot

    # --- Polish: l and L with a stroke across the upright ---------------
    0x0142: (10, [(5, 21), (5, 0), PEN_UP, (1, 8), (9, 12)]),            # l|stroke|
    0x0141: (18, [(3, 21), (3, 0), (14, 0), PEN_UP, (0, 11), (8, 15)]),  # L|stroke|

    # --- Croatian / Serbian: d and D with a bar through the ascender ----
    0x0111: (19, [  # d|stroke|
        (13, 21), (13, 0), PEN_UP,
        (13, 11), (11, 13), (8, 14), (5, 13), (3, 11), (2, 7), (3, 3), (5, 1), (8, 0), (11, 1), (13, 3),
        PEN_UP, (9, 18), (18, 18),
    ]),
    0x0110: (21, [  # D|stroke|
        (4, 21), (4, 0), (10, 0), (14, 2), (16, 6), (16, 15), (14, 19), (10, 21), (4, 21),
        PEN_UP, (0, 11), (8, 11),
    ]),

    # --- Icelandic ------------------------------------------------------
    0x00FE: (19, [  # |thorn|
        (3, 21), (3, -7), PEN_UP,
        (3, 12), (5, 14), (8, 15), (12, 14), (15, 11), (16, 7), (15, 3), (12, 0), (8, -1), (5, 0), (3, 2),
    ]),
    0x00DE: (19, [  # |Thorn|
        (3, 21), (3, 0), PEN_UP,
        (3, 17), (10, 17), (14, 15), (16, 12), (16, 9), (14, 6), (10, 4), (3, 4),
    ]),
    0x00F0: (19, [  # |eth|
        (4, 21), (14, 15), PEN_UP, (8, 19), (14, 21), PEN_UP,
        (13, 15), (15, 11), (16, 7), (15, 3), (12, 0), (8, -1), (5, 0), (3, 3), (2, 7), (3, 11),
        (5, 14), (9, 15), (13, 15),
    ]),
    0x00D0: (21, [  # |Eth| -- a D with the bar, but the bar crosses the bowl
        (4, 21), (4, 0), (10, 0), (14, 2), (16, 6), (16, 15), (14, 19), (10, 21), (4, 21),
        PEN_UP, (1, 11), (9, 11),
    ]),

    # --- Turkish: the dotless i (the capital with a dot is composed) -----
    0x0131: (8, [(4, 14), (4, 0)]),

    # --- Ukrainian |Ghe| with an upturn ---------------------------------
    0x0490: (18, [(3, 0), (3, 21), (13, 21), (13, 25)]),
    0x0491: (16, [(3, 0), (3, 14), (11, 14), (11, 18)]),

    # --- Serbian --------------------------------------------------------
    0x040F: (18, [(3, 21), (3, 0), (15, 0), (15, 21), PEN_UP, (9, 0), (9, -5)]),  # |Dzhe|
    0x045F: (16, [(3, 14), (3, 0), (13, 0), (13, 14), PEN_UP, (8, 0), (8, -5)]),  # |dzhe|
    0x040B: (19, [  # |Tshe| -- an h with a crossbar
        (3, 21), (3, 0), PEN_UP, (0, 15), (9, 15), PEN_UP,
        (3, 11), (6, 14), (10, 15), (14, 14), (16, 11), (16, 0),
    ]),
    0x045B: (17, [
        (3, 21), (3, 0), PEN_UP, (0, 12), (8, 12), PEN_UP,
        (3, 8), (5, 11), (9, 12), (12, 11), (14, 8), (14, 0),
    ]),
    0x0402: (19, [  # |Dje| -- the same, with a tail below
        (3, 21), (3, 0), PEN_UP, (0, 15), (9, 15), PEN_UP,
        (3, 11), (6, 14), (10, 15), (14, 14), (16, 11), (16, 2), (14, -2), (10, -4),
    ]),
    0x0452: (17, [
        (3, 21), (3, 0), PEN_UP, (0, 12), (8, 12), PEN_UP,
        (3, 8), (5, 11), (9, 12), (12, 11), (14, 8), (14, 0), (12, -4), (8, -5),
    ]),

    # --- Greek's final sigma, which Hershey's 24 do not include ---------
    0x03C2: (16, [
        (13, 13), (10, 14), (6, 14), (3, 12), (2, 9), (3, 6), (6, 4), (9, 3), (10, 1), (9, -2), (6, -4),
    ]),
}

# An accent is a shape and a rule for where it sits. x is measured from the
# middle of the letter it goes over, y up from wherever the accent is placed,
# so one definition serves |a|, |A| and |i| alike.
# (name, strokes, below): `below` puts it under the baseline instead of
# over the letter, and the renderer reads that flag rather than knowing
# which accents are which.
ACCENTS = [
    ("NONE", [], False),
    ("ACUTE", [(-2, 0), (2, 4)], False),
    ("GRAVE", [(-2, 4), (2, 0)], False),
    ("CIRCUMFLEX", [(-3, 0), (0, 4), (3, 0)], False),
    ("DIAERESIS", [(-3, 1), (-3, 4), PEN_UP, (3, 1), (3, 4)], False),
    ("TILDE", [(-4, 1), (-2, 4), (0, 2), (2, 0), (4, 3)], False),
    ("RING", [(0, 0), (-2, 2), (0, 4), (2, 2), (0, 0)], False),
    ("CEDILLA", [(0, 0), (0, -2), (-3, -4)], True),
    ("SLASH", [], False),  # a bar through the letter (ø); the renderer draws it
    # The Slavic and Baltic accents, and the Hungarian one.
    ("CARON", [(-3, 4), (0, 0), (3, 4)], False),          # c|caron| s|caron| z|caron|
    ("BREVE", [(-3, 4), (-2, 1), (0, 0), (2, 1), (3, 4)], False),  # a|breve| g|breve|
    ("DOUBLE_ACUTE", [(-4, 0), (-1, 4), PEN_UP, (1, 0), (4, 4)], False),  # o|dblac|
    ("MACRON", [(-3, 2), (3, 2)], False),                 # a|macron| e|macron|
    ("DOT_ABOVE", [(0, 1), (0, 3)], False),               # e|dot| z|dot| I|dot|
    ("OGONEK", [(0, 0), (2, -1), (2, -3), (0, -4)], True),      # a|ogonek| e|ogonek|
    ("COMMA_BELOW", [(0, -1), (-1, -3)], True),           # s|comma| t|comma|
]
ACCENT_ID = {name: i for i, (name, _, _) in enumerate(ACCENTS)}

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
    0x0407: ("І", "DIAERESIS"),  # Ї, over the Ukrainian І
    0x0457: ("і", "DIAERESIS"),  # ї

    # --- Caron: Czech, Slovak, Slovenian, Croatian, Serbian, the Baltics
    0x010C: ("C", "CARON"), 0x010D: ("c", "CARON"),
    0x010E: ("D", "CARON"), 0x010F: ("d", "CARON"),
    0x011A: ("E", "CARON"), 0x011B: ("e", "CARON"),
    0x013D: ("L", "CARON"), 0x013E: ("l", "CARON"),
    0x0147: ("N", "CARON"), 0x0148: ("n", "CARON"),
    0x0158: ("R", "CARON"), 0x0159: ("r", "CARON"),
    0x0160: ("S", "CARON"), 0x0161: ("s", "CARON"),
    0x0164: ("T", "CARON"), 0x0165: ("t", "CARON"),
    0x017D: ("Z", "CARON"), 0x017E: ("z", "CARON"),

    # --- Acute on consonants: Polish, Croatian, Serbian -----------------
    0x0106: ("C", "ACUTE"), 0x0107: ("c", "ACUTE"),
    0x0143: ("N", "ACUTE"), 0x0144: ("n", "ACUTE"),
    0x015A: ("S", "ACUTE"), 0x015B: ("s", "ACUTE"),
    0x0179: ("Z", "ACUTE"), 0x017A: ("z", "ACUTE"),
    0x0139: ("L", "ACUTE"), 0x013A: ("l", "ACUTE"),
    0x0154: ("R", "ACUTE"), 0x0155: ("r", "ACUTE"),

    # --- Ogonek: Polish, Lithuanian -------------------------------------
    0x0104: ("A", "OGONEK"), 0x0105: ("a", "OGONEK"),
    0x0118: ("E", "OGONEK"), 0x0119: ("e", "OGONEK"),
    0x012E: ("I", "OGONEK"), 0x012F: ("i", "OGONEK"),
    0x0172: ("U", "OGONEK"), 0x0173: ("u", "OGONEK"),

    # --- Dot above: Polish |z|, Lithuanian |e|, the Turkish capital I ---
    0x017B: ("Z", "DOT_ABOVE"), 0x017C: ("z", "DOT_ABOVE"),
    0x0116: ("E", "DOT_ABOVE"), 0x0117: ("e", "DOT_ABOVE"),
    0x0130: ("I", "DOT_ABOVE"),

    # --- Macron: Latvian -------------------------------------------------
    0x0100: ("A", "MACRON"), 0x0101: ("a", "MACRON"),
    0x0112: ("E", "MACRON"), 0x0113: ("e", "MACRON"),
    0x012A: ("I", "MACRON"), 0x012B: ("i", "MACRON"),
    0x016A: ("U", "MACRON"), 0x016B: ("u", "MACRON"),

    # --- Comma below: Romanian, Latvian ---------------------------------
    0x0218: ("S", "COMMA_BELOW"), 0x0219: ("s", "COMMA_BELOW"),
    0x021A: ("T", "COMMA_BELOW"), 0x021B: ("t", "COMMA_BELOW"),
    0x0122: ("G", "COMMA_BELOW"), 0x0123: ("g", "COMMA_BELOW"),
    0x0136: ("K", "COMMA_BELOW"), 0x0137: ("k", "COMMA_BELOW"),
    0x013B: ("L", "COMMA_BELOW"), 0x013C: ("l", "COMMA_BELOW"),
    0x0145: ("N", "COMMA_BELOW"), 0x0146: ("n", "COMMA_BELOW"),

    # --- Breve: Romanian, Turkish ---------------------------------------
    0x0102: ("A", "BREVE"), 0x0103: ("a", "BREVE"),
    0x011E: ("G", "BREVE"), 0x011F: ("g", "BREVE"),

    # --- Double acute: Hungarian ----------------------------------------
    0x0150: ("O", "DOUBLE_ACUTE"), 0x0151: ("o", "DOUBLE_ACUTE"),
    0x0170: ("U", "DOUBLE_ACUTE"), 0x0171: ("u", "DOUBLE_ACUTE"),

    # --- Ring above: Czech ----------------------------------------------
    0x016E: ("U", "RING"), 0x016F: ("u", "RING"),

    # --- Cedilla on the Turkish s ---------------------------------------
    0x015E: ("S", "CEDILLA"), 0x015F: ("s", "CEDILLA"),

    # --- Greek's accented vowels ----------------------------------------
    0x0386: ("Α", "ACUTE"), 0x03AC: ("α", "ACUTE"),
    0x0388: ("Ε", "ACUTE"), 0x03AD: ("ε", "ACUTE"),
    0x0389: ("Η", "ACUTE"), 0x03AE: ("η", "ACUTE"),
    0x038A: ("Ι", "ACUTE"), 0x03AF: ("ι", "ACUTE"),
    0x038C: ("Ο", "ACUTE"), 0x03CC: ("ο", "ACUTE"),
    0x038E: ("Υ", "ACUTE"), 0x03CD: ("υ", "ACUTE"),
    0x038F: ("Ω", "ACUTE"), 0x03CE: ("ω", "ACUTE"),
    0x03AA: ("Ι", "DIAERESIS"), 0x03CA: ("ι", "DIAERESIS"),
    0x03AB: ("Υ", "DIAERESIS"), 0x03CB: ("υ", "DIAERESIS"),
    0x0390: ("ι", "ACUTE"),  # |iota| with both marks: the acute is the one
    0x03B0: ("υ", "ACUTE"),  # that carries the meaning at this size
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
    for i, ch in enumerate(GREEK):
        out[ord(ch)] = db[527 + i]
    for i, ch in enumerate(GREEK_LOWER):
        out[ord(ch)] = db[627 + i]
    for cp, num in MIRRORED.items():
        adv, pts = db[num]
        out[cp] = (adv, [PEN_UP if p == PEN_UP else (adv - p[0], p[1]) for p in pts])
    for cp, num in ROTATED.items():
        adv, pts = db[num]
        # Turned upside down about the x-height band, which is where an
        # inverted Spanish mark sits: head down, tail up.
        out[cp] = (adv, [PEN_UP if p == PEN_UP else (adv - p[0], 14 - p[1]) for p in pts])
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
    for name, pts, below in ACCENTS:
        acc_entries.append((name, len(acc_pool), len(pts), below))
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
    for i, (name, off, n, _below) in enumerate(acc_entries):
        out.append("#define SE_ACCENT_%-12s %d" % (name, i))
    out.append("#define SE_ACCENT_COUNT      %d\n" % len(acc_entries))
    out.append("// Accent strokes: x from the MIDDLE of the letter, y up from where the")
    out.append("// accent is placed (the renderer decides that from the letter's height).")
    out.append("static int8_t const SE_HERSHEY_ACCENT_PTS[] = {")
    out.append(fmt_pts(acc_pool, "    "))
    out.append("};")
    out.append("// off/n into the strokes above, and whether it hangs BELOW the baseline.")
    out.append("static struct { uint16_t off, n; uint8_t below; } const SE_HERSHEY_ACCENT[SE_ACCENT_COUNT] = {")
    out.append("    " + " ".join("{%d,%d,%d}," % (off, n, 1 if below else 0)
                                 for _, off, n, below in acc_entries))
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
