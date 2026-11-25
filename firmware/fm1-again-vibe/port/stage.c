/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 PolarPatch */
/* FM-1 Again VIBE: the stage (SDK 23), 960 x 540, for a room, a screen behind the player or a stream: the crowd's film
 * dimmed over all of it (four pixels a dot), and over it in big type what the display's header says (the bar, "DROP IN
 * 3", a trick held), the tempo and the groove, the last 16 bars' waveform, and the pads, lit as they play. A picture
 * only: the display and the controls do everything without it. Part of main.c (the control flow). */

#define ST_W 960
#define ST_H 540
static uint16_t st_fb[ST_W * ST_H];
static uint64_t st_shown;

static void stage_init(void) { vibe_stage(st_fb, ST_W, ST_H, 0x000000, 0); }

/* (vibe_on_event) a click on the stage: a pad there is the pad (held while the pointer is down) */
static int st_pad = -1;
static void stage_event(const vibe_event *e)
{
    int x = VIBE_STAGE_X(e), y = VIBE_STAGE_Y(e), k = (x - 40) / 55;
    vibe_event p;
    memset(&p, 0, sizeof p);
    p.type = VIBE_EV_PAD;
    if (e->id == VIBE_STAGE_DOWN && y >= 470 && y < 510 && x >= 40 && k < 16 && (x - 40) % 55 < 50) {
        st_pad = k;
        p.id = (uint8_t)k, p.down = 1, p.value = 0.8f;
        ui_event(&p);
    } else if (e->id == VIBE_STAGE_UP && st_pad >= 0) {
        p.id = (uint8_t)st_pad, p.down = 0;
        st_pad = -1;
        ui_event(&p);
    }
}

void vibe_on_stage(uint16_t *fb, int w, int h)
{
    uint64_t now = vibe_frames();
    uint32_t y, x, k;
    char t[48];
    (void)fb;
    if (w != ST_W || h != ST_H)
        return;
    vg_clear(0x000000);
    if (sv.p) {                                  /* the film, its frame on its own clock (the screensaver's buffer) */
        if (!sv.on && (!st_shown || now - st_shown >= (uint64_t)(sr_host * (float)sv.ms / 1000.0f))) {
            if (sv_decode(sv.at))
                sv.at = 0;
            else
                sv.at = (sv.at + 1u) % sv.nf;
            st_shown = now;
        }
        for (y = 0; y < 135u; y++) {             /* rows 52 .. 186 of the 240, four times: 540 */
            const uint8_t *r = sv_fb + (52u + y) * 30u;
            for (x = 0; x < 240u;) {
                uint32_t x0;
                if (!(r[x >> 3] & 0x80u >> (x & 7u))) {
                    x++;
                    continue;
                }
                for (x0 = x; x < 240u && r[x >> 3] & 0x80u >> (x & 7u); x++)
                    ;
                vg_rect((int)x0 * 4, (int)y * 4, (int)(x - x0) * 4, 4, 0x2A2A2A);
            }
        }
    }
    header_text(t);                              /* what the display's header says, twice its size */
    vg_text(t, 40, 28, VG_HUGE | VG_X2, 0xFFFFFF, VG_LEFT);
    t[0] = 0;
    num(t, (int32_t)(ck.bpm + 0.5f));
    cat(t, " BPM   ");
    cat(t, PAT_NAME[set.p[PA_PAT]]);
    cat(t, "   ");
    cat(t, IN_NAMES[set.inst]);
    vg_text(t, 40, 168, VG_BIG | VG_X2, 0xB4B4B4, VG_LEFT);
    for (k = 0; k < 2u; k++) {                   /* the waveform: the last 16 bars, left over right */
        int mid = 300 + (int)k * 96;
        for (x = 0; x < WV_W; x++) {
            int p = wv_pk[k][x] * 44 / 50, q = wv_rms[k][x] * 44 / 50, xx = 40 + (int)x * 380 / 100;
            vg_rect(xx, mid - p, 3, 2 * p + 1, 0x3C3CD2);
            if (q)
                vg_rect(xx, mid - q, 3, 2 * q + 1, 0x7A7AE6);
        }
    }
    if (ck.run)
        vg_rect(40 + (int)wv_head * 380 / 100, 252, 3, 192, 0xFFFFFF);
    for (k = 0; k < 16u; k++) {                  /* the pads, lit as they play */
        uint32_t on = pad_held[k] || (k < NPF && (pfx.held >> k & 1u));
        int x0 = 40 + (int)k * 55;
        vg_rect(x0, 470, 50, 40, on ? 0xFFFFFF : 0x1A1A1A);
        vg_text(set.inst == IN_CHOP ? vg_itoa((int)k + 1, t) : PAD_TAG[k], x0 + 25, 480, VG_BOLD, on ? 0x000000 : 0x8A8A8A,
                VG_CENTER);
    }
}
