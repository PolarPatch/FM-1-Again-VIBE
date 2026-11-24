/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 PolarPatch */
/* PERFORM: tricks over the whole sound while SEL is held with a white key (several at once). The idea is Arctic
 * Scream's FX performance layer (its perform.c, GR!MM's and mine: a history of the output played back round and
 * round, a tape stop, filter throws, send throws); written anew here in floats for FM-1 Again, none of its code.
 *   STUTTER 1/16, 1/8, 1/4: the last sixteenth (eighth, beat) at the tempo, round and round
 *   REVERSE: the last beat backwards, round and round
 *   TAPE STOP: the sound slows to a stop over a second, its pitch falling
 *   LOW SWEEP, HIGH SWEEP: a low-pass closing (a high-pass opening) over two seconds; let go: back in half a second
 *   ECHO THROW, SPACE THROW: the keys and voices sent full into ECHO (SPACE): the tails ring on after
 *   KICK OUT, DRUMS OUT: the kick (every drum) gone while held: the breakdown
 *   STUTTER 1/32: a roll (loaf-breaker's R32)
 *   GATE: the sound cut to sixteenths, half on, half off (loaf-breaker's GAT)
 * The black keys are pads for eleven of them (app.c PAD_TRICK, after loaf-breaker's pads); SEL and a white key for
 * any.
 * The history: the last half second of the mix (before TAPE), stereo, 16 bits, in the pool (88 KB), held still while
 * a loop plays from it (STUTTER 1/4 under 120 BPM: its last half second). Into a trick and
 * out of it: ~10 ms crossfades; a loop's ends: ~1.5 ms fades. pfx.held from the main loop; pf_* in the audio
 * interrupt. Part of app.c. */

enum { PF_STUT16, PF_STUT8, PF_STUT4, PF_REVERSE, PF_TAPESTOP, PF_LOWSWEEP, PF_HIGHSWEEP, PF_ECHOTHROW,
       PF_SPACETHROW, PF_KICKOUT, PF_DRUMSOUT, PF_STUT32, PF_GATE, NPF };
static const char *const PF_NAME[NPF] = {"STUTTER 1/16", "STUTTER 1/8", "STUTTER 1/4", "REVERSE", "TAPE STOP",
                                         "LOW SWEEP", "HIGH SWEEP", "ECHO THROW", "SPACE THROW", "KICK OUT",
                                         "DRUMS OUT", "STUTTER 1/32", "GATE"};
#define PF_LEN 22050u                            /* the history: 0.5 s */
#define PF_EDGE 64u                              /* a loop's ends fade (frames) */
enum { PR_LIVE, PR_LOOP, PR_BACK, PR_TAPE };

static int16_t pf_buf[PF_LEN][2] PLAT_POOL;
static struct {
    volatile uint32_t held;                      /* the tricks held (bit PF_*), from the main loop */
    uint32_t w;                                  /* the next frame written */
    uint32_t mode;                               /* PR_*: what the reader plays */
    float mix;                                   /* the reader's share, 0 .. 1 */
    float rp, v;                                 /* the reader's place (frames into the history, from its start), its speed */
    uint32_t ls, ll;                             /* a loop: its start (REVERSE: its end) and length */
    uint32_t t;                                  /* frames into the loop */
    float lp_c, hp_c;                            /* the sweeps: where the cutoffs are (0 open .. 1 shut) */
    float gate;                                  /* GATE's gain now */
    svf_t lpf[2], hpf[2];
} pfx = {.gate = 1.0f};

static inline float pf_at(float p, uint32_t ch)  /* the history at p (frames, wrapped), between samples */
{
    uint32_t i = (uint32_t)p, j;
    float f = p - (float)i;
    i %= PF_LEN;
    j = i + 1u == PF_LEN ? 0u : i + 1u;
    return ((float)pf_buf[i][ch] + ((float)pf_buf[j][ch] - (float)pf_buf[i][ch]) * f) * (1.0f / 32767.0f);
}

/* the reader's trick, from the tricks held (the first that reads the history wins) */
static uint32_t pf_reader(uint32_t h, uint32_t *len)
{
    float six = SR * 15.0f / ck.bpm;
    if (h & 1u << PF_STUT32) { *len = (uint32_t)(0.5f * six); return PR_LOOP; }
    if (h & 1u << PF_STUT16) { *len = (uint32_t)six; return PR_LOOP; }
    if (h & 1u << PF_STUT8) { *len = (uint32_t)(2.0f * six); return PR_LOOP; }
    if (h & 1u << PF_STUT4) { *len = (uint32_t)(4.0f * six); return PR_LOOP; }
    if (h & 1u << PF_REVERSE) { *len = (uint32_t)(4.0f * six); return PR_BACK; }
    if (h & 1u << PF_TAPESTOP) { *len = 0; return PR_TAPE; }
    *len = 0;
    return PR_LIVE;
}

/* the audio interrupt: the mix l, r (n frames) in place; more: tricks the groove plays (its build's STUTTER) */
static void pf_render(float *l, float *r, uint32_t n, uint32_t more)
{
    uint32_t h = pfx.held | more, i, len, want = pf_reader(h, &len);
    float gph = 1.0f - ck.left / ck.len, gstep = 1.0f / ck.len;   /* GATE: where in the sixteenth */
    float lpt = h & 1u << PF_LOWSWEEP ? 1.0f : 0.0f, hpt = h & 1u << PF_HIGHSWEEP ? 1.0f : 0.0f;
    if (len > PF_LEN - 1u)
        len = PF_LEN - 1u;
    if (want != pfx.mode && pfx.mix <= 0.0f) {     /* a new trick (from the live sound): where it starts */
        pfx.mode = want;
        pfx.ll = len;
        pfx.t = 0;
        pfx.ls = (pfx.w + PF_LEN - len) % PF_LEN;  /* LOOP: the last len; BACK: from now backwards */
        pfx.rp = (float)((pfx.w + PF_LEN - 2u) % PF_LEN);
        pfx.v = 1.0f;
    }
    /* the sweeps: towards shut while held (2 s), open again after (0.5 s); once a block */
    pfx.lp_c += (lpt - pfx.lp_c) * (lpt > pfx.lp_c ? 0.0029f : 0.0116f) * 2.0f;
    pfx.hp_c += (hpt - pfx.hp_c) * (hpt > pfx.hp_c ? 0.0029f : 0.0116f) * 2.0f;
    if (pfx.lp_c > 0.002f) {
        svf_set(&pfx.lpf[0], 20000.0f * fm_exp2f(-6.6f * pfx.lp_c), 1.4f);
        pfx.lpf[1].a1 = pfx.lpf[0].a1, pfx.lpf[1].a2 = pfx.lpf[0].a2, pfx.lpf[1].a3 = pfx.lpf[0].a3, pfx.lpf[1].k = pfx.lpf[0].k;
    }
    if (pfx.hp_c > 0.002f) {
        svf_set(&pfx.hpf[0], 20.0f * fm_exp2f(6.6f * pfx.hp_c), 1.4f);
        pfx.hpf[1].a1 = pfx.hpf[0].a1, pfx.hpf[1].a2 = pfx.hpf[0].a2, pfx.hpf[1].a3 = pfx.hpf[0].a3, pfx.hpf[1].k = pfx.hpf[0].k;
    }
    for (i = 0; i < n; i++) {
        float a = l[i], b = r[i], hp;
        /* the history: the live sound (held still while a loop plays from it) */
        if (pfx.mode == PR_LIVE || pfx.mode == PR_TAPE) {
            pf_buf[pfx.w][0] = (int16_t)(fm_clampf(a, -1.0f, 1.0f) * 32767.0f);
            pf_buf[pfx.w][1] = (int16_t)(fm_clampf(b, -1.0f, 1.0f) * 32767.0f);
            pfx.w = pfx.w + 1u == PF_LEN ? 0u : pfx.w + 1u;
        }
        /* the reader: in while its trick is wanted, out when it is not (or another is) */
        pfx.mix += want != PR_LIVE && want == pfx.mode ? (pfx.mix < 1.0f ? 0.0023f : 0.0f) : (pfx.mix > 0.0f ? -0.0023f : 0.0f);
        pfx.mix = fm_clampf(pfx.mix, 0.0f, 1.0f);
        if (pfx.mix > 0.0f && pfx.mode != PR_LIVE) {
            float x, y, e = 1.0f, p;
            if (pfx.mode == PR_TAPE) {            /* slower and slower: the history read further and further behind */
                p = pfx.rp;
                pfx.v = pfx.v > 0.0f ? pfx.v - 1.4f / SR : 0.0f;   /* (stopped in 0.7 s: 0.36 s behind at most) */
                pfx.rp += pfx.v;
                if (pfx.rp >= (float)PF_LEN)
                    pfx.rp -= (float)PF_LEN;
                e = pfx.v;
            } else {
                uint32_t t = pfx.t % pfx.ll;
                p = pfx.mode == PR_LOOP ? (float)((pfx.ls + t) % PF_LEN) : (float)((pfx.ls + pfx.ll - 1u - t) % PF_LEN);
                e = t < PF_EDGE ? (float)t / PF_EDGE : t > pfx.ll - PF_EDGE ? (float)(pfx.ll - t) / PF_EDGE : 1.0f;
                pfx.t++;
            }
            x = pf_at(p, 0) * e;
            y = pf_at(p, 1) * e;
            a += (x - a) * pfx.mix;
            b += (y - b) * pfx.mix;
        }
        if (pfx.lp_c > 0.002f) {
            a = svf_run(&pfx.lpf[0], a, 0, 0);
            b = svf_run(&pfx.lpf[1], b, 0, 0);
        }
        if (pfx.hp_c > 0.002f) {
            svf_run(&pfx.hpf[0], a, 0, &hp);
            a = hp;
            svf_run(&pfx.hpf[1], b, 0, &hp);
            b = hp;
        }
        if ((h & 1u << PF_GATE) || pfx.gate < 1.0f) {   /* GATE: on the first half of each sixteenth */
            float g = !(h & 1u << PF_GATE) || gph - fm_floorf(gph) < 0.5f ? 1.0f : 0.0f;
            pfx.gate += (g - pfx.gate) * 0.02f;
            a *= pfx.gate;
            b *= pfx.gate;
        }
        gph += gstep;
        l[i] = a;
        r[i] = b;
    }
    if (pfx.mix <= 0.0f && want == PR_LIVE)
        pfx.mode = PR_LIVE;
}
