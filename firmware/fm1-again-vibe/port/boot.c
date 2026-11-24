/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 PolarPatch */
/* FM-1 Again VIBE: the boot screen, as FM-1 Again's (its app/boot.h: Arctic Scream p0larpatch's storm and timing, the
 * glitch of PolarPatch BlueWatch's CONFIG title around the letters), on 480 x 320: FM-1 and AGAIN in the pixfont at six
 * times its size, the four colour lines under them, p0larpatch under those. 5.6 s, or until the first touch; the sound
 * plays from the start (house rule 1). The pixfont: Press Start 2P (SIL OFL 1.1), freestanding/gen/again_pixfont.h.
 * Part of main.c (the control flow). */
#include "gen/again_pixfont.h"

#define BT_MS 5600u
#define BT_S 6                                   /* the pixfont's pixel, in display pixels */
#define BT_Y1 50
#define BT_Y2 (BT_Y1 + 8 * BT_S + 14)
#define BT_LY (BT_Y2 + 8 * BT_S + 24)
#define BT_PY (BT_LY + 22)
static const uint32_t BT_RGB[4] = {0xEAF6FA, 0x22D3EE, 0xFF1A8C, 0xA78BFA};   /* white, then the ghosts: ice, plasma, violet */
static const uint32_t BT_LINE[4] = {0x22D3EE, 0x3FB950, 0xA78BFA, 0xFF1A8C};
static const char *const BT_WORD[2] = {"FM-1", "AGAIN"};
typedef struct { uint8_t from, to, top, bot; } bt_band;
static const struct { uint16_t ms; int8_t dx; uint8_t k, n; bt_band b[3]; } BT_GHOST[3] = {   /* (app/boot.h's) */
    {2600, -7, 1, 3, {{15, 21, 8, 26}, {53, 59, 46, 68}, {86, 92, 70, 88}}},
    {3100, 7, 2, 3, {{23, 29, 32, 48}, {62, 68, 76, 92}, {94, 99, 12, 32}}},
    {4300, 4, 3, 2, {{41, 47, 58, 74}, {79, 85, 20, 38}, {0, 0, 0, 0}}},
};
static struct {
    uint8_t on;
    uint64_t t0;
    uint32_t rnd;
} bt = {1, 0, 0x9E3779B9u};

static uint32_t bt_rnd(uint32_t n)
{
    bt.rnd ^= bt.rnd << 13, bt.rnd ^= bt.rnd >> 17, bt.rnd ^= bt.rnd << 5;
    return bt.rnd % n;
}
/* a word's pixfont rows ya .. yb (of its 8 x BT_S) at y, shifted dx, in rgb */
static void bt_rows(int y, const char *s, int ya, int yb, int dx, uint32_t rgb)
{
    int n = (int)strlen(s), x = (480 - n * 8 * BT_S) / 2 + dx, r, b, b0;
    for (; *s; s++, x += 8 * BT_S) {
        const uint8_t *g = PIXFONT[(uint8_t)*s - 32u];
        for (r = ya / BT_S; r < 8 && r * BT_S < yb; r++) {
            int t = r * BT_S > ya ? r * BT_S : ya, u = r * BT_S + BT_S < yb ? r * BT_S + BT_S : yb;
            for (b = 0; b < 8; b = b0) {
                for (b0 = b; b0 < 8 && g[r] & (0x80u >> b0); b0++)
                    ;
                if (b0 > b)
                    vg_rect(x + b * BT_S, y + t, (b0 - b) * BT_S, u - t, rgb);
                else
                    b0++;
            }
        }
    }
}

static void boot_touch(void) { bt.on = 0; }      /* (a key, a pad, a knob: on to the pages) */

/* 1 while the boot screen is up (it drew the display) */
static int boot_draw(void)
{
    uint32_t e, i, j, w, g = 0, k, pk, ln;
    int th = BT_Y2 + 8 * BT_S - BT_Y1, y[2] = {BT_Y1, BT_Y2};
    if (!bt.on)
        return 0;
    if (!bt.t0)
        bt.t0 = vibe_frames() + 1u;
    e = (uint32_t)((vibe_frames() + 1u - bt.t0) * 1000u / (uint64_t)sr_host);
    if (e >= BT_MS) {
        bt.on = 0;
        return 0;
    }
    if (e < 1700u)
        g = e < 300u ? 64u : 64u - (e - 300u) * 64u / 1400u;   /* the storm */
    k = e < 250u ? 0u : e < 1100u ? (e - 250u) * 64u / 850u : 64u;
    if (g && bt_rnd(80) < g)
        k = k * (1u + bt_rnd(3)) / 4u;
    pk = e < 1300u ? 0u : e < 2100u ? (e - 1300u) * 8u / 800u : 8u;
    ln = e < 1000u ? 0u : e < 1800u ? (e - 1000u) * 88u / 800u : 88u;
    vg_clear(0x000000);
    for (i = 0; i < 3u; i++) {                   /* the ghosts behind: their edges show beside the letters */
        uint32_t pc = (e % BT_GHOST[i].ms) * 100u / BT_GHOST[i].ms;
        int top = 0, bot = 0, dx = BT_GHOST[i].dx * (g ? 3 : 1) * BT_S / 4;
        for (j = 0; j < BT_GHOST[i].n; j++)
            if (pc >= BT_GHOST[i].b[j].from && pc < BT_GHOST[i].b[j].to)
                top = th * BT_GHOST[i].b[j].top / 100, bot = th * BT_GHOST[i].b[j].bot / 100;
        if (g && bt_rnd(3) == 0)
            top = (int)bt_rnd((uint32_t)th - 9u), bot = top + 6 + (int)bt_rnd(15);
        for (w = 0; w < 2u && top < bot; w++) {
            int a = top - (y[w] - y[0]), z = bot - (y[w] - y[0]);
            a = a < 0 ? 0 : a, z = z > 8 * BT_S ? 8 * BT_S : z;
            if (a < z)
                bt_rows(y[w], BT_WORD[w], a, z, dx, vg_mix(0, BT_RGB[BT_GHOST[i].k], (float)k / 64.0f));
        }
    }
    for (w = 0; w < 2u; w++)                     /* the letters, whole but for the storm's tears */
        for (i = 0; i < 16u; i++) {
            int t = g && bt_rnd(64) < g / 3u ? (int)bt_rnd(2u * (2u + g / 4u) + 1u) - (int)(2u + g / 4u) : 0;
            bt_rows(y[w], BT_WORD[w], (int)i * 3, (int)i * 3 + 3, t * BT_S / 4, vg_mix(0, BT_RGB[0], (float)k / 64.0f));
        }
    for (i = 0; i < 4u && ln; i++)               /* four lines, growing from their middles */
        vg_rect(46 + (int)i * 100 + 44 - (int)ln / 2, BT_LY, (int)ln, 3, BT_LINE[i]);
    if (pk) {                                    /* p0larpatch: the pixfont at three times its size */
        const char *s = "p0larpatch";
        int x = 240 - 5 * 24, r, b;
        uint32_t c = vg_mix(0, 0x22D3EE, (float)pk / 8.0f);
        for (; *s; s++, x += 24)
            for (r = 0; r < 8; r++)
                for (b = 0; b < 8; b++)
                    if (PIXFONT[(uint8_t)*s - 32u][r] & (0x80u >> b))
                        vg_rect(x + b * 3, BT_PY + r * 3, 3, 3, c);
    }
    return 1;
}
