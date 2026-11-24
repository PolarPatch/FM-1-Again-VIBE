/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 PolarPatch */
/* RISE: a reversed stab made forwards. A stab that falls in pitch into a room, played backwards, is a rise: its pitch
 * climbing the last ~0.15 s (two octaves up to the note's double octave), its sound swelling, the room a suck of
 * noise toward the hit, then a cut. Made here as it would sound backwards, so it needs no look ahead: the length is
 * known when it starts, and so the end. Two detuned saws through a low-pass that opens toward the end, a sub an
 * octave under, a noise suck (SUCK), BRIGHT the filter's top. Playing, a rise from the keys ends on a beat: the
 * next one at least half its length away (or the next bar, for a bar's rise). rs_start in the interrupt (the queue
 * from the main loop, or the groove's), rs_render adds them. Part of app.c. */

#define RS_NV 3u
static const char *const RS_LEN_NAME[5] = {"1/16", "1/8", "1/4", "1/2", "BAR"};
static const uint8_t RS_LEN_16TH[5] = {1, 2, 4, 8, 16};
enum { RA_OFF, RA_DROP, RA_8BARS, RA_4BARS, NRA };
static const char *const RA_NAME[NRA] = {"OFF", "DROP", "8 BARS", "4 BARS"};

typedef struct {
    uint32_t t, len;                             /* frames since its start; its length (0: free) */
    float f0, ph[3], amp, lp_y;
    svf_t f;
    uint32_t rnd;
} rs_voice_t;

static struct {
    rs_voice_t v[RS_NV];
    uint32_t next;
    uint32_t lenix, autom;                       /* LENGTH (RS_LEN_*), AUTO */
    float bright, suck, level;                   /* 0..1 */
    volatile uint32_t qw, qr;
    uint32_t q[8];
} rs = {.lenix = 2, .bright = 0.6f, .suck = 0.5f, .level = 0.8f};

/* the sub's pitch: an octave under, its own rise a little earlier */
static inline float f0_sub(float f0, float ls) { return f0 * (1.0f + 1.5f * fm_expf(-ls / 0.06f)); }

/* a rise on note, len frames long (the audio interrupt) */
static void rs_start(int32_t note, uint32_t len, float vel)
{
    rs_voice_t *v = &rs.v[rs.next];
    rs.next = (rs.next + 1u) % RS_NV;
    if (len < 2000u)
        len = 2000u;
    v->t = 0;
    v->len = len;
    v->f0 = note_hz((float)note);
    v->amp = 0.4f + 0.6f * vel;
    v->ph[0] = v->ph[1] = v->ph[2] = 0.0f;
    v->lp_y = 0.0f;
    v->rnd = 0x1234567u + (uint32_t)note;
    v->f.ic1 = v->f.ic2 = 0.0f;
}

static void rs_post(int32_t note, uint32_t vel)
{
    uint32_t w = rs.qw;
    if (w - rs.qr >= 8u)
        return;
    rs.q[w & 7u] = (uint32_t)(note & 127) | (vel & 127u) << 8;
    rs.qw = w + 1u;
}

/* a rise from the keys: its length, ending on a beat while the groove plays */
static uint32_t rs_len_now(void)
{
    float six = SR * 15.0f / ck.bpm;
    uint32_t want = RS_LEN_16TH[rs.lenix % 5u], grid = want >= 16u ? 16u : 4u, s, k;
    if (!ck.run)
        return (uint32_t)((float)want * six);
    /* the steps to the next grid line at least half the length away */
    s = (ck.step + 1u) % grid;
    k = (grid - s) % grid;                       /* whole steps after this one to the line */
    while ((float)k + ck.left / ck.len < 0.5f * (float)want)
        k += grid;
    return (uint32_t)(ck.left + (float)k * six);
}

/* n frames into l, r (unpumped) and send */
static void rs_render(float *l, float *r, float *send, uint32_t n)
{
    uint32_t i, j;
    while (rs.qr != rs.qw) {
        uint32_t e = rs.q[rs.qr & 7u];
        rs.qr++;
        rs_start((int32_t)(e & 127u), rs_len_now(), (float)((e >> 8) & 127u) / 127.0f);
    }
    for (j = 0; j < RS_NV; j++) {
        rs_voice_t *v = &rs.v[j];
        float fc, left_s;
        if (!v->len)
            continue;
        /* once a block: the filter where it is (it opens toward the end) */
        left_s = (float)(v->len > v->t ? v->len - v->t : 0u) / SR;
        fc = 250.0f + (1500.0f + 6000.0f * rs.bright) * fm_expf(-left_s / 0.12f);
        svf_set(&v->f, fc, 1.1f);
        for (i = 0; i < n; i++) {
            float ls = (float)(v->len - v->t) * (1.0f / SR), rise = fm_expf(-ls / 0.09f), f, s, sub, nz, env, cut;
            f = v->f0 * (1.0f + 3.0f * rise);
            v->ph[0] += f / SR;
            v->ph[1] += f * 1.007f / SR;
            v->ph[2] += 0.5f * f0_sub(v->f0, ls) / SR;
            v->ph[0] -= fm_floorf(v->ph[0]);
            v->ph[1] -= fm_floorf(v->ph[1]);
            v->ph[2] -= fm_floorf(v->ph[2]);
            s = svf_run(&v->f, (2.0f * v->ph[0] - 1.0f) + 0.5f * (2.0f * v->ph[1] - 1.0f), 0, 0);
            sub = fm_sin_turns(v->ph[2]);
            env = fm_expf(-ls / 0.25f);           /* the stab's decay, backwards: a swell */
            nz = noise_f(&v->rnd);
            v->lp_y += (nz - v->lp_y) * 0.12f;    /* the suck: dark noise, swelling longer */
            cut = ls < 0.001f ? ls * 1000.0f : 1.0f;   /* (the last millisecond: down to 0) */
            s = (0.55f * s * env + 0.5f * sub * env + rs.suck * 1.6f * v->lp_y * fm_expf(-ls / 0.45f)) * v->amp *
                rs.level * cut * 0.7f;
            l[i] += s;
            r[i] += s;
            send[i] += 0.15f * s;
            if (++v->t >= v->len) {
                v->len = 0;
                break;
            }
        }
    }
}
