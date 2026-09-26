/* SPDX-License-Identifier: CC0-1.0   --   see LICENSE and PROVENANCE.md */

#include "pdmp2_tables.h"

#include <math.h>
#include <stddef.h>

/* ------------------------------------------------------------------ *
 *  index -> quantisation class, for every band group.
 *
 *  Read this as seven runs. The offset of each run is what the band
 *  group tables below point at; the runs overlap on purpose, because
 *  several tables share a prefix and the standard's own numbering does
 *  the same.
 * ------------------------------------------------------------------ */
uint8_t const pdmp2_alloc_codes[92] = {
    /*  0: MPEG-1, subbands 0..2, 4 bits              */
    0, 17,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16,
    /* 16: MPEG-1, subbands 3..10, 4 bits             */
    0, 17, 18,  3, 19,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 16,
    /* 32: MPEG-1, subbands 11..22, 3 bits            */
    0, 17, 18,  3, 19,  4,  5, 16,
    /* 40: MPEG-1, subbands 23.., 2 bits              */
    0, 17, 18, 16,
    /* 44: MPEG-1 low rate, and MPEG-2 LSF upper bands */
    0, 17, 18, 19,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15,
    /* 60: MPEG-2 LSF, subbands 0..3, 4 bits          */
    0, 17, 18,  3, 19,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
    /* 76: Layer I. Unused; kept so the offsets line up with everyone
     *     else's copy of this table and can be diffed against them. */
    0,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16,
};

pdmp2_bandgroup_t const pdmp2_groups_mpeg1[4] = {
    {  0, 4,  3 }, { 16, 4,  8 }, { 32, 3, 12 }, { 40, 2,  7 },
};
pdmp2_bandgroup_t const pdmp2_groups_mpeg1_low[2] = {
    { 44, 4,  2 }, { 44, 3, 10 },
};
pdmp2_bandgroup_t const pdmp2_groups_mpeg2[3] = {
    { 60, 4,  4 }, { 44, 3,  7 }, { 44, 2, 19 },
};

/* ------------------------------------------------------------------ *
 *  Quantisation classes, computed rather than typed.
 * ------------------------------------------------------------------ */
pdmp2_class_t const pdmp2_classes[PDMP2_CLASS_MAX + 1] = {
    { 0, 1,     0,     0 },  /*  0: not transmitted */
    { 0, 1,     0,     0 },  /*  1: does not exist  */
    /* 2..16: linear, `ba` bits, 2^ba - 1 levels, symmetric about
     * 2^(ba-1) - 1. The all-ones code is the one that is never sent. */
    {  2, 1,     3,     1 }, {  3, 1,     7,     3 },
    {  4, 1,    15,     7 }, {  5, 1,    31,    15 },
    {  6, 1,    63,    31 }, {  7, 1,   127,    63 },
    {  8, 1,   255,   127 }, {  9, 1,   511,   255 },
    { 10, 1,  1023,   511 }, { 11, 1,  2047,  1023 },
    { 12, 1,  4095,  2047 }, { 13, 1,  8191,  4095 },
    { 14, 1, 16383,  8191 }, { 15, 1, 32767, 16383 },
    { 16, 1, 65535, 32767 },
    /* 17..19: grouped, three samples to a codeword */
    {  5, 3,     3,     1 },  /* 27 of 32   */
    {  7, 3,     5,     2 },  /* 125 of 128 */
    { 10, 3,     9,     4 },  /* 729 of 1024 */
};

float pdmp2_sf_a[PDMP2_SF_MAX + 1];

void pdmp2_tables_init(void) {
    static int done = 0;
    int        b;
    if (done) return;
    /* Table 3-B.1 is 2 * 2^(-b/3); pdmp2 keeps the half, 2^(-b/3),
     * because the useful question is always "what is the biggest
     * magnitude this scalefactor carries", and that is the half. */
    for (b = 0; b <= PDMP2_SF_MAX; b++) pdmp2_sf_a[b] = (float)exp2(-(double)b / 3.0);
    done = 1;
}

int pdmp2_rate_index(int samplerate) {
    switch (samplerate) {
        case 44100: case 22050: return 0;
        case 48000: case 24000: return 1;
        case 32000: case 16000: return 2;
        default:                return -1;
    }
}

int pdmp2_is_mpeg1(int samplerate) {
    return samplerate >= 32000;
}

int pdmp2_pick_table(int samplerate, int channels, int bitrate_kbps,
                     pdmp2_bandgroup_t const** groups, int* sblimit) {
    int const ri = pdmp2_rate_index(samplerate);
    int       kbps_per_ch;
    if (ri < 0) return -1;

    /* MPEG-2 LSF Layer II has exactly ONE allocation table, at every
     * rate and every bitrate. That is the strongest reason to prefer LSF
     * when the source is already 22050: there is no selection rule left
     * to get wrong. */
    if (!pdmp2_is_mpeg1(samplerate)) {
        *groups  = pdmp2_groups_mpeg2;
        *sblimit = 30;
        return 3;
    }

    kbps_per_ch = channels == 1 ? bitrate_kbps : bitrate_kbps / 2;

    if (kbps_per_ch < 56) {
        *groups  = pdmp2_groups_mpeg1_low;
        *sblimit = (ri == 2) ? 12 : 8;  /* 32 kHz keeps more bands */
        return 2;
    }
    *groups  = pdmp2_groups_mpeg1;
    /* 48 kHz stays at 27 however many bits it is given; the other two
     * open up to 30 once there are 96 kbit/s a channel to spend. */
    *sblimit = (kbps_per_ch >= 96 && ri != 1) ? 30 : 27;
    return 4;
}

/* ------------------------------------------------------------------ *
 *  Legal bitrates.
 *
 *  MPEG-1 Layer II restricts which MODES may use which bitrate: the
 *  bottom of the range is mono-only and the top is stereo-only. LSF has
 *  no such rule. Encoders that ignore this produce streams that most
 *  decoders play and some reject, which is the worst of both, so pdmp2
 *  refuses them at open() instead.
 * ------------------------------------------------------------------ */

static int const rates_mpeg1_mono[]   = { 32, 48, 56, 64, 80, 96, 112, 128, 160, 192 };
static int const rates_mpeg1_stereo[] = { 64, 96, 112, 128, 160, 192, 224, 256, 320, 384 };
static int const rates_mpeg2[]        = { 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 };

int const* pdmp2_bitrates(int samplerate, int channels, int* n) {
    if (pdmp2_rate_index(samplerate) < 0) { *n = 0; return NULL; }
    if (!pdmp2_is_mpeg1(samplerate)) {
        *n = (int)(sizeof rates_mpeg2 / sizeof rates_mpeg2[0]);
        return rates_mpeg2;
    }
    if (channels == 1) {
        *n = (int)(sizeof rates_mpeg1_mono / sizeof rates_mpeg1_mono[0]);
        return rates_mpeg1_mono;
    }
    *n = (int)(sizeof rates_mpeg1_stereo / sizeof rates_mpeg1_stereo[0]);
    return rates_mpeg1_stereo;
}

/* The bitrate_index field for the header, or -1. */
int pdmp2_bitrate_index(int samplerate, int bitrate_kbps) {
    static int const idx_mpeg1[15] = { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384 };
    static int const idx_mpeg2[15] = { 0,  8, 16, 24, 32, 40, 48,  56,  64,  80,  96, 112, 128, 144, 160 };
    int const*       tab = pdmp2_is_mpeg1(samplerate) ? idx_mpeg1 : idx_mpeg2;
    int              i;
    for (i = 1; i < 15; i++)
        if (tab[i] == bitrate_kbps) return i;
    return -1;
}
