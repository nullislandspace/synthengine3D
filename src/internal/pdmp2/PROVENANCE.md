<!-- VENDORED COPY. Upstream:
       git@github.com:nullislandspace/public-domain-mp2-encoder.git
       commit 9c1b7ea
     Kept byte-identical apart from this banner. Fixes go upstream first,
     then come back here; the tests that prove it correct (round trips
     through a reference decoder) only exist upstream, because they need
     ffmpeg and numpy and a host. -->

# Where every number in here came from

This project exists so that an MPEG audio encoder can be linked into
something without dragging a copyleft licence along. That claim is only
worth anything if the provenance of the code is actually clean, so this
file records it line by line. If you are auditing this, start here.

## The short version

No code, and no table, was taken from any MPEG encoder. Not shine, not
twolame, not LAME, not ffmpeg, not the ISO reference source (dist10).
The normative lookup tables came from a **public domain (CC0)** decoder.
The filterbank window was **measured**, not copied. Everything else is
either a formula or original work.

## Table by table

### Quantisation classes — `src/pdmp2_tables.c`, `pdmp2_classes`

Formulas, not data. Layer II's quantisers are `2^n - 1` levels for the
linear classes and 3, 5 and 9 levels for the three grouped ones; the
codeword widths follow from `ceil(log2(levels^3))`. `tools/checktables.c`
regenerates all of it from those formulas and asserts the table matches,
so there is nothing here to have copied.

Structure described in: ISO/IEC 11172-3 body text, §2.4.2.7 and §2.4.3.2
(the document is freely downloadable; see "the standard itself" below).

### Bit allocation tables — `pdmp2_alloc_codes`, `pdmp2_groups_*`

These are the only genuinely tabular normative data in the codec. A
decoder reads the same numbers, so they are an interface: a Layer II
bitstream cannot be parsed without them and no implementation is free to
choose different ones.

**Source: [minimp3](https://github.com/lieff/minimp3), dedicated to the
public domain under CC0-1.0.** minimp3 stores them very compactly, as a
flat index→class array plus a per-band-group `{offset, width, count}`
descriptor. pdmp2 re-expresses the same data in the form an *encoder*
needs (walking indices upward rather than looking one up), but the
numbers are minimp3's.

CC0 imposes no conditions, so nothing has to be carried forward. The
credit is here because it is deserved, not because it is required.

Cross-checks, because trusting one source for interface data is how you
ship plausible noise:

* the MPEG-2 LSF table's `nbal` values sum to 75 bits per channel and 150
  for stereo at `sblimit` 30, which matches independent descriptions of
  ISO Table 3-B.2;
* every legal rate / channel / bitrate combination is round-tripped
  through ffmpeg's decoder by `tests/measure.py`. A wrong allocation
  table does not decode quietly wrong, it decodes as noise, and the
  measured SNR would collapse.

### Scalefactors — `pdmp2_sf_a`

ISO Table 3-B.1 is not really a table: it is `2 * 2^(-b/3)` evaluated 63
times. pdmp2 evaluates it. `tools/checktables.c` asserts the consequences
(monotone, successive ratio `2^(-1/3)`, index 0 reaches full scale, the
last index falls below one 16-bit LSB).

### The analysis window — `src/pdmp2_window.c`

**Measured, by `tools/measure_window.py`.** Not copied from anywhere.

The method: emit one frame per subband, each carrying a single nonzero
subband sample; have a reference decoder play them back; what comes out
for band *k* is that decoder's synthesis basis function. Fit all bands at
once to recover the shared prototype (the fit residual is 0.0005%, so
this is a measurement, not an estimate), then reverse it into the matched
analysis filter.

This is not gold-plating. A cosine-modulated pseudo-QMF only reconstructs
if the analysis prototype is the time reverse of the synthesis one, and
the synthesis side is in the decoder. A perfectly good 512-tap
Kaiser-windowed sinc with a 100 dB stopband was tried first and measured
**16 dB SNR** — the aliasing between adjacent subbands simply does not
cancel against a mismatched partner. So the window is as much an
interface as the allocation tables are; the difference is that this one
can be measured, and was.

Black-box measurement of an interface, with our own code. No source file
of anyone else's was involved.

### Frame and header layout

ISO/IEC 11172-3 §2.4.1 (and ISO/IEC 13818-3 for the LSF sample rates and
bitrates). Verified byte-for-byte: for identical settings, pdmp2's header
bytes match what ffmpeg's own MP2 encoder writes.

One trap worth recording, since it cost an afternoon: **the syncword is
12 bits, not 11.** With `ID = 1` the two readings produce the same first
two bytes, so an off-by-one there yields a working MPEG-1 encoder and a
completely broken MPEG-2 one, and shifts every field after it by a bit.

### Bit allocation — `allocate()` in `src/pdmp2.c`

Entirely original, and nothing about it is normative: the standard's
psychoacoustic models are informative, and a decoder cannot tell whether
one was used. pdmp2 uses textbook rate-distortion greedy on mean square
error, with no tuned constants. See the comment above the function for
why the masking model that used to be there was removed.

## The standard itself

ISO/IEC 11172-3 is available without charge from several university
mirrors; this work used the copy at
`https://csclub.uwaterloo.ca/~pbarfuss/ISO11172-3.pdf` (and an identical
one at `cs.baylor.edu`). Note that these copies contain the body text but
**not** Annex B, which is where the lookup tables live — hence minimp3
for those.

Reading a specification in order to implement it is ordinary engineering
and creates no encumbrance. ISO asserts copyright in the *document*, not
in the interface it describes, and the tables required to interoperate
are functional data rather than creative expression.

## What was deliberately not used

* **shine, twolame, LAME, libmpg123** — all LGPL. Never consulted for
  this implementation.
* **ffmpeg** — LGPL. Used only as a *reference decoder and encoder* to
  test against, exactly as any other black box would be. No ffmpeg source
  was read or copied.
* **The ISO reference source ("dist10")** — not used. Its distribution
  terms are unclear and it is unnecessary.
* **kjmp2** — a nicely written public-domain-ish MP2 decoder. Its
  `kjmp2.c` was downloaded and its licence header read, at which point it
  turned out to be zlib, not public domain: clause 3 requires the notice
  to travel with source distributions. Nothing was taken from it. It is
  mentioned only so that this list is complete and honest.

## Patents

MPEG-1 Audio Layer II patents have all expired; the last of them lapsed
around 2012, more than a decade before this was written. This is not
legal advice, and if you are shipping at a scale where it matters, get
your own.
