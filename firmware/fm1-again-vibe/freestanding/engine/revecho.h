/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 PolarPatch */
/* REVERSE ECHO (ECHO's third mode): what was just played, backwards, one TIME later. The send is cut into pieces TIME
 * long (at most half a second); while a piece is written, the last one plays backwards: a note's decay becomes a
 * swell into the next beat. AGAIN feeds the backwards sound into the next piece (echoes of echoes, each turned
 * again), WEAR darkens it; its pieces' edges fade (~3 ms). At 22.05 kHz, mono, two pieces of 16 bits in the pool
 * (44 KB). re_render in the audio interrupt: the send in, the swells added to l, r (pumped) and to the reverb's send.
 * Part of app.c. */

#define RE_MAX 11025u                            /* a piece at most: 0.5 s at 22.05 kHz */
static int16_t re_buf[2][RE_MAX] PLAT_POOL;
static struct {
    uint32_t len, w, cur;                        /* the piece's length now; frames written into piece cur */
    uint32_t half;                               /* (the even / odd frame: 22.05 kHz) */
    float acc, out, prev, lp, fb_s;
} re;

/* the audio interrupt: n frames; len16 the piece in sixteenths, fb 0..1.5, wear 0..1, mix 0..1 */
static void re_render(const float *send, float *l, float *r, float *rev, uint32_t n, float len16, float fb,
                      float wear, float mix)
{
    uint32_t i, len = (uint32_t)(len16 * 0.5f * SR * 15.0f / ck.bpm);
    float c = 0.9f - 0.85f * wear;               /* the low-pass, a 22.05 kHz frame */
    if (len > RE_MAX)
        len = RE_MAX;
    if (len < 512u)
        len = 512u;
    if (!re.len)
        re.len = len;
    for (i = 0; i < n; i++) {
        float y;
        re.acc += send[i];
        if (re.half ^= 1u) {                     /* every other frame: one 22.05 kHz frame read and written */
            const int16_t *b = re_buf[re.cur ^ 1u];
            uint32_t k = re.w, edge = re.len - 1u - k;
            float e = k < 64u ? (float)k / 64.0f : edge < 64u ? (float)edge / 64.0f : 1.0f, x;
            re.prev = re.out;
            x = k < re.len ? (float)b[re.len - 1u - k] * (1.0f / 32767.0f) * e : 0.0f;   /* the last piece, backwards */
            re.lp += (x - re.lp) * c;
            re.out = re.lp;
            re_buf[re.cur][k] = (int16_t)(fm_clampf(0.5f * re.acc + fb * re.out, -1.0f, 1.0f) * 32767.0f);
            re.acc = 0.0f;
            if (++re.w >= re.len) {              /* the piece is full: it plays next, backwards */
                re.w = 0;
                re.cur ^= 1u;
                re.len = len;                    /* (TIME's new length: from the next piece) */
            }
        }
        y = (re.half ? re.prev + (re.out - re.prev) * 0.5f : re.out) * mix;
        l[i] += y;
        r[i] += y;
        rev[i] += 0.6f * y;
    }
}
