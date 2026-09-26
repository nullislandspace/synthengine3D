/* =====================================================================
 *  pdmp2  --  MPEG-1/2 Audio Layer II encoder
 * ---------------------------------------------------------------------
 *  SPDX-License-Identifier: CC0-1.0   (see LICENSE, PROVENANCE.md)
 *
 *  Layout of this file, in the order a frame moves through it:
 *
 *    1. the ANALYSIS FILTERBANK, 1152 samples -> 36 x 32 subband
 *       samples. This is the only expensive part and the only part with
 *       coefficients that are mine rather than the standard's (see the
 *       long note above pdmp2_window_init);
 *    2. SCALEFACTORS, one per 12-sample part, three parts per subband,
 *       plus the scfsi code that lets parts share one;
 *    3. BIT ALLOCATION, which is where an encoder is allowed to have
 *       opinions -- nothing about it is normative, so this one is a
 *       plain greedy noise-to-mask walk rather than either of the two
 *       psychoacoustic models in the standard's informative annex;
 *    4. QUANTISE AND WRITE.
 * ===================================================================== */

#include "pdmp2.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "pdmp2_tables.h"

/* Embedders: point PDMP2_CONFIG_H at a header of your own (quoted, e.g.
 * -DPDMP2_CONFIG_H='"pdmp2_port.h"') to define PDMP2_MALLOC and
 * PDMP2_FREE. The working set is around 35 KB and wants to live wherever
 * your slow-but-plentiful memory is -- PSRAM on an ESP32 -- rather than
 * in whatever malloc() hands out. */
#ifdef PDMP2_CONFIG_H
#include PDMP2_CONFIG_H
#endif

#ifndef PDMP2_MALLOC
#define PDMP2_MALLOC(n) malloc(n)
#define PDMP2_FREE(p)   free(p)
#endif

#define NCH_MAX  2
#define NSB      32   /* subbands the filterbank always produces */
#define NSLOT    36   /* subband sample slots in a frame (36 * 32 = 1152) */
#define NPART    3    /* scalefactor parts; 12 slots each */
#define WINLEN   512

/* M_PI is POSIX, not C99, and this file builds with -std=c99 -Wpedantic. */
#define PDMP2_PI 3.14159265358979323846

/* ------------------------------------------------------------------ *
 *  ANALYSIS GAIN
 *
 *  The filterbank's absolute gain is not free: whatever comes out of
 *  here is reconstructed by the DECODER's synthesis bank, whose gain is
 *  fixed by the standard. Get this wrong and nothing sounds broken, the
 *  output is just quiet or loud by a constant -- the sort of bug that
 *  survives listening tests and shows up as "why is our stream 6 dB
 *  under everything else".
 *
 *  So it is calibrated rather than derived: tests/calibrate.sh encodes a
 *  full-scale 1 kHz tone, decodes it with ffmpeg, and prints the ratio
 *  to apply here. tests/run_tests.sh then asserts the round-trip level
 *  is within 0.1 dB, so this constant cannot silently drift.
 * ------------------------------------------------------------------ */
#ifndef PDMP2_ANALYSIS_GAIN
#define PDMP2_ANALYSIS_GAIN 1.0f
#endif

struct pdmp2_enc {
    pdmp2_config_t cfg;
    int            nch;
    int            sblimit;
    int            mpeg1;
    int            rate_idx;
    int            br_idx;

    /* per subband, flattened out of the band group tables so the hot
     * loops never walk a group list */
    uint8_t        nbal[NSB];
    uint8_t        ncodes[NSB];
    uint8_t const* codes[NSB];

    /* frame size, as an exact fraction so padding lands where it must */
    int            frame_base;
    int            pad_num, pad_den, pad_acc;

    /* filterbank */
    float*         win;                 /* WINLEN */
    float*         mat;                 /* NSB * 64 */
    float*         hist;                /* NCH_MAX * WINLEN */
    float*         work;                /* WINLEN + PDMP2_SAMPLES_PER_FRAME */
    float*         sb;                  /* NCH_MAX * NSLOT * NSB */
    float*         pcm;                 /* NCH_MAX * PDMP2_SAMPLES_PER_FRAME */

    /* per-frame decisions */
    uint8_t        aidx[NCH_MAX][NSB];        /* allocation index on the wire */
    uint8_t        aba[NCH_MAX][NSB];         /* the class it decodes to */
    uint8_t        scf[NCH_MAX][NSB][NPART];  /* scalefactor index, per part */
    uint8_t        nscf[NCH_MAX][NSB];        /* how many are transmitted */
    uint8_t        scfsi[NCH_MAX][NSB];
    float          apart[NCH_MAX][NSB][NPART];/* 2^(-scf/3), per part */
    double         power[NCH_MAX][NSB];       /* mean square, full scale = 1 */
    uint8_t        live[NCH_MAX][NSB];        /* worth spending a bit on */

    uint8_t        buf[PDMP2_MAX_FRAME_BYTES];
    int            bitpos;
};

/* ================================================================== *
 *  bit writer -- MSB first into a buffer that starts zeroed
 * ================================================================== */

static void put_bits(pdmp2_enc_t* e, uint32_t v, int n) {
    while (n > 0) {
        int const space = 8 - (e->bitpos & 7);
        int const take  = n < space ? n : space;
        uint32_t const chunk = (v >> (n - take)) & ((1u << take) - 1u);
        e->buf[e->bitpos >> 3] |= (uint8_t)(chunk << (space - take));
        e->bitpos += take;
        n -= take;
    }
}

/* ================================================================== *
 *  1. the analysis filterbank
 * ================================================================== */

/* THE ANALYSIS WINDOW IS NOT A DESIGN CHOICE. That is the one thing
 * about this filterbank worth knowing, and it is not obvious.
 *
 * A Layer II bitstream says nothing about how the encoder split the
 * signal into subbands, so it is tempting to conclude the analysis
 * prototype is free and any good 512 tap lowpass will do. It is not.
 * This is a cosine-modulated pseudo-QMF bank: adjacent subbands overlap
 * heavily and their aliasing only CANCELS if the analysis prototype is
 * the time reverse of the synthesis prototype -- and the synthesis
 * prototype lives in the decoder, fixed by the standard. Use a different
 * one and the bitstream is still perfectly valid, the audio is still
 * recognisable, and the SNR sits near 16 dB however many bits are spent.
 *
 * (That is not a hypothetical. A Kaiser-windowed sinc, cutoff pi/64,
 * stopband over 100 dB, measured 16 dB. Spending 50% more bitrate moved
 * it by less than 1 dB, which is what pointed at the filterbank.)
 *
 * So pdmp2_window is MEASURED rather than designed or copied:
 * tools/measure_window.py pushes single subband samples through a
 * reference decoder, fits the basis functions that come back, and
 * reverses them into the matched analysis filter. The result is a
 * measurement of an interface, taken with our own code -- which is why
 * it can carry this project's licence and not somebody else's.
 *
 * The sign alternation every 64 taps is folded into the table. It is
 * forced, not chosen: the matrixing below sums every 64th windowed
 * sample with equal sign, but the true modulation cos((2k+1)(n-16)pi/64)
 * negates each time n advances by 64, because (2k+1)*pi is an odd
 * multiple of pi. Folding the alternation in is what makes the fast form
 * equal the slow one. */
extern float const pdmp2_window[WINLEN];

static void window_init(float* win, float* mat) {
    int n, k, i;
    for (n = 0; n < WINLEN; n++) win[n] = pdmp2_window[n] * (float)PDMP2_ANALYSIS_GAIN;
    for (k = 0; k < NSB; k++)
        for (i = 0; i < 64; i++)
            mat[k * 64 + i] = (float)cos(((2 * k + 1) * (i - 16) * PDMP2_PI) / 64.0);
}

/* 1152 new samples for one channel -> 36 slots of 32 subband samples.
 * `work` holds the previous 512 samples followed by the new 1152, so the
 * window for slot t is simply work[t*32 .. t*32+511], newest last. */
static void analyse(pdmp2_enc_t* e, int ch) {
    float* const out = e->sb + (size_t)ch * NSLOT * NSB;
    float* const hist = e->hist + (size_t)ch * WINLEN;
    int          t;

    memcpy(e->work, hist, WINLEN * sizeof(float));
    memcpy(e->work + WINLEN, e->pcm + (size_t)ch * PDMP2_SAMPLES_PER_FRAME,
           PDMP2_SAMPLES_PER_FRAME * sizeof(float));

    for (t = 0; t < NSLOT; t++) {
        float const* const w = e->work + t * 32;  /* w[511] is newest */
        float              y[64];
        int                i, j, k;

        for (i = 0; i < 64; i++) {
            float s = 0.0f;
            for (j = 0; j < 8; j++) {
                int const idx = i + 64 * j;
                s += w[511 - idx] * e->win[idx];
            }
            y[i] = s;
        }
        for (k = 0; k < NSB; k++) {
            float const* const m = e->mat + k * 64;
            float              s = 0.0f;
            for (i = 0; i < 64; i++) s += m[i] * y[i];
            out[t * NSB + k] = s;
        }
    }
    /* Carry the last 512 samples for the next frame. */
    memcpy(hist, e->work + PDMP2_SAMPLES_PER_FRAME, WINLEN * sizeof(float));
}

/* ================================================================== *
 *  2. scalefactors
 * ================================================================== */

/* The smallest index whose reach 2^(-b/3) still covers `peak`. Smaller
 * index means larger reach, so this is a floor, not a ceiling. */
static int scf_for_peak(float peak) {
    int b;
    if (!(peak > 0.0f)) return PDMP2_SF_MAX;
    b = (int)floor(-3.0 * log2((double)peak));
    if (b < 0) b = 0;                        /* clipping; caller clamps */
    if (b > PDMP2_SF_MAX) b = PDMP2_SF_MAX;  /* below the last LSB */
    /* floor() can land one short against the float table; walk back. */
    while (b > 0 && pdmp2_sf_a[b] < peak) b--;
    return b;
}

static void scalefactors(pdmp2_enc_t* e) {
    int ch, sb, p;
    for (ch = 0; ch < e->nch; ch++) {
        float const* const sbv = e->sb + (size_t)ch * NSLOT * NSB;
        for (sb = 0; sb < e->sblimit; sb++) {
            int   b[NPART];
            float pk[NPART];
            double energy = 0.0;
            float  peak_all = 0.0f;

            for (p = 0; p < NPART; p++) {
                float mx = 0.0f;
                int   t;
                for (t = p * 12; t < p * 12 + 12; t++) {
                    float const v = sbv[t * NSB + sb];
                    float const a = fabsf(v);
                    if (a > mx) mx = a;
                    energy += (double)v * (double)v;
                }
                pk[p] = mx;
                b[p]  = scf_for_peak(mx);
                if (mx > peak_all) peak_all = mx;
            }
            (void)pk;

            /* Share a scalefactor between parts whose indices are close.
             * Sharing must take the SMALLER index -- the larger reach --
             * or the louder part clips. One index of slack is 2 dB of
             * lost resolution for the quieter part and 4 or 6 bits
             * saved, which at these bitrates is the better trade. */
            {
                int const d01 = abs(b[0] - b[1]);
                int const d12 = abs(b[1] - b[2]);
                int const d02 = abs(b[0] - b[2]);
                if (d01 <= 1 && d12 <= 1 && d02 <= 1) {
                    int const m = b[0] < b[1] ? (b[0] < b[2] ? b[0] : b[2]) : (b[1] < b[2] ? b[1] : b[2]);
                    b[0] = b[1] = b[2] = m;
                    e->scfsi[ch][sb] = 2;
                    e->nscf[ch][sb]  = 1;
                } else if (d01 <= 1) {
                    b[0] = b[1] = (b[0] < b[1] ? b[0] : b[1]);
                    e->scfsi[ch][sb] = 1;   /* parts 0,1 share; 2 alone */
                    e->nscf[ch][sb]  = 2;
                } else if (d12 <= 1) {
                    b[1] = b[2] = (b[1] < b[2] ? b[1] : b[2]);
                    e->scfsi[ch][sb] = 3;   /* part 0 alone; 1,2 share */
                    e->nscf[ch][sb]  = 2;
                } else {
                    e->scfsi[ch][sb] = 0;
                    e->nscf[ch][sb]  = 3;
                }
            }

            for (p = 0; p < NPART; p++) {
                e->scf[ch][sb][p]   = (uint8_t)b[p];
                e->apart[ch][sb][p] = pdmp2_sf_a[b[p]];
            }

            /* Mean square power, which is what the allocator compares
             * against quantisation noise. `live` only excludes bands
             * that are genuinely empty -- rate-distortion sorts out the
             * merely quiet ones on its own, and much better than a
             * threshold does. */
            e->power[ch][sb] = energy / (double)NSLOT;
            e->live[ch][sb]  = (uint8_t)(peak_all > 0.0f);
        }
        for (sb = e->sblimit; sb < NSB; sb++) e->live[ch][sb] = 0;
    }
}

/* ================================================================== *
 *  3. bit allocation
 *
 *  NOTHING HERE IS NORMATIVE. The standard's annexes offer two
 *  psychoacoustic models; both are informative, and a decoder cannot
 *  tell which one -- or none -- was used.
 *
 *  This is none of them. It is textbook rate-distortion greedy: pick
 *  whichever step buys the most noise reduction per bit, over and over,
 *  until the frame is full. Distortion is plain mean square error, so
 *  there is not a single tuned dB constant anywhere in it.
 *
 *  THAT IS A DELIBERATE RETREAT, and worth recording. There was a
 *  masking model here -- spreading function, signal-to-mask offset, the
 *  usual shape -- and it was actively harmful. With the filterbank
 *  working, a loud tone leaves about -80 dBFS of skirt in every other
 *  subband. Whether the model called that "masked" turned on whether a
 *  spreading constant was 7 or 8 dB per band; on the wrong side of it,
 *  the allocator fed thirty bands of filterbank leakage before the band
 *  with the actual signal in it, and a 1 kHz tone came back at 27 dB SNR
 *  instead of 65. Rate-distortion cannot make that mistake: leakage
 *  carries almost no energy, so buying noise reduction there is almost
 *  never the best value, and no constant decides it.
 *
 *  The one property this leans on is of the tables, not of hearing: in
 *  every allocation table, stepping the index up costs more bits AND
 *  gives more levels, monotonically. tools/checktables.c asserts it.
 * ================================================================== */

/* Mean square quantisation noise per sample, for one subband at one
 * class. Uniform quantiser of step d has noise d*d/12; a band that is
 * not transmitted at all has noise equal to its own signal power, which
 * is what makes dropping it comparable to coding it coarsely. */
static double noise_power(pdmp2_enc_t* e, int ch, int sb, int ba) {
    double sum = 0.0;
    int    p;
    if (ba == 0) return e->power[ch][sb];
    for (p = 0; p < NPART; p++) {
        double const d = 2.0 * (double)e->apart[ch][sb][p] / (double)pdmp2_classes[ba].nlevels;
        sum += d * d / 12.0;
    }
    return sum / NPART;
}

static int step_cost(pdmp2_enc_t* e, int ch, int sb, int idx) {
    int ba;
    if (idx == 0) return 0;
    ba = e->codes[sb][idx];
    if (ba == 0) return 0;
    /* scfsi + the scalefactors + the samples: a subband's FIRST bit is
     * expensive, every later step only widens the codewords. Charging
     * that entry fee to the first step is what stops the allocator
     * opening thirty bands it cannot afford to fill. */
    return 2 + 6 * e->nscf[ch][sb] + pdmp2_class_frame_bits(&pdmp2_classes[ba]);
}

static void allocate(pdmp2_enc_t* e, int avail) {
    int used = 0;
    int ch, sb;

    memset(e->aidx, 0, sizeof e->aidx);

    for (;;) {
        double best_gain = 0.0;
        int    best_ch = -1, best_sb = -1, best_cost = 0;

        for (ch = 0; ch < e->nch; ch++) {
            for (sb = 0; sb < e->sblimit; sb++) {
                int    cur, cost;
                double gain;
                if (!e->live[ch][sb]) continue;
                cur = e->aidx[ch][sb];
                if (cur + 1 >= e->ncodes[sb]) continue;
                cost = step_cost(e, ch, sb, cur + 1) - step_cost(e, ch, sb, cur);
                if (cost <= 0 || used + cost > avail) continue;
                gain = (noise_power(e, ch, sb, e->codes[sb][cur]) -
                        noise_power(e, ch, sb, e->codes[sb][cur + 1])) / (double)cost;
                if (gain > best_gain) {
                    best_gain = gain; best_ch = ch; best_sb = sb; best_cost = cost;
                }
            }
        }
        /* The only stopping condition: nothing left that fits and helps.
         * A Layer II frame is a FIXED SIZE -- bits not spent are written
         * out as zero padding and thrown away, never carried to the next
         * frame -- so there is never a reason to stop early. */
        if (best_ch < 0) break;
        e->aidx[best_ch][best_sb]++;
        used += best_cost;
    }

    for (ch = 0; ch < e->nch; ch++)
        for (sb = 0; sb < NSB; sb++)
            e->aba[ch][sb] = (sb < e->sblimit) ? e->codes[sb][e->aidx[ch][sb]] : 0;
}

/* ================================================================== *
 *  4. quantise and write
 * ================================================================== */

/* The decoder reconstructs q * (2/nlevels) * 2^(-b/3). Inverting that is
 * the only safe way to pick q: any other scaling reproduces the shape of
 * the signal at the wrong level, which is inaudible in a listening test
 * and obvious in a measurement. The clamp costs the single peak sample
 * of a part up to half a step, which is why it is a clamp and not an
 * error. */
static int quantise(float x, float a, pdmp2_class_t const* c) {
    float const scaled = x * (float)c->nlevels / (2.0f * a);
    int         q      = (int)lrintf(scaled);
    if (q > c->half) q = c->half;
    if (q < -c->half) q = -c->half;
    return q;
}

static void write_frame(pdmp2_enc_t* e, int frame_bytes, int padding) {
    int sb, ch, gr, p, i;

    memset(e->buf, 0, (size_t)frame_bytes);
    e->bitpos = 0;

    /* ---- header ---- */
    /* TWELVE bits of syncword, not eleven. With ID = 1 the two spellings
     * produce the same first two bytes, so an off-by-one here builds a
     * working MPEG-1 encoder and a broken MPEG-2 one -- and the 31-bit
     * header shifts every field after it by a bit. */
    put_bits(e, 0xFFFu, 12);                       /* syncword            */
    put_bits(e, (uint32_t)(e->mpeg1 ? 1 : 0), 1);  /* 1 = MPEG-1, 0 = LSF */
    put_bits(e, 2u, 2);                            /* layer 10 = II       */
    put_bits(e, 1u, 1);                            /* 1 = no CRC          */
    put_bits(e, (uint32_t)e->br_idx, 4);
    put_bits(e, (uint32_t)e->rate_idx, 2);
    put_bits(e, (uint32_t)(padding ? 1 : 0), 1);
    put_bits(e, 0u, 1);                            /* private             */
    put_bits(e, e->nch == 1 ? 3u : 0u, 2);         /* 11 = mono, 00 = st. */
    put_bits(e, 0u, 2);                            /* mode_extension      */
    put_bits(e, 0u, 1);                            /* copyright           */
    put_bits(e, 0u, 1);                            /* original            */
    put_bits(e, 0u, 2);                            /* emphasis: none      */

    /* ---- allocation ---- */
    for (sb = 0; sb < e->sblimit; sb++)
        for (ch = 0; ch < e->nch; ch++)
            put_bits(e, e->aidx[ch][sb], e->nbal[sb]);

    /* ---- scfsi, then scalefactors: two passes, subband major ---- */
    for (sb = 0; sb < e->sblimit; sb++)
        for (ch = 0; ch < e->nch; ch++)
            if (e->aba[ch][sb]) put_bits(e, e->scfsi[ch][sb], 2);

    for (sb = 0; sb < e->sblimit; sb++) {
        for (ch = 0; ch < e->nch; ch++) {
            if (!e->aba[ch][sb]) continue;
            switch (e->scfsi[ch][sb]) {
                case 0:
                    for (p = 0; p < 3; p++) put_bits(e, e->scf[ch][sb][p], 6);
                    break;
                case 1:  /* parts 0 and 1 share the first */
                    put_bits(e, e->scf[ch][sb][0], 6);
                    put_bits(e, e->scf[ch][sb][2], 6);
                    break;
                case 2:
                    put_bits(e, e->scf[ch][sb][0], 6);
                    break;
                default: /* 3: part 0 alone, then 1 and 2 share */
                    put_bits(e, e->scf[ch][sb][0], 6);
                    put_bits(e, e->scf[ch][sb][1], 6);
                    break;
            }
        }
    }

    /* ---- samples: 12 groups of 3, subband major, channel minor ---- */
    for (gr = 0; gr < 12; gr++) {
        int const part = gr / 4;
        for (sb = 0; sb < e->sblimit; sb++) {
            for (ch = 0; ch < e->nch; ch++) {
                pdmp2_class_t const* c;
                float const*         sbv;
                float                a;
                int                  q[3];
                int const            ba = e->aba[ch][sb];
                if (!ba) continue;
                c   = &pdmp2_classes[ba];
                sbv = e->sb + (size_t)ch * NSLOT * NSB;
                a   = e->apart[ch][sb][part];
                for (i = 0; i < 3; i++)
                    q[i] = quantise(sbv[(gr * 3 + i) * NSB + sb], a, c);

                if (c->group == 1) {
                    for (i = 0; i < 3; i++)
                        put_bits(e, (uint32_t)(q[i] + c->half), c->bits);
                } else {
                    /* Three levels into one codeword, least significant
                     * sample first: code = q0 + m*(q1 + m*q2). */
                    uint32_t const m    = c->nlevels;
                    uint32_t       code = (uint32_t)(q[2] + c->half);
                    code = code * m + (uint32_t)(q[1] + c->half);
                    code = code * m + (uint32_t)(q[0] + c->half);
                    put_bits(e, code, c->bits);
                }
            }
        }
    }
    /* The rest of the frame stays zero. A Layer II frame is a fixed size
     * and there is no reservoir to hand the slack to the next one. */
}

/* ================================================================== *
 *  entry points
 * ================================================================== */

int pdmp2_check_config(pdmp2_config_t const* cfg) {
    int const* rates;
    int        n, i;
    if (cfg == NULL) return -1;
    if (cfg->channels != 1 && cfg->channels != 2) return -2;
    if (pdmp2_rate_index(cfg->samplerate) < 0) return -3;
    rates = pdmp2_bitrates(cfg->samplerate, cfg->channels, &n);
    if (rates == NULL) return -3;
    for (i = 0; i < n; i++)
        if (rates[i] == cfg->bitrate_kbps) return 0;
    return -4;  /* legal Layer II rate, but not in this channel mode */
}

int pdmp2_samples_per_frame(pdmp2_enc_t const* e) {
    (void)e;
    return PDMP2_SAMPLES_PER_FRAME;
}

pdmp2_enc_t* pdmp2_open(pdmp2_config_t const* cfg) {
    pdmp2_enc_t*             e;
    pdmp2_bandgroup_t const* groups;
    int                      ngroups, sblimit, g, sb, k;
    long long                num;

    if (pdmp2_check_config(cfg) != 0) return NULL;
    pdmp2_tables_init();

    ngroups = pdmp2_pick_table(cfg->samplerate, cfg->channels, cfg->bitrate_kbps, &groups, &sblimit);
    if (ngroups < 0) return NULL;

    e = (pdmp2_enc_t*)PDMP2_MALLOC(sizeof *e);
    if (e == NULL) return NULL;
    memset(e, 0, sizeof *e);
    e->cfg      = *cfg;
    e->nch      = cfg->channels;
    e->sblimit  = sblimit;
    e->mpeg1    = pdmp2_is_mpeg1(cfg->samplerate);
    e->rate_idx = pdmp2_rate_index(cfg->samplerate);
    e->br_idx   = pdmp2_bitrate_index(cfg->samplerate, cfg->bitrate_kbps);
    if (e->br_idx < 0) { PDMP2_FREE(e); return NULL; }

    /* Flatten the band groups: one lookup per subband, not a walk. */
    sb = 0;
    for (g = 0; g < ngroups && sb < sblimit; g++) {
        int c;
        for (c = 0; c < groups[g].band_count && sb < sblimit; c++, sb++) {
            e->nbal[sb]   = groups[g].nbal;
            e->ncodes[sb] = (uint8_t)(1u << groups[g].nbal);
            e->codes[sb]  = pdmp2_alloc_codes + groups[g].tab_offset;
        }
    }
    if (sb != sblimit) { PDMP2_FREE(e); return NULL; }

    /* frame_bytes = 144 * bitrate / samplerate, kept as a fraction so
     * the padding bit lands exactly often enough to hit the nominal
     * bitrate on average rather than drifting under it. */
    num          = 144LL * (long long)cfg->bitrate_kbps * 1000LL;
    e->frame_base = (int)(num / cfg->samplerate);
    e->pad_num    = (int)(num % cfg->samplerate);
    e->pad_den    = cfg->samplerate;
    e->pad_acc    = 0;

    e->win  = (float*)PDMP2_MALLOC(sizeof(float) * WINLEN);
    e->mat  = (float*)PDMP2_MALLOC(sizeof(float) * NSB * 64);
    e->hist = (float*)PDMP2_MALLOC(sizeof(float) * NCH_MAX * WINLEN);
    e->work = (float*)PDMP2_MALLOC(sizeof(float) * (WINLEN + PDMP2_SAMPLES_PER_FRAME));
    e->sb   = (float*)PDMP2_MALLOC(sizeof(float) * NCH_MAX * NSLOT * NSB);
    e->pcm  = (float*)PDMP2_MALLOC(sizeof(float) * NCH_MAX * PDMP2_SAMPLES_PER_FRAME);
    if (!e->win || !e->mat || !e->hist || !e->work || !e->sb || !e->pcm) {
        pdmp2_close(e);
        return NULL;
    }
    for (k = 0; k < NCH_MAX * WINLEN; k++) e->hist[k] = 0.0f;
    window_init(e->win, e->mat);
    return e;
}

void pdmp2_close(pdmp2_enc_t* e) {
    if (e == NULL) return;
    PDMP2_FREE(e->win);
    PDMP2_FREE(e->mat);
    PDMP2_FREE(e->hist);
    PDMP2_FREE(e->work);
    PDMP2_FREE(e->sb);
    PDMP2_FREE(e->pcm);
    PDMP2_FREE(e);
}

/* Everything after the filterbank. Split out so the window measurement
 * in tools/measure_window.py can push subband samples straight in. */
static uint8_t const* encode_subbands(pdmp2_enc_t* e, size_t* len) {
    int frame_bytes, padding, avail, sb;

    scalefactors(e);

    /* Padding: add a slot whenever the accumulated fraction has earned
     * one. This is the whole of Layer II's rate control. */
    e->pad_acc += e->pad_num;
    padding = 0;
    if (e->pad_acc >= e->pad_den) { e->pad_acc -= e->pad_den; padding = 1; }
    frame_bytes = e->frame_base + padding;

    avail = frame_bytes * 8 - 32;  /* header; no CRC */
    for (sb = 0; sb < e->sblimit; sb++) avail -= e->nbal[sb] * e->nch;

    allocate(e, avail);
    write_frame(e, frame_bytes, padding);

    if (len) *len = (size_t)frame_bytes;
    return e->buf;
}

static uint8_t const* encode_common(pdmp2_enc_t* e, size_t* len) {
    int ch;
    for (ch = 0; ch < e->nch; ch++) analyse(e, ch);
    return encode_subbands(e, len);
}

/* TEST HOOK. Hands the encoder a subband frame directly, bypassing the
 * filterbank. Its one job is to let the window be MEASURED: put a single
 * subband sample through and whatever the decoder plays back is that
 * decoder's synthesis basis function, which is the only honest way to
 * learn the prototype an analysis window has to be matched to. */
uint8_t const* pdmp2_debug_encode_subbands(pdmp2_enc_t* e, float const* sbin, size_t* len) {
    if (e == NULL || sbin == NULL) return NULL;
    memcpy(e->sb, sbin, sizeof(float) * (size_t)NCH_MAX * NSLOT * NSB);
    return encode_subbands(e, len);
}

uint8_t const* pdmp2_encode_frame(pdmp2_enc_t* e, int16_t const* pcm, size_t* len) {
    int ch, i;
    if (e == NULL || pcm == NULL) return NULL;
    for (ch = 0; ch < e->nch; ch++) {
        float* const dst = e->pcm + (size_t)ch * PDMP2_SAMPLES_PER_FRAME;
        for (i = 0; i < PDMP2_SAMPLES_PER_FRAME; i++)
            dst[i] = (float)pcm[i * e->nch + ch] * (1.0f / 32768.0f);
    }
    return encode_common(e, len);
}

uint8_t const* pdmp2_encode_frame_f32(pdmp2_enc_t* e, float const* pcm, size_t* len) {
    int ch, i;
    if (e == NULL || pcm == NULL) return NULL;
    for (ch = 0; ch < e->nch; ch++) {
        float* const dst = e->pcm + (size_t)ch * PDMP2_SAMPLES_PER_FRAME;
        for (i = 0; i < PDMP2_SAMPLES_PER_FRAME; i++) {
            float v = pcm[i * e->nch + ch];
            dst[i]  = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
        }
    }
    return encode_common(e, len);
}
