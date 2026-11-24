/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 GR!MM; Arctic Scream p0larpatch and FM-1 Again changes: Copyright (C) 2026 PolarPatch */
/* ECHO: Arctic Scream's DELAY (its firmware/src/tape.c, with the tape wear of its wear.c), a tape echo written in
 * integers by GR!MM; its design follows X0X's TAPE delay (fx_delay in firmware/src/dsp/fxbus.c of
 * charlesvestal/fm1-x0x, GPL-3.0, itself a port of 9W9's er99_dly_tick, GPL-3.0), none of X0X's code in it; the
 * integer pieces are Felucca's (softclip, soft_knee, sh_tz). As there: at a quarter of the rate (11.025 kHz) into a
 * 2.6 s line, read TIME back with wow and flutter between samples, the loop darker each time round (WEAR's low-pass),
 * FEEDBACK (0 .. 150 %), a tanh saturation, a DC block, a soft knee; TIME glides (the pitch bends, as a tape's does);
 * PING-PONG (Arctic Scream's, the idea from ECHOFIELD's, Philip Bos, MIT): the echoes left, right, left.
 * FM-1 Again's changes: TIME in milliseconds (dl_ms, from the tempo: app.c), not Arctic Scream's table; PING-PONG's
 * square of FEEDBACK in 32 bits; the level counters out; the echoes into the reverb's send as floats (echo_render).
 * Part of app.c (the unity build). */

/* the wow's sine: a full cycle over 2^32 -> -32767..32767 (a refined parabola, ~0.1 %: Dinghy's) */
static int32_t wear_sine(uint32_t ph)
{
    int32_t x = (int32_t)ph >> 16, ax = x < 0 ? -x : x, y, ay;
    y = (x * (32768 - ax)) >> 13;
    ay = y < 0 ? -y : y;
    y += ((((y * ay) >> 15) - y) * 7373) >> 15;
    return y > 32767 ? 32767 : y < -32767 ? -32767 : y;
}
/* m / 2^sh rounded towards zero (Felucca's sh_tz: a product that feeds back must not floor) */
static inline int32_t wear_tz(int32_t m, uint32_t sh) { return (m + ((m >> 31) & ((1 << sh) - 1))) >> sh; }

/* the wow and flutter at phases p1 (0.5 Hz), p2 (0.19 Hz), p3 (6.1 Hz), reach a: about +-0.9 a */
static inline int32_t wear_wow(uint32_t p1, uint32_t p2, uint32_t p3, int32_t a)
{
    return (((wear_sine(p1) + mulq15(wear_sine(p2), 19661)) >> 1) * a + mulq15(wear_sine(p3), 3277) * a) >> 15;
}
/* the saturation: v driven by g (Q12, 4096 = 1x) through the tanh (wear_drive), made up by ig (Q15, 2^27 / g) */
static inline int32_t wear_drive(int32_t v, int32_t g) { return softclip(wear_tz(v * g, 12)); }
static inline int32_t wear_sat(int32_t v, int32_t g, int32_t ig) { return wear_tz(wear_drive(v, g) * ig, 15); }

#define DL_D 4u                                 /* (2.1.4) the line's rate: 44.1 kHz / DL_D (was 2) */
#define DL_NC 64u                               /* a block at that rate: app_audio's 256 frames */
#define DL_LEN 28992u                           /* 2600 ms (28665 samples; 1.5.2, was 3000) + the wow's reach (1.3: ~11
                                                 * ms) + the right side's 9 ms, a whole number of blocks */
#define DL_SPREAD (99 << 8)                     /* the right side reads 9 ms further back (Q8 samples) */
#define DL_IN 8192                              /* the voices into the line, Q15: a quarter (headroom for the loop; V2:
                                                 * half of 1.0's, its return twice: loud chords never reach its knee) */
#define DL_DCK 560                              /* the DC block: 30 Hz at 11.025 kHz, Q15 */
#define DL_W1 12467522u                         /* the wow: 0.5 Hz, a block's step (2^32 * 256 / 44100 * Hz) */
#define DL_W2 4737658u                          /* .. and 0.19 Hz beside it: not quite periodic */
#define DL_W3 152103765u                        /* the flutter: 6.1 Hz */
#define DL_MSQ8 2822                            /* a ms in Q8 samples at 11.025 kHz */
_Static_assert(2600u * DL_MSQ8 / 256u + 1u + 26u + 99u + 2u < DL_LEN, "DELAY: the longest read stays in the line");

static int16_t dl_buf[DL_LEN] PLAT_POOL;
static struct {
    uint32_t w, quiet;                          /* the write index; half-rate samples with silence in and written */
    uint32_t p1, p2, p3;                        /* the wow's and the flutter's phases */
    int32_t d, m;                               /* TIME gliding to its target, the wow at the block's end (Q8 samples) */
    int32_t x, lp, le, dc, de, ml, mr;          /* the input's last sample; the loop's low-pass and DC block (and
                                                 * their steps' remainders); the last wet out, left / right */
    int32_t l[DL_NC], r[DL_NC];                 /* the block's echoes at half the rate */
} dl PLAT_POOL;

/* the parameters, 0..127, written by the main loop */
static volatile int32_t dl_ms = 363, dl_fb = 64, dl_wear = 50, dl_mix = 100;   /* a dotted eighth at 124 BPM, 55 %, return 79 % */
static volatile int32_t dl_pp;                  /* (1.4) PING-PONG, 0..127: 0 off */
#define DL_PPMAX (((DL_LEN - 400u) / 2u) << 8)    /* PING-PONG's TIME at most (Q8 samples: 2 TIME and the wow in the line) */

/* main loop: the delay gives nothing out (MIX 0, or the line silent and nothing coming in) */
static int echo_idle(void) { return dl_mix <= 0 || dl.quiet >= DL_LEN; }

/* a read d (Q8 samples) behind the write index w, between samples */
static inline int32_t echo_read(uint32_t w, int32_t d)
{
    int32_t i0 = (int32_t)w - (d >> 8), f = d & 255, b0, b1;
    i0 = i0 < 0 ? i0 + (int32_t)DL_LEN : i0;
    b0 = dl_buf[i0];
    b1 = dl_buf[i0 ? i0 - 1 : (int32_t)DL_LEN - 1];   /* one further back */
    return b0 + (((b1 - b0) * f + 128) >> 8);
}

/* the audio ISR: n (= 2 DL_NC) frames of the DLY bus in (mono) -> the echoes added to out (L R) and to the REV bus */
static void echo_render_q(const int32_t *in, int32_t *out, int32_t *rev, uint32_t n)
{
    uint32_t i, any = 0, w;
    int32_t mix = dl_mix, wear = dl_wear, fbk = dl_fb;
    int32_t target, d0, d1, m1, a, tc, g, ig, fb, f1, pp = clamp(dl_pp, 0, 127), po = 32767 - pp * 258;
    int32_t xp = dl.x, lp = dl.lp, le = dl.le, dc = dl.dc, de = dl.de, ml = dl.ml, mr = dl.mr;
    if (n != DL_D * DL_NC || mix <= 0)
        return;                                 /* MIX 0: the dry alone; the line waits as it is */
    for (i = 0; i < DL_D * DL_NC && !any; i++)
        any = in[i] != 0;
    if (!any && dl.quiet >= DL_LEN)
        return;                                 /* the line silent, nothing coming in: nothing to do */
    wear = clamp(wear, 0, 127);
    fbk = clamp(fbk, 0, 127);

    /* this block's settings: TIME (gliding), the wow, the loop's tone, drive and feedback */
    target = clamp(dl_ms, 20, 2600) * DL_MSQ8;   /* ms -> Q8 samples at 11.025 kHz */
    if (pp && target > (int32_t)DL_PPMAX) target = (int32_t)DL_PPMAX;
    if (!dl.d)
        dl.d = target;                          /* (the first block: no glide from nothing) */
    d0 = dl.d;
    d1 = d0 + ((target - d0) >> 3);             /* ~50 ms to a new TIME */
    dl.d = d1;
    dl.p1 += DL_W1, dl.p2 += DL_W2, dl.p3 += DL_W3;
    a = ((10 + wear) * 94 + wear * wear * 3) / 2;   /* the wow's reach, Q8 samples: ~0.17 .. ~11 ms (Arctic Scream 1.3: was
                                                 * 2.3 ms; a worn-out tape at the top) */
    m1 = wear_wow(dl.p1, dl.p2, dl.p3, a);      /* (wear.c) */
    tc = 32767 - wear * 148 - 4467;             /* the loop's low-pass, Q15, as it was at 22.05 kHz (~7 kHz .. ~1.2 kHz), */
    tc = 32768 - (((32768 - tc) * (32768 - tc)) >> 15);   /* .. the same pole over two of its samples at 11.025 */
    g = 4096 + wear * 97;                       /* the saturation's drive, Q12: 1x .. 4x (1.3: was 1.8x) */
    ig = (int32_t)(134217728u / (uint32_t)g) >> 1;   /* .. and the level made up after it, Q15 (the line at
                                                 * half: driven twice as hard, half back: saturating where it did) */
    g *= 2;
    fb = fbk <= 100 ? fbk * 284 : 28400 + (fbk - 100) * 769;   /* FEEDBACK, Q15: 0 .. ~0.87 at 100, then on to 1.5 (Arctic
                                                 * p0larpatch: past unity on purpose, as Audrey II's echo: it grows
                                                 * and saturates) */
    f1 = fb;
    if (pp) fb = ((fb >> 1) * (fb >> 1)) >> 13;   /* (PING-PONG: the loop twice TIME: FEEDBACK twice; 32 bits) */

    w = dl.w;
    {
        int32_t D0 = d0 + dl.m, dD = (d1 + m1) - D0;
        for (i = 0; i < DL_NC; i++) {
            const int32_t *q = in + DL_D * i;
            int32_t x = (xp + 2 * (q[0] + q[1] + q[2]) + q[3]) >> 3, D = D0 + ((dD * (int32_t)i) >> 6);
            int32_t y = echo_read(w, D), v, e;
            xp = q[3];
            x = mulq15(clamp(x, -65535, 65535), DL_IN);
            if (pp) {                           /* PING-PONG: left TIME back, right (and the loop) 2 TIME */
                int32_t y2 = echo_read(w, 2 * D), yr = mulq15(y2, f1);   /* (the right's echo: one FEEDBACK after
                                                 * the left's) */
                dl.l[i] = y + mulq15(yr, po);
                dl.r[i] = yr + mulq15(y, po);
                y = y2;
            } else {
                dl.l[i] = y;
                dl.r[i] = echo_read(w, D + DL_SPREAD);
            }
            e = (y - lp) * tc + le, le = e & 32767, lp += e >> 15;          /* darker each time round */
            v = sh_tz(lp * fb, 15);                                         /* FEEDBACK */
            v = wear_sat(v, g, ig);                                         /* the tape's saturation (wear.c) */
            e = (v - dc) * DL_DCK + de, de = e & 32767, dc += e >> 15;      /* the DC block */
            v = clamp(soft_knee(x + v - dc, 16384), -32767, 32767);         /* in with the input: never clips */
            dl_buf[w] = (int16_t)v;
            dl.quiet = (v | x) ? 0u : dl.quiet + 1u;
            if (++w == DL_LEN) w = 0;
        }
    }
    dl.w = w, dl.m = m1, dl.x = xp, dl.lp = lp, dl.le = le, dl.dc = dc, dl.de = de;

    /* back to 44.1 kHz, between samples, at the return's level (MIX), into out and into the REV bus */
    {
        int32_t wg = mix * 258;
        for (i = 0; i < DL_NC; i++) {
            int32_t wl = dl.l[i], wr = dl.r[i], *o = out + 2u * DL_D * i, k;
            for (k = 0; k < (int32_t)DL_D; k++) {   /* (from the last toward this one, a quarter on each) */
                int32_t a = 2 * mulq15((ml * (3 - k) + wl * (k + 1)) >> 2, wg), b = 2 * mulq15((mr * (3 - k) + wr * (k + 1)) >> 2, wg);   /* (x 2) */
                o[2 * k] += a, o[2 * k + 1] += b;
                rev[DL_D * i + (uint32_t)k] += (a + b) >> 1;   /* (the echoes, both sides, into SPACE) */
            }
            ml = wl;
            mr = wr;
        }
    }
    dl.ml = ml, dl.mr = mr;
}

/* the audio interrupt: the send (mono, floats about -1..1, APP_BLOCK = 4 DL_NC frames) -> the echoes added to l, r
 * and to the reverb's send */
static int32_t dl_in[DL_D * DL_NC], dl_out[2u * DL_D * DL_NC], dl_rev[DL_D * DL_NC];
static void echo_render(const float *send, float *l, float *r, float *rev, uint32_t n)
{
    uint32_t i;
    if (n != DL_D * DL_NC)
        return;
    for (i = 0; i < n; i++) {
        dl_in[i] = (int32_t)(fm_clampf(send[i], -1.99f, 1.99f) * 32767.0f);
        dl_out[2u * i] = dl_out[2u * i + 1u] = dl_rev[i] = 0;
    }
    echo_render_q(dl_in, dl_out, dl_rev, n);
    for (i = 0; i < n; i++) {
        l[i] += (float)dl_out[2u * i] * (1.0f / 32767.0f);
        r[i] += (float)dl_out[2u * i + 1u] * (1.0f / 32767.0f);
        rev[i] += (float)dl_rev[i] * (0.5f / 32767.0f);
    }
}
