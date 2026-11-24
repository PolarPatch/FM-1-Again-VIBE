/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 PolarPatch */
/* PUMP: the sidechain. Each kick (SOURCE KICK), or each beat whether a kick plays or not (SOURCE BEAT), pulls the
 * pumped sound down by DEPTH in 3 ms and lets it back up over RELEASE along SHAPE's curve (a straight line at 0, a
 * slow start up to 1: the long swell of a house pump). Everything but the kick and the hats is pumped: the keys, the
 * voice, the reverb's return. pm_kick from the groove, pm_gain once a frame. Part of app.c. */

enum { PS_KICK, PS_BEAT, PS_OFF, NPS };
static const char *const PS_NAME[NPS] = {"KICK", "BEAT", "OFF"};

static struct {
    float depth, rel_s, shape;                   /* DEPTH 0..1, RELEASE seconds, SHAPE 0..1 */
    uint32_t src;
    uint32_t t;                                  /* frames since the last pull (big: none) */
    float now;                                   /* the gain now (shown) */
} pm = {0.7f, 0.22f, 0.5f, PS_KICK, 0x7FFFFFFFu, 1.0f};

static inline void pm_pull(void) { pm.t = 0; }

/* the audio interrupt: the gain this frame */
static inline float pm_gain(void)
{
    float e, x;
    uint32_t t = pm.t;
    if (pm.src == PS_OFF || t > 0x40000000u) {
        pm.now = 1.0f;
        return 1.0f;
    }
    pm.t = t + 1u;
    if (t < 132u)                                /* 3 ms down */
        e = (float)t / 132.0f;
    else {
        x = (float)(t - 132u) / (pm.rel_s * SR);
        if (x >= 1.0f) {
            pm.t = 0x7FFFFFFFu;
            e = 0.0f;
        } else {
            float y = 1.0f - x;                  /* how much of the pull is left: a line, or a curve that holds longer */
            e = y * (1.0f - pm.shape) + y * y * (3.0f - 2.0f * y) * pm.shape;
        }
    }
    pm.now = 1.0f - pm.depth * e;
    return pm.now;
}
