/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 PolarPatch */
/* The drums, all synthesized (no samples): a warm house kick, a clap, a snare for ghosts, closed and open hats from
 * six square waves (the 808's way: a metallic cluster through a high-pass), a shaker and a rim. dr_hit starts one
 * (the groove, the keys in DRUMS, the build's roll); dr_render adds them, the kick to its own pair (it is never
 * pumped) and the rest to the drum pair, and the clap and snare to the reverb's send. Part of app.c. */

enum { DR_KICK, DR_CLAP, DR_SNARE, DR_HAT, DR_OPEN, DR_SHAKER, DR_RIM, NDR };
static const char *const DR_NAME[NDR] = {"KICK", "CLAP", "SNARE", "HAT", "OPEN HAT", "SHAKER", "RIM"};

static struct {
    float kick_ph, kick_f, kick_a, kick_click;
    float kick_tune;                             /* the kick's low note (Hz) */
    float kick_dk;                               /* its decay a frame */
    uint32_t clap_t;                             /* frames since the clap (0xFFFFFFFF: none) */
    float clap_v;
    float sn_ph, sn_a, sn_n;                     /* the snare: its tone and noise */
    float hat_a, open_a, sh_a, sh_env, rim_a, rim_ph1, rim_ph2;
    float hat_pan;
    float mph[6];                                /* the metal's six squares */
    svf_t clap_f, sn_f, hat_f, sh_f;
    uint32_t rnd;
    volatile uint8_t pend[NDR];                  /* hits waiting (velocity 1..127), from the main loop */
} dr;

static const float DR_METAL[6] = {205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f};

static void dr_init(void)
{
    dr.clap_t = 0xFFFFFFFFu;
    dr.rnd = 0x2545F491u;
    dr.kick_tune = 48.0f;
    dr.kick_dk = 0.99978f;
    svf_set(&dr.clap_f, 1250.0f, 1.6f);
    svf_set(&dr.sn_f, 3200.0f, 0.8f);
    svf_set(&dr.hat_f, 7600.0f, 0.9f);
    svf_set(&dr.sh_f, 6800.0f, 1.3f);
}

/* a hit now (audio interrupt), v 0..1 */
static void dr_hit(uint32_t d, float v)
{
    switch (d) {
    case DR_KICK:
        dr.kick_ph = 0.0f;
        dr.kick_f = 170.0f;
        dr.kick_a = v;
        dr.kick_click = v;
        break;
    case DR_CLAP:
        dr.clap_t = 0;
        dr.clap_v = v;
        break;
    case DR_SNARE:
        dr.sn_ph = 0.0f;
        dr.sn_a = v;
        dr.sn_n = v;
        break;
    case DR_HAT:
        dr.hat_a = v;
        dr.open_a = 0.0f;                        /* the closed hat chokes the open one */
        dr.hat_pan = dr.hat_pan > 0.5f ? 0.38f : 0.62f;
        break;
    case DR_OPEN:
        dr.open_a = v;
        break;
    case DR_SHAKER:
        dr.sh_a = v;
        dr.sh_env = 0.0f;
        break;
    case DR_RIM:
        dr.rim_a = v;
        dr.rim_ph1 = dr.rim_ph2 = 0.0f;
        break;
    }
}

/* n frames: the kick into kl/kr, the rest into l/r, the clap and snare also into send. lv: each drum's level */
static void dr_render(float *kl, float *kr, float *l, float *r, float *send, const float *lv, uint32_t n)
{
    uint32_t i, d;
    for (d = 0; d < NDR; d++) {                  /* the main loop's hits (the keys in DRUMS) */
        uint32_t v = dr.pend[d];
        if (v) {
            dr.pend[d] = 0;
            dr_hit(d, (float)v / 127.0f);
        }
    }
    for (i = 0; i < n; i++) {
        float k = 0.0f, x = noise_f(&dr.rnd), s = 0.0f, rv = 0.0f, metal = 0.0f, hp, bp;
        /* the kick: a sine falling from 170 Hz to its note, a soft click, a little drive */
        if (dr.kick_a > 1e-4f) {
            dr.kick_f = dr.kick_tune + (dr.kick_f - dr.kick_tune) * 0.9988f;
            dr.kick_ph += dr.kick_f / SR;
            if (dr.kick_ph >= 1.0f)
                dr.kick_ph -= 1.0f;
            k = fm_tanhf(1.6f * fm_sin_turns(dr.kick_ph) * dr.kick_a) * lv[DR_KICK];
            k += x * dr.kick_click * 0.12f * lv[DR_KICK];
            dr.kick_click *= 0.985f;
            dr.kick_a *= dr.kick_dk;
        }
        /* the clap: three quick bursts of band-passed noise, then its tail */
        if (dr.clap_t != 0xFFFFFFFFu) {
            uint32_t t = dr.clap_t++;
            float e = t < 1323u ? fm_expf(-(float)(t % 441u) * (1.0f / 70.0f))
                                : 0.7f * fm_expf(-(float)(t - 1323u) * (1.0f / 3500.0f));
            float c;
            svf_run(&dr.clap_f, x, &c, 0);
            c *= e * dr.clap_v * 2.2f * lv[DR_CLAP];
            s += c;
            rv += c;
            if (t > 30000u)
                dr.clap_t = 0xFFFFFFFFu;
        }
        /* the snare: a 185 Hz body and high-passed noise, short (ghost notes) */
        if (dr.sn_a > 1e-4f || dr.sn_n > 1e-4f) {
            float c;
            dr.sn_ph += 185.0f / SR;
            if (dr.sn_ph >= 1.0f)
                dr.sn_ph -= 1.0f;
            svf_run(&dr.sn_f, x, 0, &hp);
            c = (fm_sin_turns(dr.sn_ph) * dr.sn_a * 0.6f + hp * dr.sn_n * 0.7f) * lv[DR_SNARE];
            dr.sn_a *= 0.9982f;
            dr.sn_n *= 0.9990f;
            s += c;
            rv += 0.5f * c;
        }
        /* the hats: six squares (and a little noise) through a high-pass */
        if (dr.hat_a > 1e-4f || dr.open_a > 1e-4f) {
            uint32_t m;
            float h;
            for (m = 0; m < 6u; m++) {
                dr.mph[m] += DR_METAL[m] * 1.7f / SR;
                if (dr.mph[m] >= 1.0f)
                    dr.mph[m] -= 1.0f;
                metal += dr.mph[m] < 0.5f ? 1.0f : -1.0f;
            }
            svf_run(&dr.hat_f, metal * 0.16f + 0.3f * x, 0, &hp);
            h = hp * (dr.hat_a * lv[DR_HAT] + dr.open_a * lv[DR_OPEN]) * 0.9f;
            dr.hat_a *= 0.9988f;
            dr.open_a *= 0.99985f;
            l[i] += h * (1.0f - dr.hat_pan) * 2.0f * 0.5f;
            r[i] += h * dr.hat_pan * 2.0f * 0.5f;
        }
        /* the shaker: band-passed noise, a soft attack */
        if (dr.sh_a > 1e-4f) {
            dr.sh_env += (1.0f - dr.sh_env) * 0.02f;
            svf_run(&dr.sh_f, x, &bp, 0);
            l[i] += bp * dr.sh_a * dr.sh_env * 0.55f * lv[DR_SHAKER] * 0.7f;
            r[i] += bp * dr.sh_a * dr.sh_env * 0.55f * lv[DR_SHAKER] * 1.3f;
            dr.sh_a *= 0.9992f;
        }
        /* the rim: two short sines, a woodblock's knock */
        if (dr.rim_a > 1e-4f) {
            dr.rim_ph1 += 1720.0f / SR;
            dr.rim_ph2 += 540.0f / SR;
            s += (fm_sin_turns(dr.rim_ph1) + 0.6f * fm_sin_turns(dr.rim_ph2)) * dr.rim_a * 0.35f * lv[DR_RIM];
            dr.rim_a *= 0.9965f;
        }
        kl[i] += k;
        kr[i] += k;
        l[i] += s;
        r[i] += s;
        send[i] += rv * 0.5f;
    }
}
