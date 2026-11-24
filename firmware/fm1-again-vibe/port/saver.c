/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 PolarPatch */
/* FM-1 Again VIBE: the screensaver, as the FM-1's (its app/saver.h): after a minute untouched, the crowd at a concert,
 * dithered black and white, plays over the whole display, round and round, while the music plays on; the first touch
 * only wakes the display. SETUP's SAVER turns it off. The film and its decoder are FM-1 Again's (freestanding/film.h,
 * gen/again_film.h: the USB002 tour's creative drop's footage, not GPL); here its 240 x 240 at twice the size, the
 * middle 160 rows filling the 480 x 320. Part of main.c (the control flow). */
#include "film.h"
#include "gen/again_film.h"

static uint64_t sv_touched;                      /* the last touch, in the host's frames */
static uint64_t sv_shown;                        /* when the frame shown came, in the host's frames */

static void saver_init(void) { sv_film(AGAIN_FILM, (uint32_t)sizeof AGAIN_FILM); }

/* (vibe_on_event) a touch: 1 when it only woke the display (and is taken) */
static int saver_touch(const vibe_event *e)
{
    int was = sv.on;
    if (!(e->type == VIBE_EV_NOTE || e->type == VIBE_EV_KEY || e->type == VIBE_EV_PAD || e->type == VIBE_EV_BUTTON ||
          e->type == VIBE_EV_ENCODER || e->type == VIBE_EV_TOUCH))
        return 0;
    sv_touched = vibe_frames();
    sv.on = 0;
    return was && e->down;                       /* (a key or button let go after waking: it still lets go) */
}

/* (vibe_on_draw) 1 while the film plays (it drew the display) */
static int saver_draw(uint16_t *fb)
{
    uint64_t now = vibe_frames();
    uint32_t y, x;
    if (!sv.p || !set.p[PA_SAVER] || now - sv_touched < (uint64_t)(sr_host * 60.0f)) {
        sv.on = 0;
        return 0;
    }
    if (!sv.on) {                                /* (it begins: from the film's start) */
        sv.on = 1, sv.at = 0, sv_shown = 0;
    }
    if (!sv_shown || now - sv_shown >= (uint64_t)(sr_host * (float)sv.ms / 1000.0f)) {
        if (sv_decode(sv.at))
            sv.at = 0;
        else
            sv.at = (sv.at + 1u) % sv.nf;
        sv_shown = now;
    }
    for (y = 0; y < 320u; y++) {                 /* rows 40 .. 199 of the 240, each twice, each pixel twice */
        const uint8_t *r = sv_fb + (40u + y / 2u) * 30u;
        uint16_t *px = fb + y * 480u;
        for (x = 0; x < 480u; x++)
            px[x] = r[x >> 4] & 0x80u >> ((x >> 1) & 7u) ? 0xFFFFu : 0x0000u;
    }
    return 1;
}
