/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 PolarPatch */
/* FM-1 Again VIBE: FM-1 Again's sound (freestanding/core.c and what it includes, synced from FM-1 Again by
 * tools/sync_engine.sh, never edited here) as a native VIBE FM firmware. Built with the VIBE FM SDK (Apache-2.0).
 *
 * The engine runs as on the FM-1, at its own 44.1 kHz in blocks of up to 256 frames (core_render); vibe_on_audio fills
 * a ring from it and reads the ring at the host's rate (a cubic Hermite), as Arctic Scream VIBE does.
 *
 * Two flows (VIBE_PARALLEL_AUDIO): the engine is the sound flow's. The control flow (vibe_on_event, vibe_on_draw) is
 * the one writer of the settings (set.p, set.inst: bytes) and the parameters the host sees; what moves the engine
 * (notes, PLAY, BUILD, the tricks, "the parameters changed") it hands the sound flow through a queue (one writer, one
 * reader). The display reads what the sound flow writes (the clock, the groove, the waveform's columns). */
#include <stdint.h>
#include <string.h>

/* ---- what the engine expects of fm1-base (plat.h) */
#define APP_BLOCK 256u
#define PLAT_POOL
static inline int32_t clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
#include "fastmath.h"
#include "core.c"

#include "vibe.h"
#include "vibe_gfx.h"
#include "vibe_dsp.h"

VIBE_PARALLEL_AUDIO;

#define FS 44100.0f

/* ---- the queue: the control flow's commands to the sound flow */
enum { Q_NOTE, Q_APPLY, Q_PLAY, Q_STOP, Q_BUILD, Q_TRICK, Q_SLICE, Q_REC };
typedef struct { uint8_t type, a, b, on; } qcmd;
#define QN 256u
static qcmd qbuf[QN];
static volatile uint32_t q_w, q_r;
static void q_put(uint8_t type, uint8_t a, uint8_t b, uint8_t on)   /* (control flow) */
{
    qcmd c = {type, a, b, on};
    if (q_w - q_r >= QN)
        return;                                  /* (full: the sound flow has not run for a while) */
    qbuf[q_w % QN] = c;
    VIBE_RELEASE();
    q_w++;
}

/* ---- the ring: the engine's 44.1 kHz, read at the host's rate */
#define RING 1024u
static float ring[RING * 2u];
static uint32_t ring_w, ring_r;
static float r_pos, r_step = 1.0f;
static float out_gain = 0.8f;

/* ---- the waveform: the last 16 bars, a column each 1/232 (the FM-1's, wave.h), peaks and RMS, left and right */
#define WV_W 232
static uint8_t wv_pk[2][WV_W], wv_rms[2][WV_W];
static volatile uint32_t wv_head;
static float wv_peak[2], wv_sum[2];
static uint32_t wv_n;
static void wv_feed(const float *l, const float *r, uint32_t n, float pos, float dpos)   /* (sound flow) */
{
    uint32_t i, c, k;
    for (i = 0; i < n; i++) {
        float a = fm_fabsf(l[i]), b = fm_fabsf(r[i]);
        wv_peak[0] = a > wv_peak[0] ? a : wv_peak[0];
        wv_peak[1] = b > wv_peak[1] ? b : wv_peak[1];
        wv_sum[0] += l[i] * l[i], wv_sum[1] += r[i] * r[i];
        wv_n++;
        c = (uint32_t)(pos * (float)WV_W / 256.0f) % WV_W;
        pos += dpos;
        if (c != wv_head) {
            for (k = 0; k < 2u; k++) {
                float p = fm_minf(wv_peak[k], 1.0f), q = fm_sqrtf(wv_sum[k] / (float)(wv_n ? wv_n : 1u));
                wv_pk[k][wv_head] = (uint8_t)(p * 50.0f + 0.5f);
                wv_rms[k][wv_head] = (uint8_t)(fm_minf(q, 1.0f) * 50.0f + 0.5f);
                wv_peak[k] = wv_sum[k] = 0.0f;
            }
            wv_n = 0;
            wv_head = c;
        }
    }
}

/* ---- the parameters, for the DAW and the plugin's Editor: every PA_*, its id for ever, in the values the screen
 * shows (TEMPO in BPM, PITCH in semitones, SLICE from 1), grouped as the pages */
static const char *const P_ID[NPA] = {
    "tempo", "swing", "kick", "hats", "clap", "keys_tone", "keys_decay", "chords", "keys_plays", "one_key",
    "vox_vowels", "vox_gate", "vox_shift", "vox_breath", "pump_depth", "pump_time", "pump_shape", "pump_source",
    "space_size", "space_damp", "space_level", "space_shine", "filter", "key", "build_bars", "keys_level",
    "voice_level", "groove", "chop_slice", "chop_length", "chop_pitch", "chop_mode", "echo_time", "echo_again",
    "echo_wear", "echo_level", "echo_mode", "tape_wow", "tape_warmth", "tape_dull", "tape_hiss", "tape",
    "rise_length", "rise_bright", "rise_suck", "rise_auto", "vary", "build_style", "rush", "play_mode", "black_keys",
    "rise_level", "saver", "chop_reverse"};
_Static_assert(sizeof P_ID / sizeof P_ID[0] == NPA, "an id for every parameter (append only: automation hangs on them)");
static const char *const KEY_NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static vibe_param params[NPA];
static const char *p_group[NPA];
static int32_t pa_off(uint32_t i) { return PA[i].fmt == FMT_BPM ? 80 : PA[i].fmt == FMT_SEMI ? -12 : PA[i].fmt == FMT_ONE ? 1 : 0; }

static void settings_write(void);
static void say(const char *a, const char *b);
static void num(char *d, int32_t v);
static void cat(char *d, const char *s);
static void rec_to_chop(void);
#include "port/rec.c"                            /* sound in, straight into CHOP */
#include "port/load.c"                           /* a WAV dropped on the window, into CHOP */
#include "port/pages.c"                          /* the pages, the controls, the display */

static void params_init(void)
{
    uint32_t i;
    for (i = 0; i < NPA; i++) {
        uint32_t f = PA[i].fmt;
        vibe_param *p = &params[i];
        p->id = P_ID[i];
        p->name = PA[i].name;
        p->min = (float)pa_off(i);
        p->max = (float)(PA[i].top + pa_off(i));
        p->def = (float)(PA[i].def + pa_off(i));
        p->unit = f == FMT_PCT && PA[i].top == 100 ? "%" : f == FMT_BPM ? "BPM" : f == FMT_SEMI ? "st" : "";
        p->steps = f == FMT_LIST || f == FMT_KEY || f == FMT_BARS || PA[i].top < 40 ? PA[i].top + 1 : 0;
        p->choices = f == FMT_LIST ? PA[i].list : f == FMT_KEY ? KEY_NAMES : f == FMT_BARS ? BARS_NAME : 0;
        p_group[i] = pg_group(i);
    }
    vibe_params(params, NPA);
    vibe_param_groups(p_group);
}

/* the settings, as the host keeps them (in the plugin: in the DAW's project) */
static void settings_read(void)
{
    if (vibe_store_read("settings", &set, (int)sizeof set) != (int)sizeof set || set.magic != SET_MAGIC)
        settings_defaults();
    else
        settings_fix();
    if (set.page >= NPG)
        set.page = PG_HOME;
}
static void settings_write(void) { vibe_store_write("settings", &set, (int)sizeof set); }

void vibe_on_init(float sample_rate)
{
    uint32_t i;
    r_step = FS / (sample_rate > 1000.0f ? sample_rate : FS);
    rec_step = (float)REC_RATE / (sample_rate > 1000.0f ? sample_rate : FS);
    sr_host = sample_rate > 1000.0f ? sample_rate : FS;
    vibe_notes(VIBE_NOTES);
    dr_init();
    settings_read();
    chop_factory();
    ch.s = ch_next;
    ch_swap = 0;
    rec_load();                                  /* (a recording the host kept takes the factory phrase's place) */
    {
        char p[8];
        int a, b, c;
        (void)vibe_media_dropped(p, 0, &a, &b, &c);   /* (SDK 14: files may be dropped on the window) */
    }
    apply();
    params_init();
    for (i = 0; i < NPA; i++)
        vibe_param_set((int)i, (float)(set.p[i] + pa_off(i)));
    ui_init();
}

void vibe_on_event(const vibe_event *e)          /* (the control flow) */
{
    if (e->type == VIBE_EV_PARAM && e->id < NPA) {   /* the DAW's automation, a mapped controller */
        int32_t v = (int32_t)(e->value + (e->value < 0.0f ? -0.5f : 0.5f)) - pa_off(e->id);
        set.p[e->id] = (uint8_t)clamp(v, 0, PA[e->id].top);
        q_put(Q_APPLY, 0, 0, 0);
        return;
    }
    if (e->type == VIBE_EV_MEDIA) {             /* a file dropped, or what was not there yet has come */
        ld_poll();
        return;
    }
    if (e->type == VIBE_EV_SAVE) {
        settings_write();
        return;
    }
    ui_event(e);
}

static void q_run(const qcmd *c)                 /* (the sound flow) */
{
    switch (c->type) {
    case Q_NOTE:
        play_note(c->a, c->b, c->on, 1);
        break;
    case Q_APPLY:
        apply();
        break;
    case Q_PLAY:
        if (ck.run) {
            ck.run = 0;
            gr_stop();
        } else
            ck_start();
        break;
    case Q_STOP:
        if (ck.run) {
            ck.run = 0;
            gr_stop();
        }
        break;
    case Q_BUILD:
        if (!ck.run)
            ck_start();
        gr_build_button();
        break;
    case Q_TRICK:
        if (c->on)
            pfx.held |= 1u << c->a;
        else
            pfx.held &= ~(1u << c->a);
        break;
    case Q_REC:
        if (c->a)
            rec_start();
        else
            rec_stop();
        break;
    case Q_SLICE:                                /* a pad as CHOP's slice: its note, whatever the keys play */
        if (c->on)
            ch_start(53 + (int32_t)c->a, (float)c->b / 127.0f);
        else {
            uint32_t i;
            for (i = 0; i < CH_NV; i++)
                if (ch.v[i].note == 53 + (int32_t)c->a)
                    ch.v[i].held = 0;
        }
        break;
    }
}

static int host_playing;                         /* the DAW's (or a MIDI clock's) transport, last block */
static void follow_host(void)                    /* (the sound flow) the host's tempo and its PLAY, when it gives them */
{
    const vibe_transport_info *t = vibe_transport();
    if (!t || !t->has)
        return;
    if (t->bpm > 20.0f)
        bpm_base = t->bpm;
    if (t->playing && !host_playing && !ck.run)
        ck_start();
    else if (!t->playing && host_playing && ck.run) {
        ck.run = 0;
        gr_stop();
    }
    host_playing = t->playing;
}

static float r_at(uint32_t i, uint32_t c) { return ring[(i % RING) * 2u + c]; }
static void engine_block(void)                   /* (the sound flow) 256 frames more into the ring */
{
    float p0, p1;
    uint32_t i;
    core_render(APP_BLOCK, &p0, &p1);
    wv_feed(b_kl, b_kr, APP_BLOCK, p0, (p1 - p0) / (float)APP_BLOCK);
    for (i = 0; i < APP_BLOCK; i++, ring_w++) {
        ring[(ring_w % RING) * 2u] = b_kl[i];
        ring[(ring_w % RING) * 2u + 1u] = b_kr[i];
    }
}

void vibe_on_audio(const float *in, float *out_l, float *out_r, int frames)
{
    int k;
    while (q_r != q_w) {
        VIBE_ACQUIRE();
        q_run(&qbuf[q_r % QN]);
        VIBE_RELEASE();
        q_r++;
    }
    follow_host();
    for (k = 0; k < frames; k++) {
        uint32_t i = ring_r + (uint32_t)r_pos, c;
        float t = r_pos - (float)(uint32_t)r_pos, o[2];
        while (ring_w - i < 3u)
            engine_block();
        for (c = 0; c < 2u; c++) {               /* cubic Hermite through i-1 .. i+2 */
            float y0 = r_at(i - 1u, c), y1 = r_at(i, c), y2 = r_at(i + 1u, c), y3 = r_at(i + 2u, c);
            float a = (y3 - y0) * 0.5f + (y1 - y2) * 1.5f, b = y0 - y1 * 2.5f + y2 * 2.0f - y3 * 0.5f, d = (y2 - y0) * 0.5f;
            o[c] = ((a * t + b) * t + d) * t + y1;
        }
        out_l[k] = o[0] * out_gain;
        out_r[k] = o[1] * out_gain;
        r_pos += r_step;
        if (r_pos >= 1.0f) {
            uint32_t w = (uint32_t)r_pos;
            ring_r += w;
            r_pos -= (float)w;
        }
    }
    rec_audio(in, out_l, out_r, frames);
}

void vibe_on_draw(uint16_t *fb)
{
    ld_poll();
    rec_poll();
    ui_draw(fb);
}
