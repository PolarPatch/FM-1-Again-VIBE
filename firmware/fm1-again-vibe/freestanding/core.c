/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 PolarPatch */
/* FM-1 Again's core: the sound, apart from any device. The parameters, the settings record, what the keys play, and
 * the mix: the groove's drums, KEYS, VOX, CHOP, RISE, the PUMP, ECHO, SPACE, the filters, PERFORM, TAPE, at 44.1 kHz
 * in blocks of up to APP_BLOCK frames. The FM-1 (app.c) and FM-1 Again VIBE (its port, at the host's rate through a
 * resampler) include it; each brings its own controls, screen, storage and MIDI, and calls:
 *   apply()                    the parameters (set.p) into the engine
 *   play_note(note, vel, on, from_audio)   a key or a MIDI note to what the keys play
 *   core_render(n, &p0, &p1)   a block into b_kl / b_kr
 *   core_panic()               silence
 *   settings_defaults(), settings_fix()    the settings record
 *   chop_factory()             CHOP's factory phrase
 * It needs, before it: stdint, string (memset, memcpy), clamp() and PLAT_POOL (plat.h), fastmath.h, APP_BLOCK. */
#include "dsp/qdsp.h"
#include "dsp/fdsp.h"
#include "dsp/space.h"
#include "dsp/echo.h"
#include "dsp/tape.h"
#include "engine/clock.h"
#include "engine/drums.h"
#include "engine/keys.h"
#include "engine/vox.h"
#include "engine/chop.h"
#include "gen/again_chop.h"   /* CHOP's factory sample (tools/again_chop.py, tests/factory_chop.txt) */
#include "engine/pump.h"
#include "engine/rise.h"
#include "engine/perform.h"
#include "engine/groove.h"
#include "engine/revecho.h"

/* ------------------------------------------------------------- parameters --- */
/* every value a knob turns, 0 .. its top; the settings keep them */
enum {
    PA_TEMPO, PA_SWING, PA_KICK, PA_HATS, PA_CLAP,          /* the groove */
    PA_TONE, PA_DECAY, PA_PROG, PA_RHYTHM, PA_CHORD,         /* KEYS */
    PA_VOWELS, PA_GATE, PA_FORMANT, PA_BREATH,               /* VOX */
    PA_DEPTH, PA_RELEASE, PA_SHAPE, PA_SOURCE,               /* PUMP */
    PA_SIZE, PA_DAMP, PA_MIX, PA_SHIMMER,                    /* SPACE */
    PA_FILTER, PA_KEY, PA_BUILD, PA_KEYS_LV, PA_VOX_LV, PA_PAT,
    PA_SLICE, PA_LENGTH, PA_PITCH, PA_CMODE,                 /* CHOP */
    PA_ETIME, PA_EFB, PA_EWEAR, PA_EMIX, PA_EPING,           /* ECHO */
    PA_TWOW, PA_TSAT, PA_TTONE, PA_THISS, PA_TAPE,           /* TAPE */
    PA_RLEN, PA_RBRIGHT, PA_RSUCK, PA_RAUTO,                 /* RISE */
    PA_VARY, PA_BSTYLE, PA_TEMPOUP, PA_PLAYMODE, PA_PADS, PA_RISE_LV, PA_SAVER, PA_CREV,
    PA_SYNC, PA_CLKOUT,                                      /* MIDI clock (1.0.1) */
    PA_NOTESOUT,                                             /* the groove's notes out (1.0.2) */
    NPA
};
enum { FMT_PCT, FMT_BPM, FMT_LIST, FMT_MS, FMT_KEY, FMT_BARS, FMT_FILTER, FMT_SIZE, FMT_RAW, FMT_SEMI, FMT_ONE, FMT_UP };
typedef struct {
    const char *name;                            /* on the screen (and to a screen reader) */
    uint8_t top, def, fmt;
    const char *const *list;                     /* FMT_LIST's names */
} param_t;
static const char *const BARS_NAME[] = {"2", "4", "8"};
static const char *const ETIME_NAME[6] = {"1/16", "1/8", "3/16", "1/4", "3/8", "1/2"};
static const uint8_t ETIME_16TH[6] = {1, 2, 3, 4, 6, 8};
static const char *const EMODE_NAME[3] = {"TAPE", "PING-PONG", "REVERSE"};
static const char *const PLAYMODE_NAME[2] = {"LOOP", "SONG"};
static const char *const PADS_NAME[2] = {"NOTES", "PADS"};
static const char *const SAVER_NAME[2] = {"OFF", "ON"};
static const char *const CREV_NAME[2] = {"FORWARD", "REVERSE"};
static const char *const SYNC_NAME[2] = {"AUTO", "INTERNAL"};
static const char *const ONOFF_NAME[2] = {"OFF", "ON"};
/* the black keys as pads (loaf-breaker's way): PERFORM's tricks while held, F#3 .. F#5, and their names on the strip */
#define NPAD 11
static const uint8_t PAD_TRICK[NPAD] = {PF_STUT8, PF_STUT16, PF_STUT32, PF_REVERSE, PF_TAPESTOP, PF_GATE, PF_LOWSWEEP,
                                        PF_ECHOTHROW, PF_SPACETHROW, PF_KICKOUT, PF_DRUMSOUT};
static const char *const PAD_SHORT[NPAD] = {"R8", "R16", "R32", "REV", "STP", "GAT", "FLT", "ECH", "SPC", "KIL", "DRM"};
/* TAPE's presets: WOW, WARMTH, DULL, HISS */
enum { NTP = 5 };
static const char *const TP_NAME[NTP] = {"CLEAN", "WARM", "CASSETTE", "WORN", "MEMORY"};
static const uint8_t TP_SET[NTP][4] = {{0, 0, 0, 0}, {12, 30, 20, 8}, {30, 45, 45, 22}, {60, 70, 60, 35},
                                       {100, 55, 85, 30}};
static const char *const KEY_NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const char *const PAT_NAME[GR_NPAT] = {"HOUSE", "2-STEP", "GARAGE 4X4", "SHUFFLE", "HALFTIME", "BROKEN",
                                              "MINIMAL", "KICK ONLY"};
static const param_t PA[NPA] = {
    [PA_TEMPO] = {"TEMPO", 100, 44, FMT_BPM, 0},            /* 80 .. 180 BPM */
    [PA_SWING] = {"SWING", 100, 67, FMT_PCT, 0},
    [PA_KICK] = {"KICK", 100, 85, FMT_PCT, 0},
    [PA_HATS] = {"HATS", 100, 65, FMT_PCT, 0},
    [PA_CLAP] = {"CLAP", 100, 75, FMT_PCT, 0},
    [PA_TONE] = {"TONE", 100, 45, FMT_PCT, 0},
    [PA_DECAY] = {"DECAY", 100, 55, FMT_PCT, 0},
    [PA_PROG] = {"CHORDS", GR_NPROG - 1, 1, FMT_LIST, PROG_NAME},
    [PA_RHYTHM] = {"PLAYS", NCR - 1, CR_HOLD, FMT_LIST, CR_NAME},
    [PA_CHORD] = {"ONE KEY", NCH - 1, CH_MIN9, FMT_LIST, CH_NAME},
    [PA_VOWELS] = {"VOWELS", VX_NSET - 1, 0, FMT_LIST, VX_SET_NAME},
    [PA_GATE] = {"GATE", 100, 25, FMT_MS, 0},               /* 40 .. 640 ms */
    [PA_FORMANT] = {"SHIFT", 100, 50, FMT_PCT, 0},
    [PA_BREATH] = {"BREATH", 100, 20, FMT_PCT, 0},
    [PA_DEPTH] = {"PUMP", 100, 70, FMT_PCT, 0},
    [PA_RELEASE] = {"TIME", 100, 30, FMT_MS, 0},          /* 60 .. 660 ms */
    [PA_SHAPE] = {"SHAPE", 100, 50, FMT_PCT, 0},
    [PA_SOURCE] = {"SOURCE", NPS - 1, PS_KICK, FMT_LIST, PS_NAME},
    [PA_SIZE] = {"SIZE", 127, 52, FMT_SIZE, 0},
    [PA_DAMP] = {"DAMP", 127, 60, FMT_PCT, 0},
    [PA_MIX] = {"SPACE", 127, 70, FMT_PCT, 0},
    [PA_SHIMMER] = {"SHINE", 127, 0, FMT_PCT, 0},
    [PA_FILTER] = {"FILTER", 100, 50, FMT_FILTER, 0},
    [PA_KEY] = {"KEY", 11, 9, FMT_KEY, 0},
    [PA_BUILD] = {"BUILD", 2, 1, FMT_BARS, 0},
    [PA_KEYS_LV] = {"KEYS", 100, 70, FMT_PCT, 0},
    [PA_VOX_LV] = {"VOICE", 100, 70, FMT_PCT, 0},            /* VOX and CHOP */
    [PA_PAT] = {"GROOVE", GR_NPAT - 1, 1, FMT_LIST, PAT_NAME},
    [PA_SLICE] = {"SLICE", CH_MAX_SLICES - 1, 0, FMT_ONE, 0},
    [PA_LENGTH] = {"LENGTH", 100, 100, FMT_PCT, 0},
    [PA_PITCH] = {"PITCH", 24, 12, FMT_SEMI, 0},             /* -12 .. +12 */
    [PA_CMODE] = {"MODE", NCM - 1, CM_SLICES, FMT_LIST, CM_NAME},
    [PA_ETIME] = {"TIME", 5, 2, FMT_LIST, ETIME_NAME},
    [PA_EFB] = {"AGAIN", 127, 60, FMT_PCT, 0},
    [PA_EWEAR] = {"WEAR", 127, 45, FMT_PCT, 0},
    [PA_EMIX] = {"ECHO", 127, 40, FMT_PCT, 0},
    [PA_EPING] = {"MODE", 2, 1, FMT_LIST, EMODE_NAME},       /* TAPE, PING-PONG, REVERSE */
    [PA_TWOW] = {"WOW", 127, 12, FMT_PCT, 0},
    [PA_TSAT] = {"WARMTH", 127, 30, FMT_PCT, 0},
    [PA_TTONE] = {"DULL", 127, 20, FMT_PCT, 0},
    [PA_THISS] = {"HISS", 127, 8, FMT_PCT, 0},
    [PA_TAPE] = {"TAPE", NTP - 1, 1, FMT_LIST, TP_NAME},
    [PA_RLEN] = {"LENGTH", 4, 2, FMT_LIST, RS_LEN_NAME},
    [PA_RBRIGHT] = {"BRIGHT", 100, 60, FMT_PCT, 0},
    [PA_RSUCK] = {"SUCK", 100, 50, FMT_PCT, 0},
    [PA_RAUTO] = {"AUTO", NRA - 1, RA_DROP, FMT_LIST, RA_NAME},
    [PA_VARY] = {"VARY", 100, 25, FMT_PCT, 0},
    [PA_BSTYLE] = {"STYLE", NBS - 1, BS_SNARE, FMT_LIST, BS_NAME},
    [PA_TEMPOUP] = {"RUSH", 20, 0, FMT_UP, 0},          /* + 0 .. 20 BPM at the drop */
    [PA_PLAYMODE] = {"PLAY", 1, 0, FMT_LIST, PLAYMODE_NAME},
    [PA_PADS] = {"BLACK", 1, 1, FMT_LIST, PADS_NAME},
    [PA_RISE_LV] = {"RISE", 100, 80, FMT_PCT, 0},
    [PA_SAVER] = {"SAVER", 1, 1, FMT_LIST, SAVER_NAME},   /* the screensaver after a minute untouched */
    [PA_CREV] = {"REVERSE", 1, 0, FMT_LIST, CREV_NAME},   /* the slice in hand (chop_cur): its bit in set.crev */
    [PA_SYNC] = {"SYNC", 1, 0, FMT_LIST, SYNC_NAME},      /* AUTO: a MIDI clock that comes in leads */
    [PA_CLKOUT] = {"CLOCK OUT", 1, 0, FMT_LIST, ONOFF_NAME},   /* MIDI clock, START and STOP out over USB */
    [PA_NOTESOUT] = {"NOTES OUT", 1, 1, FMT_LIST, ONOFF_NAME},   /* the groove's notes out: drums 2, chords 4, RISE 5 */
};

/* what the keys play (ALGORITHM) */
enum { IN_KEYS, IN_VOX, IN_CHOP, IN_RISE, IN_DRUMS, NIN };
static const char *const IN_NAME[NIN] = {"KEYS", "VOX", "CHOP", "RISE", "DRUMS"};

/* The settings record, kept from one release to the next: its parameters in PA_* order with their count (np), so a
 * release that adds parameters (at the end of PA_*, never between) loads the old record and gives the new ones their
 * defaults; a value out of its range takes its default alone. Another magic or size: all defaults. */
#define SET_MAGIC 0x41474E32u            /* "AGN2" */
#define SET_NP 96u                       /* the parameters' room */
typedef struct {
    uint32_t magic;
    uint8_t np;                          /* the parameters this record holds */
    int8_t octave;                       /* -2 .. +2 */
    uint8_t inst;                        /* IN_* */
    uint8_t ble_off;                     /* Bluetooth switched off (REC held); 0, the default: on */
    uint8_t page;
    uint8_t tips;                        /* the tips done (bit each: the tutor) */
    uint8_t rsv0[2];
    uint32_t crev;                       /* CHOP: the slices played backwards (REVERSE) */
    uint8_t p[SET_NP];                   /* the parameters */
    uint8_t rsv[16];
} settings_t;
_Static_assert(NPA <= SET_NP, "the settings record's room for parameters");
static settings_t set;
static uint8_t set_dirty;                /* changed since the last save: SAVE is lit */

/* the parameters into the engine (the main loop; the engine reads them a block at a time) */
static float pa01(uint32_t i) { return (float)set.p[i] / (float)PA[i].top; }
static volatile float bpm_base = 124.0f;     /* TEMPO (the clock adds the build's TEMPO UP to it) */
static void apply(void)
{
    bpm_base = 80.0f + (float)set.p[PA_TEMPO];
    ck.bpm = bpm_base + gr.bpm_add;
    ck.swing = 0.33f * pa01(PA_SWING);
    gr.pat = set.p[PA_PAT];
    gr.prog = set.p[PA_PROG];
    gr.rhythm = set.p[PA_RHYTHM];
    gr.key = set.p[PA_KEY];
    gr.build_bars = 2u << set.p[PA_BUILD];
    gr.lv[DR_KICK] = pa01(PA_KICK);
    gr.lv[DR_HAT] = gr.lv[DR_OPEN] = gr.lv[DR_SHAKER] = pa01(PA_HATS);
    gr.lv[DR_CLAP] = gr.lv[DR_SNARE] = gr.lv[DR_RIM] = pa01(PA_CLAP);
    kp.tone = pa01(PA_TONE);
    kp.decay = pa01(PA_DECAY);
    kp.level = pa01(PA_KEYS_LV);
    vx.set = set.p[PA_VOWELS];
    vx.gate_s = 0.04f + 0.6f * pa01(PA_GATE);
    vx.shift = 0.7f * fm_exp2f(pa01(PA_FORMANT));       /* 0.7 .. 1.4 */
    vx.breath = 0.6f * pa01(PA_BREATH);
    vx.level = pa01(PA_VOX_LV);
    ch.level = pa01(PA_VOX_LV);
    ch.sel = set.p[PA_SLICE];
    ch.length = 0.05f + 0.95f * pa01(PA_LENGTH);
    ch.pitch = (float)set.p[PA_PITCH] - 12.0f;
    ch.mode = set.p[PA_CMODE];
    ch.rev = set.crev;
    pm.depth = pa01(PA_DEPTH);
    pm.rel_s = 0.06f + 0.6f * pa01(PA_RELEASE);
    pm.shape = pa01(PA_SHAPE);
    pm.src = set.p[PA_SOURCE];
    rv_size = set.p[PA_SIZE];
    rv_damp = set.p[PA_DAMP];
    rv_mix = set.p[PA_MIX];
    rv_shim = set.p[PA_SHIMMER];
    rv_pre = 20;
    dl_ms = (int32_t)(15000.0f * (float)ETIME_16TH[set.p[PA_ETIME]] / bpm_base);
    dl_fb = set.p[PA_EFB];
    dl_wear = set.p[PA_EWEAR];
    dl_mix = set.p[PA_EPING] == 2u ? 0 : set.p[PA_EMIX];   /* (REVERSE: revecho.h instead) */
    dl_pp = set.p[PA_EPING] == 1u ? 110 : 0;
    rs.lenix = set.p[PA_RLEN];
    rs.bright = pa01(PA_RBRIGHT);
    rs.suck = pa01(PA_RSUCK);
    rs.autom = set.p[PA_RAUTO];
    rs.level = pa01(PA_RISE_LV);
    gr.vary = pa01(PA_VARY);
    gr.style = set.p[PA_BSTYLE];
    gr.tempo_up = (float)set.p[PA_TEMPOUP];
    gr.song = set.p[PA_PLAYMODE];
    tf_wow = set.p[PA_TWOW];
    tf_sat = set.p[PA_TSAT];
    tf_tone = set.p[PA_TTONE];
    tf_hiss = set.p[PA_THISS];
}

/* ------------------------------------------------------------------ sound --- */
static volatile uint32_t midi_last, midi_last_port, midi_count;   /* the last message in (shown), and how many */
static volatile int32_t last_note = -1;      /* the last note on (keys or MIDI), -1 after its off */
static volatile uint8_t want_stop;           /* PLAY stopped it: the interrupt lets the chords go */
static ch_sample_t ch_next;                  /* CHOP's sample from the main loop, taken at the next block */
static volatile uint8_t ch_swap;

/* a note from the keys or MIDI, to whatever the keys play (the main loop and MIDI in, both: the queues are one
 * writer each, so MIDI in posts from the audio interrupt only through kp_start directly) */
static void play_note(int32_t note, uint32_t vel, int on, int from_isr)
{
    uint32_t i;
    if (on)
        last_note = note;
    else if (last_note == note)
        last_note = -1;
    if (set.inst == IN_DRUMS) {
        uint32_t d = (uint32_t)(note % 12) * NDR / 12u;
        if (on) {
            if (from_isr)
                dr_hit(d, (float)vel / 127.0f);
            else
                dr.pend[d] = (uint8_t)(vel ? vel : 1u);
        }
        return;
    }
    if (set.inst == IN_CHOP) {
        if (from_isr) {
            if (on)
                ch_start(note, (float)vel / 127.0f);
            else
                for (i = 0; i < CH_NV; i++)
                    if (ch.v[i].note == note)
                        ch.v[i].held = 0;
        } else
            ch_post(note, vel, on);
        return;
    }
    if (set.inst == IN_RISE) {
        if (on) {
            if (from_isr)
                rs_start(note, rs_len_now(), (float)vel / 127.0f);
            else
                rs_post(note, vel);
        }
        return;
    }
    if (set.inst == IN_VOX) {
        if (from_isr) {
            if (on)
                vx_start(note, (float)vel / 127.0f);
            else
                for (i = 0; i < VX_NV; i++)
                    if (vx.v[i].note == note)
                        vx.v[i].held = 0;
        } else
            vx_post(note, vel, on);
        return;
    }
    for (i = 0; i < 5u; i++) {                   /* KEYS: the one-finger chord */
        int32_t iv = CH_IV[set.p[PA_CHORD]][i];
        if (iv < 0)
            break;
        if (from_isr) {
            if (on)
                kp_start(note + iv, (float)vel / 127.0f, 0);
            else
                kp_stop(note + iv, 0);
        } else
            kp_post(note + iv, vel, 0, on);
    }
}

/* the mix's buses, -1 .. 1: the kick (never pumped), the drums, the pumped (keys, voice), the reverb's send and
 * return */
static float b_kl[APP_BLOCK], b_kr[APP_BLOCK], b_dl[APP_BLOCK], b_dr[APP_BLOCK], b_pl[APP_BLOCK], b_pr[APP_BLOCK];
static float b_send[APP_BLOCK], b_rl[APP_BLOCK], b_rr[APP_BLOCK];
static svf_t f_hp[2], f_lp[2], f_riser;
static float kick_g = 1.0f, drums_g = 1.0f;      /* PERFORM's KICK OUT and DRUMS OUT, gliding */
static uint32_t riser_rnd = 99u;

static void render_seg(uint32_t a, uint32_t b)
{
    if (b <= a)
        return;
    dr_render(b_kl + a, b_kr + a, b_dl + a, b_dr + a, b_send + a, gr.lv, b - a);
    kp_render(b_pl + a, b_pr + a, b_send + a, b - a);
    vx_render(b_pl + a, b_pr + a, b_send + a, b - a);
    ch_render(b_pl + a, b_pr + a, b_send + a, b - a);
    rs_render(b_dl + a, b_dr + a, b_send + a, b - a);
}

/* a block of the mix (n <= APP_BLOCK frames) into b_kl / b_kr, -1 .. 1; *p0, *p1: the clock's position (sixteenths
 * of the 16 bars) at its start and its end (the FM-1's waveform) */
static void core_render(uint32_t n, float *p0, float *p1)
{
    uint32_t i, a = 0;
    float pos0, pos1, hp_fc, lp_fc, flt = (float)set.p[PA_FILTER], kick_to, drums_to;
    if (n > APP_BLOCK)
        n = APP_BLOCK;
    ck.bpm = bpm_base + gr.bpm_add;              /* (BUILD's TEMPO UP) */
    if (ch_swap) {                               /* a new sample: every voice of the old one off first */
        for (i = 0; i < CH_NV; i++)
            ch.v[i].note = -1;
        ch.s = ch_next;
        ch_swap = 0;
    }
    if (want_stop) {
        want_stop = 0;
        gr_stop();
    }
    for (i = 0; i < n; i++)
        b_kl[i] = b_kr[i] = b_dl[i] = b_dr[i] = b_pl[i] = b_pr[i] = b_send[i] = b_rl[i] = b_rr[i] = 0.0f;
    /* the steps, each where it falls in the block: the engines rendered up to it, then the step played */
    pos0 = ck_pos256();
    for (i = 0; i < n; i++)
        if (ck_frame()) {
            render_seg(a, i);
            a = i;
            if (ck.run)
                gr_step(ck.step);
        }
    render_seg(a, n);
    pos1 = ck_pos256();
    if (pos1 < pos0)
        pos1 += pos1 < 1.0f && pos0 > 255.0f ? 256.0f : 0.0f;
    if (pos1 < pos0)                             /* PLAY: the page starts again */
        pos0 = 0.0f;
    if (pfx.held & 1u << PF_ECHOTHROW)            /* PERFORM's throws: the keys and voices full into ECHO, SPACE */
        for (i = 0; i < n; i++)
            b_send[i] += 0.9f * (b_pl[i] + b_pr[i]);
    if (set.p[PA_EPING] == 2u)                   /* ECHO's REVERSE: the send backwards, a TIME later */
        re_render(b_send, b_rl, b_rr, b_send, n, (float)ETIME_16TH[set.p[PA_ETIME]], 1.2f * (float)set.p[PA_EFB] / 127.0f,
                  (float)set.p[PA_EWEAR] / 127.0f, 1.6f * (float)set.p[PA_EMIX] / 127.0f);
    echo_render(b_send, b_rl, b_rr, b_send, n);     /* ECHO: the send in; the echoes out (pumped) and into SPACE */
    if (pfx.held & 1u << PF_SPACETHROW)
        for (i = 0; i < n; i++)
            b_send[i] += 0.9f * (b_pl[i] + b_pr[i]);
    space_render(b_send, b_rl, b_rr, n);
    /* the filters: the build's high-pass on all but the kick; FILTER (HOME K3) on everything */
    hp_fc = gr.hp_fc;
    lp_fc = 20000.0f;
    if (flt > 52.0f)
        hp_fc = fm_maxf(hp_fc, 20.0f * fm_exp2f((flt - 52.0f) / 48.0f * 6.6f));
    else if (flt < 48.0f)
        lp_fc = 250.0f * fm_exp2f(flt / 48.0f * 6.3f);
    lp_fc = fm_minf(lp_fc, gr.song_lp);          /* SONG: the intro opening, the outro closing */
    svf_set(&f_hp[0], hp_fc, 0.75f);
    svf_set(&f_hp[1], hp_fc, 0.75f);
    svf_set(&f_lp[0], lp_fc, 0.8f);
    svf_set(&f_lp[1], lp_fc, 0.8f);
    svf_set(&f_riser, 300.0f * fm_exp2f(4.5f * gr.bd_p), 3.0f);
    kick_to = pfx.held & (1u << PF_KICKOUT | 1u << PF_DRUMSOUT) ? 0.0f : 1.0f;
    drums_to = pfx.held & 1u << PF_DRUMSOUT ? 0.0f : 1.0f;
    for (i = 0; i < n; i++) {
        float g = pm_gain(), l, r, hp, bp;
        kick_g += (kick_to - kick_g) * 0.004f;   /* PERFORM's KICK OUT, DRUMS OUT: ~5 ms, no click */
        drums_g += (drums_to - drums_g) * 0.004f;
        b_kl[i] *= kick_g;
        b_kr[i] *= kick_g;
        l = b_dl[i] * drums_g + (b_pl[i] + b_rl[i]) * g;
        r = b_dr[i] * drums_g + (b_pr[i] + b_rr[i]) * g;
        if (gr.riser > 0.0f) {                   /* the build's riser: noise, its band rising */
            svf_run(&f_riser, noise_f(&riser_rnd), &bp, 0);
            l += bp * gr.riser * 0.5f;
            r += bp * gr.riser * 0.5f;
        }
        if (hp_fc > 21.0f) {
            svf_run(&f_hp[0], l, 0, &hp);
            l = hp;
            svf_run(&f_hp[1], r, 0, &hp);
            r = hp;
        }
        l += b_kl[i];
        r += b_kr[i];
        if (lp_fc < 19000.0f) {
            l = svf_run(&f_lp[0], l, 0, 0);
            r = svf_run(&f_lp[1], r, 0, 0);
        }
        b_kl[i] = l;
        b_kr[i] = r;
    }
    pf_render(b_kl, b_kr, n, gr.pf_auto);        /* PERFORM's tricks (the pads, SEL + a white key; the build's) */
    tape_render(b_kl, b_kr, n);                  /* TAPE over everything */
    for (i = 0; i < n; i++) {
        b_kl[i] = fm_tanhf(1.3f * b_kl[i]);      /* a soft glue, never past full scale */
        b_kr[i] = fm_tanhf(1.3f * b_kr[i]);
    }
    *p0 = pos0;
    *p1 = pos1;
}

static void core_panic(void)
{
    uint32_t i;
    ck.run = 0;
    for (i = 0; i < KP_NV; i++)
        kp.v[i].note = -1;
    for (i = 0; i < VX_NV; i++)
        vx.v[i].note = -1;
    for (i = 0; i < CH_NV; i++)
        ch.v[i].note = -1;
    gr.chord[0] = gr.chord[1] = gr.chord[2] = gr.chord[3] = gr.chord[4] = -1;
}

/* --------------------------------------------------------------- settings --- */
static void settings_defaults(void)
{
    uint32_t i;
    memset(&set, 0, sizeof set);
    set.magic = SET_MAGIC;
    set.np = NPA;
    set.inst = IN_KEYS;
    set.page = 0;
    for (i = 0; i < NPA; i++)
        set.p[i] = PA[i].def;
}

/* the record as loaded: what is wrong in it (or new since it was saved) back to its default; the page is the caller's */
static void settings_fix(void)
{
    uint32_t i, np = set.np;             /* (an older release's record: fewer; the rest take their defaults) */
    for (i = 0; i < NPA; i++)
        if (i >= np || set.p[i] > PA[i].top)
            set.p[i] = PA[i].def;
    set.np = NPA;
    if (set.octave < -2 || set.octave > 2)
        set.octave = 0;
    if (set.inst >= NIN)
        set.inst = IN_KEYS;
    if (set.ble_off > 1u)
        set.ble_off = 0;
}

static void chop_factory(void)                /* the factory phrase now (its flash about to be written) */
{
    ch_sample_t s;
    ch_parse(CHOP_FACTORY, sizeof CHOP_FACTORY, &s);
    ch_swap = 0;
    ch_next = s;
    ch_swap = 1;                             /* (the next block lets go of the old one before it reads a byte) */
}
