/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 PolarPatch */
/* VOX: chopped, pitched voice without a recording: each key sings a short syllable, a buzz (a band-limited saw and a
 * breath of noise) through three formants that glide from one vowel to the next, scooped up into its pitch, cut off
 * after GATE. Each key has its own syllable (the set: VOWELS), so a run over the keys sounds like a voice cut into
 * slices. Until CHOP plays recordings, this is what the keys chop. vx_post from the main loop, vx_render in the
 * interrupt. The formant frequencies are the usual published values for a soprano's vowels. Part of app.c. */

#define VX_NV 4u
enum { VW_A, VW_E, VW_I, VW_O, VW_U, NVW };
static const float VW_F[NVW][3] = {{800, 1150, 2900}, {350, 2000, 2800}, {270, 2140, 2950}, {450, 800, 2830},
                                   {325, 700, 2700}};
static const float VW_G[NVW][3] = {{1.0f, 0.5f, 0.18f}, {1.0f, 0.2f, 0.18f}, {1.0f, 0.25f, 0.2f},
                                   {1.0f, 0.3f, 0.14f}, {1.0f, 0.2f, 0.1f}};
/* the syllables (from, to) a set gives the keys, round the octave */
#define VX_NSET 4
static const char *const VX_SET_NAME[VX_NSET] = {"OOH AH", "OH EH", "AH EE", "ALL"};
static const uint8_t VX_SYL[VX_NSET][12][2] = {
    {{VW_U, VW_A}, {VW_A, VW_A}, {VW_U, VW_U}, {VW_A, VW_O}, {VW_O, VW_U}, {VW_U, VW_A}, {VW_A, VW_U},
     {VW_U, VW_O}, {VW_A, VW_A}, {VW_O, VW_A}, {VW_U, VW_U}, {VW_A, VW_O}},
    {{VW_O, VW_E}, {VW_E, VW_E}, {VW_O, VW_O}, {VW_E, VW_O}, {VW_O, VW_E}, {VW_E, VW_I}, {VW_O, VW_U},
     {VW_E, VW_E}, {VW_O, VW_A}, {VW_E, VW_O}, {VW_O, VW_O}, {VW_E, VW_A}},
    {{VW_A, VW_I}, {VW_I, VW_I}, {VW_A, VW_A}, {VW_I, VW_A}, {VW_A, VW_E}, {VW_E, VW_I}, {VW_A, VW_I},
     {VW_I, VW_E}, {VW_A, VW_A}, {VW_I, VW_A}, {VW_E, VW_E}, {VW_A, VW_I}},
    {{VW_A, VW_A}, {VW_E, VW_E}, {VW_I, VW_I}, {VW_O, VW_O}, {VW_U, VW_U}, {VW_U, VW_A}, {VW_A, VW_I},
     {VW_O, VW_E}, {VW_E, VW_U}, {VW_I, VW_O}, {VW_A, VW_U}, {VW_O, VW_A}}};

typedef struct {
    int8_t note;
    uint8_t held;
    uint32_t t, gate;                            /* frames since the start; the cut */
    float ph, inc, scoop, vel, amp, glide;
    uint8_t from, to;
    svf_t f[3];
    float g[3];
} vx_voice_t;

static struct {
    vx_voice_t v[VX_NV];
    uint32_t next, rnd;
    uint32_t set;                                /* VOWELS */
    float gate_s, shift, breath, level;          /* GATE (s), FORMANT (a factor), BREATH, LEVEL */
    volatile uint32_t qw, qr;
    uint32_t q[16];
} vx = {.gate_s = 0.18f, .shift = 1.0f, .breath = 0.2f, .level = 0.7f, .rnd = 77u};

static void vx_post(int32_t note, uint32_t vel, int on)
{
    uint32_t w = vx.qw;
    if (w - vx.qr >= 16u)
        return;
    vx.q[w & 15u] = (uint32_t)(note & 127) | (on ? 0u : 0x80u) | (vel & 127u) << 8;
    vx.qw = w + 1u;
}

static void vx_start(int32_t note, float vel)
{
    vx_voice_t *v = &vx.v[vx.next];
    const uint8_t *syl = VX_SYL[vx.set % VX_NSET][note % 12];
    vx.next = (vx.next + 1u) % VX_NV;
    v->note = (int8_t)note;
    v->held = 1;
    v->t = 0;
    v->gate = (uint32_t)(vx.gate_s * SR);
    v->inc = note_hz((float)note) / SR;
    v->scoop = -1.2f;                            /* a semitone and a bit under, up into the note */
    v->vel = 0.4f + 0.6f * vel;
    v->amp = 0.0f;
    v->from = syl[0];
    v->to = syl[1];
    v->glide = 0.0f;
}

/* n frames into l, r and send */
static void vx_render(float *l, float *r, float *send, uint32_t n)
{
    uint32_t i, j, k;
    while (vx.qr != vx.qw) {
        uint32_t e = vx.q[vx.qr & 15u];
        vx.qr++;
        if (e & 0x80u) {
            for (j = 0; j < VX_NV; j++)
                if (vx.v[j].note == (int8_t)(e & 127u))
                    vx.v[j].held = 0;
        } else {
            vx_start((int32_t)(e & 127u), (float)((e >> 8) & 127u) / 127.0f);
        }
    }
    for (j = 0; j < VX_NV; j++) {
        vx_voice_t *v = &vx.v[j];
        float inc, tgt, ga = fm_minf(1.0f, (float)v->t / (0.25f * SR));
        if (v->note < 0)
            continue;
        /* once a block: the formants where the glide is, the pitch where the scoop is */
        v->glide = ga * ga * (3.0f - 2.0f * ga);
        for (k = 0; k < 3u; k++) {
            float f = VW_F[v->from][k] + (VW_F[v->to][k] - VW_F[v->from][k]) * v->glide;
            svf_set(&v->f[k], f * vx.shift, k ? 9.0f : 6.0f);
            v->g[k] = VW_G[v->from][k] + (VW_G[v->to][k] - VW_G[v->from][k]) * v->glide;
        }
        v->scoop *= 0.6f;
        inc = v->inc * fm_exp2f(v->scoop / 12.0f);
        for (i = 0; i < n; i++) {
            float src, s = 0.0f, bp;
            int open = v->t < v->gate && (v->held || v->t < (uint32_t)(0.06f * SR));
            tgt = open ? v->vel : 0.0f;
            v->amp += (tgt - v->amp) * (open ? 0.006f : 0.0025f);
            src = saw_blep(v->ph, inc) * (1.0f - vx.breath) + noise_f(&vx.rnd) * vx.breath * 1.5f;
            v->ph += inc;
            v->ph -= v->ph >= 1.0f ? 1.0f : 0.0f;
            for (k = 0; k < 3u; k++) {
                svf_run(&v->f[k], src, &bp, 0);
                s += bp * v->g[k];
            }
            s *= v->amp * vx.level * 0.5f;
            l[i] += s;
            r[i] += s;
            send[i] += s * 0.5f;
            v->t++;
        }
        if (v->t > v->gate + (uint32_t)(0.2f * SR) || (!v->held && v->amp < 1e-4f && v->t > 4000u))
            v->note = -1;
    }
}
