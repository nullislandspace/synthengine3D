# The Hershey font data, and how to make more of it

`hershey.dat` is Dr A. V. Hershey's vector font database, as distributed by
Paul Bourke at <https://paulbourke.net/dataformats/hershey/> (the `hershey.zip`
on that page, one file, unchanged). It is public domain — the work was done
for the US National Bureau of Standards in 1967 — and it is vendored here so
that regenerating the glyph tables needs nothing but Python: no download, no
network, no version of the data that might have moved on.

The engine has always drawn text with it. `src/internal/hershey.h` holds the 95
ASCII glyphs of the *roman simplex* face as `simplex[95][112]`, and that table
is part of the public API (`se_text.h`) — a game may draw its own strokes from
it, and SynthMiner's sibling does. **Nothing here touches it.**

`make_hershey_ext.py` generates `src/internal/hershey_ext.h`, which is
everything a Latin-1 or Cyrillic UI needs on top of those 95:

    python3 tools/hershey/make_hershey_ext.py

It writes the header in place. The output is checked in, so an ordinary build
never runs Python; run the script again after changing the tables at the top of
it, and commit what changes.

## The two things it refuses to do

**It will not write a header whose ASCII disagrees with `simplex`.** On every
run it regenerates all 95 ASCII glyphs from `hershey.dat` through the same
coordinate conversion it uses for everything else, and compares them, point for
point, against the committed table. If that check passes, the conversion is
provably the one the renderer already draws with — which is the only reason to
believe a Cyrillic glyph put through it will land on the right baseline.

**It will not write a header that cannot spell one of its languages.** The
`LANGUAGES` table near the top lists, per language, every letter that language
can write, in both cases — not the letters today's translations happen to use.
The script checks each one has a glyph, and dies naming those that do not:

    these have no glyph:
      pl (Polish): ł U+0142
      pl (Polish): Ł U+0141

So a translator can write whatever their language allows, and a new string in
an existing language can never introduce a character the font lacks. Only a new
*language* can, and then this script says so before anything is generated.

## What is in the generated header

**Cyrillic**, glyph numbers 2801-2832 and 2901-2932 — the *cyrillic complex*
face, because Hershey drew no Cyrillic simplex. It is a seriffed face and a
little heavier than the Latin beside it; in a Bulgarian or Russian UI, where
nearly every letter is Cyrillic, it is consistent with itself, and at 16 px (the
smallest text the game sets) it stays legible. The order is the Russian
alphabet, А Б В Г Д Е Ж З И Й К Л М Н О П Р С Т У Ф Х Ц Ч Ш Щ Ъ Ы Ь Э Ю Я — 32
letters, no Ё, which is composed instead. Bulgarian uses all but Ы and Э.

**Accented Latin**, composed rather than drawn: `é` is the simplex `e` with an
acute stroke over it, and the script emits a (codepoint → base, accent) row
instead of a glyph. That is how fifty-odd accented letters cost fifty-odd rows,
and why every acute in the font is the same acute. The renderer decides the
height: over the cap line for a capital (Latin or Cyrillic), over the x-height
for lowercase, and above the dot for `i` and `j`. A cedilla hangs below the
baseline instead, and `ø` is a bar drawn across the letter.

**Ligatures**, built by placing Hershey's own glyphs side by side: `Æ` `æ` from
A/a and E/e overlapped, `Ĳ` `ĳ` (the Dutch digraph) from I/i and J/j, the double
quotes `“` `”` `„` from two singles, and `…` from three full stops. Each part is
`(glyph, dx)`, where `dx` moves on from where the last part ended, so a negative
number overlaps them. The overlaps for `Æ` and `æ` were chosen by drawing them.

**Single glyphs lifted by number**: `‘` `’` `‚` and the en dash are Hershey's own
(2252, 2251, 711, 2231).

**A few letters nobody can compose or join** — `ß`, `ẞ`, `œ`, `Œ`, the French
guillemets `«` `»`, and the em dash — drawn by hand in this script, in Hershey's
own coordinate system.

**Folded characters**: no-break and thin spaces, the soft hyphen, the minus sign
and the primes, mapped to an ASCII twin or to nothing. These are not letters,
and a stroke font this light has nothing better to draw for them.

## The coordinate system

Hershey's records are `left`/`right` bounds followed by vertex pairs, with
`' R'` (-50, 0 once decoded) meaning pen up. Y runs *down*: the cap line is -12
and the baseline +9. The engine wants what `simplex` already uses — x from the
left bound, y *up* from the baseline — so the script converts

    x_engine = x_raw - left        advance = right - left
    y_engine = 9 - y_raw           (baseline 0, x-height 14, cap height 21)

and that is the transform the ASCII self-check proves correct.

## Looking at it: the proof sheet

`textsheet.c` draws UTF-8 through the very headers the renderer uses, on the
host, into a PGM. Build it and print every alphabet the font claims:

    cc -O2 -I../../src/internal -o textsheet textsheet.c -lm
    python3 make_hershey_ext.py --alphabets | tr '\n' '\0' | xargs -0 ./textsheet 26 > proof.pgm

Or look at one string at whatever size you please:

    ./textsheet 44 "Grüße" "cœur" "Български" > sheet.pgm

A codepoint with no glyph comes out as an empty box — on the sheet and on the
badge alike, so a gap is something you see rather than something you lose.

## Adding a language

1. **Write its alphabet into `LANGUAGES`**, both cases, every letter the
   language can write. Add anything it needs to `COMMON_PUNCTUATION` too.
2. **Run the script.** If it says nothing is missing, the font already covers
   the language — Spanish, Italian, Portuguese and the Nordic languages are all
   inside Latin-1, and Russian is inside the Cyrillic already imported. Stop
   here.
3. **Otherwise it names what is missing**, and each one is answered in whichever
   of these ways fits:
   - **Composed** (`COMPOSED`) if it is a letter plus an accent that already
     exists: `ā` is `a` with a macron. A new accent goes in `ACCENTS` as a small
     stroke list, x from the middle of the letter and y up from where the accent
     sits; add its name to the list and the renderer picks it up by number.
   - **Lifted from the database** (`ALIASES`) if Hershey drew it. To find the
     number, render a block of the database and look:

         python3 contact_sheet.py 2200 2300 > sheet.pgm

     which is how 2251, 2252, 711 and 2231 were identified. Hershey's other
     faces are all in there — Greek at 2001-2132, script, gothic, italic — and
     the `.hmp` map files on Paul Bourke's page say which glyph sits at which
     ASCII position in each face.
   - **Joined** (`LIGATURES`) if it is two glyphs side by side.
   - **Drawn by hand** (`HAND_DRAWN`) if it is none of those. Cap height 21,
     x-height 14, baseline 0, `PEN_UP` between strokes, advance a little wider
     than the drawing.
4. **Run the script again, then the proof sheet**, and look at the new letters
   at 26 px and at 60. A stroke font hides nothing: if it is wrong, it looks
   wrong.
5. **Rebuild the game.** SynthMiner's `make check` has its own `langcheck`,
   which reads `lang/*.txt` and fails on any character this font cannot draw —
   the same guarantee from the other end.

A language in a script the database has no face for at all (Greek is there,
Arabic, Hebrew, Devanagari and CJK are not) needs a different font, not a
bigger table, and that is a larger decision than this directory.
