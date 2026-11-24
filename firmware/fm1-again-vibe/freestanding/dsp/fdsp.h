/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 PolarPatch */
/* FM-1 Again's float building blocks (the FM-1's single-precision FPU; never a double): the state-variable filter
 * (Andrew Simper's trapezoidal SVF, the published equations), a one-pole, the noise, the polyBLEP saw. Part of app.c
 * (the unity build). */

#define SR 44100.0f

/* the trapezoidal SVF: set its coefficients (fc Hz, q), then run it a sample at a time */
typedef struct {
    float a1, a2, a3, k;                         /* the coefficients */
    float ic1, ic2;                              /* the state */
} svf_t;

static inline void svf_set(svf_t *f, float fc, float q)
{
    float g = fm_tanf(FM_PI * fm_clampf(fc, 10.0f, 0.45f * SR) / SR);
    f->k = 1.0f / q;
    f->a1 = 1.0f / (1.0f + g * (g + f->k));
    f->a2 = g * f->a1;
    f->a3 = g * f->a2;
}
/* one sample: the low-pass out; *bp, *hp the band- and high-pass (either may be 0) */
static inline float svf_run(svf_t *f, float x, float *bp, float *hp)
{
    float v3 = x - f->ic2, v1 = f->a1 * f->ic1 + f->a2 * v3, v2 = f->ic2 + f->a2 * f->ic1 + f->a3 * v3;
    f->ic1 = fm_flush(2.0f * v1 - f->ic1);
    f->ic2 = fm_flush(2.0f * v2 - f->ic2);
    if (bp)
        *bp = v1;
    if (hp)
        *hp = x - f->k * v1 - v2;
    return v2;
}

/* a one-pole's coefficient for a cutoff of fc Hz (the low-pass: y += (x - y) * c) */
static inline float onepole_c(float fc) { return 1.0f - fm_expf(-FM_TWO_PI * fc / SR); }

/* white noise, -1 .. 1 (an LCG: its own seed) */
static inline float noise_f(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return (float)(int32_t)*s * (1.0f / 2147483648.0f);
}

/* a band-limited saw from a phase in turns (0..1) and its step: the polyBLEP's correction at the wrap */
static inline float saw_blep(float ph, float inc)
{
    float s = 2.0f * ph - 1.0f, t;
    if (ph < inc) {
        t = ph / inc;
        s -= t + t - t * t - 1.0f;
    } else if (ph > 1.0f - inc) {
        t = (ph - 1.0f) / inc;
        s -= t * t + t + t + 1.0f;
    }
    return s;
}

/* a MIDI note's frequency */
static inline float note_hz(float note) { return 440.0f * fm_exp2f((note - 69.0f) * (1.0f / 12.0f)); }
