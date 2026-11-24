/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 PolarPatch */
/* KEYS: a soft electric piano, FM of my own (no instrument's patches): a carrier and a modulator at 1:1 whose index
 * falls after the strike (the bark softening into a round tone), a second modulator at 14:1 that rings only a moment
 * (the tine's click), a decay that is shorter up high, a warm low-pass, and a tremolo that pans. One key may play a
 * chord (CHORD: the voicings a one-finger player wants); the groove's PROG plays its chords here too.
 * kp_on / kp_off from the main loop (through a queue the audio interrupt reads), kp_render adds the voices. Part of
 * app.c. */

#define KP_NV 12u                                /* voices */
enum { CH_OFF, CH_MIN7, CH_MAJ7, CH_MIN9, CH_MAJ9, CH_SUS2, CH_ADD9, CH_OCT, NCH };
static const char *const CH_NAME[NCH] = {"SINGLE", "MIN7", "MAJ7", "MIN9", "MAJ9", "SUS2", "ADD9", "OCTAVES"};
static const int8_t CH_IV[NCH][5] = {            /* each chord's notes over the key (-1: none) */
    {0, -1, -1, -1, -1}, {0, 3, 7, 10, -1}, {0, 4, 7, 11, -1}, {0, 3, 10, 14, 19}, {0, 4, 11, 14, 19},
    {0, 2, 7, 12, -1},   {0, 4, 7, 14, -1}, {0, 12, -1, -1, -1}};

typedef struct {
    int8_t note;                                 /* -1: free */
    uint8_t held, src;                           /* the key still down; who started it (0 keys, 1 the groove) */
    uint32_t age;
    float inc, ph_c, ph_m, ph_t;
    float amp, dk, rel;                          /* the level, its decay a frame (held), its release a frame */
    float ix, ix_dk, tine, vel, lp, pan;
} kp_voice_t;

static struct {
    kp_voice_t v[KP_NV];
    uint32_t age;
    float tone, decay, trem, level;              /* 0..1, set by the main loop */
    float lfo;                                   /* the tremolo's phase */
    uint32_t nv;                                 /* the voices allowed (fewer when the CPU is short: app.c) */
    /* the queue from the main loop: note (bit 7: off), velocity, source */
    volatile uint32_t qw, qr;
    uint32_t q[32];
} kp = {.tone = 0.5f, .decay = 0.5f, .trem = 0.3f, .level = 0.7f, .nv = KP_NV};

static void kp_start(int32_t note, float vel, uint32_t src)
{
    uint32_t i, best = 0, oldest = 0xFFFFFFFFu;
    kp_voice_t *v;
    if (note < 12 || note > 115)
        return;
    for (i = 0; i < kp.nv && i < KP_NV; i++) {   /* a free voice, or the quietest released, or the oldest */
        kp_voice_t *w = &kp.v[i];
        uint32_t score = w->note < 0 ? 0u : w->held ? 0x80000000u + w->age : (uint32_t)(w->amp * 1e6f);
        if (w->note < 0) {
            best = i;
            break;
        }
        if (score < oldest) {
            oldest = score;
            best = i;
        }
    }
    v = &kp.v[best];
    v->note = (int8_t)note;
    v->held = 1;
    v->src = (uint8_t)src;
    v->age = ++kp.age;
    v->inc = note_hz((float)note) / SR;
    v->ph_c = v->ph_m = v->ph_t = 0.0f;
    v->vel = vel;
    v->amp = (0.35f + 0.65f * vel) * 0.26f;
    /* the decay: 2 .. 14 s at middle C by DECAY, shorter an octave up */
    {
        float secs = (2.0f + 12.0f * kp.decay * kp.decay) * fm_exp2f(-((float)note - 60.0f) / 24.0f);
        v->dk = fm_expf(-6.9f / (secs * SR));
    }
    v->rel = fm_expf(-6.9f / (0.35f * SR));
    v->ix = (0.25f + 0.9f * vel) * (0.4f + 1.2f * kp.tone);    /* the bark: turns of phase */
    v->ix_dk = fm_expf(-1.0f / (0.22f * SR));
    v->tine = (0.08f + 0.35f * vel) * (0.3f + kp.tone);
    v->pan = 0.5f + 0.25f * (float)(((note * 7) % 12) - 6) / 6.0f;
}

static void kp_stop(int32_t note, uint32_t src)
{
    uint32_t i;
    for (i = 0; i < KP_NV; i++)
        if (kp.v[i].note == note && kp.v[i].held && kp.v[i].src == src)
            kp.v[i].held = 0;
}

/* the main loop: a note on or off, by the queue (the groove, in the interrupt, calls kp_start / kp_stop) */
static void kp_post(int32_t note, uint32_t vel, uint32_t src, int on)
{
    uint32_t w = kp.qw;
    if (w - kp.qr >= 32u)
        return;
    kp.q[w & 31u] = (uint32_t)(note & 127) | (on ? 0u : 0x80u) | (vel & 127u) << 8 | src << 16;
    kp.qw = w + 1u;
}

/* n frames of the voices into l, r (the pumped pair) and send */
static void kp_render(float *l, float *r, float *send, uint32_t n)
{
    uint32_t i, j;
    float lfo_inc = 4.6f / SR, lpc = onepole_c(1200.0f + 9000.0f * kp.tone * kp.tone), lvl = kp.level;
    while (kp.qr != kp.qw) {                     /* the queue */
        uint32_t e = kp.q[kp.qr & 31u];
        kp.qr++;
        if (e & 0x80u)
            kp_stop((int32_t)(e & 127u), e >> 16);
        else
            kp_start((int32_t)(e & 127u), (float)((e >> 8) & 127u) / 127.0f, e >> 16);
    }
    for (j = 0; j < KP_NV; j++) {
        kp_voice_t *v = &kp.v[j];
        float ph = kp.lfo;
        if (v->note < 0)
            continue;
        for (i = 0; i < n; i++) {
            float m = fm_sin_turns(v->ph_m) * v->ix, t = fm_sin_turns(v->ph_t) * v->tine, s, tr;
            s = fm_sin_turns(v->ph_c + m + t);
            v->ph_c += v->inc;
            v->ph_m += v->inc;
            v->ph_t += 14.0f * v->inc;
            v->ph_c -= v->ph_c >= 1.0f ? 1.0f : 0.0f;
            v->ph_m -= v->ph_m >= 1.0f ? 1.0f : 0.0f;
            v->ph_t -= fm_floorf(v->ph_t);
            v->ix = 0.18f * (0.4f + kp.tone) + (v->ix - 0.18f * (0.4f + kp.tone)) * v->ix_dk;
            v->tine *= 0.9985f;
            v->lp += (s - v->lp) * lpc;
            s = v->lp * v->amp * lvl;
            v->amp *= v->held ? v->dk : v->rel;
            tr = kp.trem * 0.5f * fm_sin_turns(ph);
            ph += lfo_inc;
            l[i] += s * (1.0f - v->pan + tr);
            r[i] += s * (v->pan - tr);
            send[i] += s * 0.35f;
        }
        if (v->amp < 1e-5f)
            v->note = -1;
    }
    kp.lfo += lfo_inc * (float)n;
    kp.lfo -= fm_floorf(kp.lfo);
}
