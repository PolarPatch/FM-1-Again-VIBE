/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * GR!MM changes: Copyright (C) 2026 GR!MM */
/* Felucca's fixed-point helpers (hugelton/Felucca firmware/src/dsp.c), as Arctic Scream's dsp.c carries them, the few
 * that SPACE (space.h) uses: mulq15, clamp, softclip, soft_knee. Part of app.c (the unity build). */
#include "gen/again_tables.h"

static inline int32_t mulq15(int32_t a, int32_t b) { return (a * b) >> 15; }
/* (clamp: the platform's, plat.h) */

/* tanh(x / 32768) * 32767, linearly interpolated (a plain lookup gives 256-unit steps, audible on quiet signals) */
static inline int32_t softclip(int32_t x)
{
    int32_t a = x < 0 ? -x : x, i = a >> 8, y;
    if (i >= 256)
        y = TANH_Q15[256];                           /* continuous with the table */
    else
        y = TANH_Q15[i] + (((TANH_Q15[i + 1] - TANH_Q15[i]) * (a & 255)) >> 8);
    return x < 0 ? -y : y;
}

/* linear up to k, then a soft knee: only the peaks above k saturate (|y| - k must stay < 2^30) */
static inline int32_t soft_knee(int32_t y, int32_t k)
{
    int32_t a = y < 0 ? -y : y;
    if (a <= k)
        return y;
    a = k + (softclip((a - k) * 2) >> 1);
    return y < 0 ? -a : a;
}
