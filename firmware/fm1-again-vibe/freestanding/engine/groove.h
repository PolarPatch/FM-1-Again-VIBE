/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 PolarPatch */
/* GROOVE: the drum patterns (house, UK garage's two-step and its shuffle, halftime, broken), their swing, the
 * chords the groove plays on KEYS (PROG), and BUILD / DROP: a build over BARS bars (the snares rolling faster, a
 * high-pass rising over everything but the kick, a noise riser, the kick gone in the last bar, the last beat held
 * silent), then the drop at the bar: everything back at once with a crash into the reverb. gr_step runs in the audio
 * interrupt at each step of the clock. Part of app.c. */

/* a pattern: each drum's 16 steps ('.' none, 'g' a ghost, 'x' a hit, 'X' an accent; 'c' a hit half the time, 'a'
 * a hit in the first bar of two, 'b' in the second: loaf-breaker's step conditions, as letters) and its swing */
typedef struct {
    const char *name;
    float swing;
    const char *d[NDR];                          /* KICK CLAP SNARE HAT OPEN SHAKER RIM */
} gr_pat_t;
#define GR_NPAT 8
static const gr_pat_t GR_PAT[GR_NPAT] = {
    {"HOUSE", 0.06f,
     {"X...x...X...x...", "....x.......x...", "................", "g.g.g.g.g.g.g.g.", "..x...x...x...x.",
      "................", "................"}},
    {"2-STEP", 0.22f,
     {"X.........x..c..", "....X.......X...", ".......g...c..g.", "..x..gx...x..gx.", "..............a.",
      "gxg.gxg.gxg.gxg.", "...........g..b."}},
    {"GARAGE 4X4", 0.18f,
     {"X...x...X...x...", "....X.......X...", ".......g.g....g.", "..x...x...x.....", "..............x.",
      "xgxgxgxgxgxgxgxg", "................"}},
    {"SHUFFLE", 0.25f,
     {"X..x......x..xc.", "....X.......X...", "..g....g..g..c.g", "x.xgx.xgx.xgx.xg", "......b.........",
      "................", ".g.....g.....g.."}},
    {"HALFTIME", 0.1f,
     {"X.....x...x.....", "........X.......", "...........g....", "x.x.x.x.x.x.x.x.", "................",
      "..g...g...g...g.", "................"}},
    {"BROKEN", 0.12f,
     {"X.....a...x.x..b", "....X.......X...", ".g.....g.g..c.g.", "x.x.xgx.x.x.xgx.", "..............x.",
      "................", "........c......."}},
    {"MINIMAL", 0.15f,
     {"X...x...X...x...", "................", "................", "................", "..x...x...x...x.",
      "xgxgxgxgxgxgxgxg", "..g...g.x..g..g."}},
    {"KICK ONLY", 0.0f,
     {"X...x...X...x...", "................", "................", "................", "................",
      "................", "................"}}};

/* PROG: four chords, two bars each: (degree over KEY, chord) */
#define GR_NPROG 5
static const char *const PROG_NAME[GR_NPROG] = {"OFF", "LONGING", "NIGHT BUS", "HOPE", "FLOAT"};
static const uint8_t GR_PROG[GR_NPROG][4][2] = {
    {{0, 0}, {0, 0}, {0, 0}, {0, 0}},
    {{0, CH_MIN9}, {8, CH_MAJ9}, {3, CH_MAJ9}, {10, CH_ADD9}},
    {{0, CH_MIN9}, {5, CH_MIN7}, {3, CH_MAJ7}, {10, CH_SUS2}},
    {{0, CH_ADD9}, {7, CH_ADD9}, {9, CH_MIN7}, {5, CH_MAJ7}},
    {{0, CH_MIN9}, {0, CH_MIN9}, {5, CH_MIN9}, {8, CH_MAJ7}}};
enum { CR_HOLD, CR_STABS, CR_PULSE, NCR };
static const char *const CR_NAME[NCR] = {"HOLD", "STABS", "PULSE"};

enum { BD_NONE, BD_ARMED, BD_BUILD };
/* BUILD's STYLE: the snares rolling, the kicks rolling, the last bar stuttering (PERFORM's), or one of them in turn */
enum { BS_SNARE, BS_KICK, BS_STUTTER, BS_MIX, NBS };
static const char *const BS_NAME[NBS] = {"SNARE", "KICK", "STUTTER", "MIX"};

/* SONG: PLAY runs the sections in order by themselves, round and round (loaf-breaker's TRACKS, as one fixed song):
 * what plays in each, a build where one is, the filter opening in the intro and closing at the end */
enum { SF_KICK = 1, SF_HATS = 2, SF_CLAP = 4, SF_CHORDS = 8, SF_BUILD = 16, SF_OPEN = 32, SF_CLOSE = 64 };
typedef struct {
    const char *name;
    uint8_t bars, flags;
} gr_sec_t;
#define GR_NSEC 8
static const gr_sec_t GR_SONG[GR_NSEC] = {
    {"Intro", 8, SF_HATS | SF_CHORDS | SF_OPEN},
    {"Groove", 8, SF_KICK | SF_HATS | SF_CHORDS},
    {"Build", 4, SF_KICK | SF_HATS | SF_CHORDS | SF_BUILD},
    {"Drop", 16, SF_KICK | SF_HATS | SF_CLAP | SF_CHORDS},
    {"Break", 8, SF_CHORDS},
    {"Build", 4, SF_KICK | SF_HATS | SF_CHORDS | SF_BUILD},
    {"Drop", 16, SF_KICK | SF_HATS | SF_CLAP | SF_CHORDS},
    {"Outro", 8, SF_KICK | SF_HATS | SF_CHORDS | SF_CLOSE}};
#define GR_SONG_BARS 72u
static struct {
    uint32_t pat, prog, key, rhythm, build_bars;  /* the main loop's choices */
    float lv[NDR];                               /* each drum's level */
    /* the chords playing */
    int8_t chord[5];
    /* BUILD / DROP */
    volatile uint8_t bd, drop_now;               /* BD_*; the player asked for the drop at the next bar */
    uint32_t bd_from;                            /* the step the build began */
    float bd_p;                                  /* how far, 0..1 */
    float hp_fc;                                 /* the build's high-pass, Hz (20: none) */
    float riser;                                 /* its level */
    uint8_t mute_kick, hush;                     /* the build's last bar: no kick; its last beat: nothing */
    volatile uint32_t drops, drop_step;          /* drops so far, the step of the last (the screen's flash) */
    uint32_t bd_bars, style, style_now;          /* this build's bars; STYLE, and the one this build plays */
    float tempo_up, bpm_add;                     /* TEMPO UP (BPM at the drop); the build's now (the clock adds it) */
    float vary;                                  /* VARY 0..1: hits left out, ghosts added */
    uint32_t rnd;
    uint8_t song;                                /* PLAY: 0 LOOP, 1 SONG */
    volatile uint8_t sec, sec_bar;               /* SONG: the section, its bar (the screen) */
    uint8_t flags;                               /* what plays now (SF_*; LOOP: all) */
    float song_lp;                               /* SONG's low-pass, Hz (20000: open) */
    volatile uint32_t pf_auto;                   /* PERFORM's tricks the build plays (STUTTER) */
} gr = {.rnd = 0xC0FFEEu, .flags = 0xFF, .song_lp = 20000.0f, .bd_bars = 4, .pat = 1, .prog = 1, .key = 9, .rhythm = CR_HOLD, .build_bars = 4,
        .lv = {1.0f, 0.8f, 0.6f, 0.6f, 0.55f, 0.6f, 0.6f}, .chord = {-1, -1, -1, -1, -1}, .hp_fc = 20.0f};

static float gr_vel(char c) { return c == 'X' ? 1.0f : c == 'x' ? 0.78f : c == 'g' ? 0.34f : 0.0f; }

static void gr_chord_off(void)
{
    uint32_t i;
    for (i = 0; i < 5u; i++)
        if (gr.chord[i] >= 0) {
            kp_stop(gr.chord[i], 1);
            gr.chord[i] = -1;
        }
}

static void gr_chord_on(uint32_t which, float vel)
{
    const uint8_t *c = GR_PROG[gr.prog][which & 3u];
    int32_t root = 48 + (int32_t)((gr.key + c[0]) % 12u);
    uint32_t i;
    gr_chord_off();
    for (i = 0; i < 5u; i++) {
        int32_t iv = CH_IV[c[1]][i];
        if (iv < 0)
            break;
        gr.chord[i] = (int8_t)(root + iv);
        kp_start(root + iv, vel, 1);
    }
}

/* the player: BUILD (ARP). Playing: the build starts at the next bar; building: the drop comes at the next bar */
static void gr_build_button(void)
{
    if (gr.bd == BD_BUILD)
        gr.drop_now = 1;
    else if (gr.bd == BD_NONE)
        gr.bd = BD_ARMED;
}

static void gr_drop(void)
{
    gr.bd = BD_NONE;
    gr.bpm_add = 0.0f;
    gr.pf_auto = 0;
    gr.drop_now = 0;
    gr.hp_fc = 20.0f;
    gr.riser = 0.0f;
    gr.mute_kick = gr.hush = 0;
    gr.drops++;
    gr.drop_step = ck.step;
    dr_hit(DR_OPEN, 1.0f);
    dr_hit(DR_CLAP, 1.0f);
}

static inline uint32_t gr_rand(void)
{
    gr.rnd = gr.rnd * 1664525u + 1013904223u;
    return gr.rnd >> 16;                         /* 0 .. 65535 */
}

/* the build starts at this step, bars long */
static void gr_build_start(uint32_t step, uint32_t bars)
{
    gr.bd = BD_BUILD;
    gr.bd_from = step;
    gr.bd_bars = bars;
    gr.style_now = gr.style == BS_MIX ? gr_rand() % 3u : gr.style;
}

/* the audio interrupt: step `step` begins */
static void gr_step(uint32_t step)
{
    uint32_t s = step & 15u, d, bar_start = s == 0u, bar = step >> 4;
    const gr_pat_t *p = &GR_PAT[gr.pat % GR_NPAT];
    /* SONG: the section, at each bar */
    if (gr.song && bar_start) {
        uint32_t b = bar % GR_SONG_BARS, k = 0;
        while (b >= GR_SONG[k].bars) {
            b -= GR_SONG[k].bars;
            k++;
        }
        gr.sec = (uint8_t)k;
        gr.sec_bar = (uint8_t)b;
        gr.flags = GR_SONG[k].flags;
        if ((gr.flags & SF_BUILD) && b == 0u && gr.bd == BD_NONE)
            gr_build_start(step, GR_SONG[k].bars);
        gr.song_lp = gr.flags & SF_OPEN ? 300.0f * fm_exp2f(6.0f * (float)b / (float)GR_SONG[k].bars)
                   : gr.flags & SF_CLOSE && b >= GR_SONG[k].bars - 4u ? 20000.0f * fm_exp2f(-1.5f * (float)(b - GR_SONG[k].bars + 5u))
                   : 20000.0f;
    } else if (!gr.song) {
        gr.flags = 0xFF;
        gr.song_lp = 20000.0f;
    }
    /* BUILD / DROP, at the bar */
    if (bar_start) {
        if (gr.bd == BD_ARMED)
            gr_build_start(step, gr.build_bars);
        else if (gr.bd == BD_BUILD && (gr.drop_now || step - gr.bd_from >= gr.bd_bars * 16u))
            gr_drop();
    }
    if (gr.bd == BD_BUILD) {
        uint32_t len = gr.bd_bars * 16u, in = step - gr.bd_from;
        gr.bd_p = (float)in / (float)len;
        gr.hp_fc = 20.0f * fm_exp2f(6.0f * gr.bd_p * gr.bd_p);     /* 20 Hz .. 1.3 kHz */
        gr.riser = gr.bd_p * gr.bd_p;
        gr.bpm_add = gr.tempo_up * gr.bd_p * gr.bd_p;              /* TEMPO UP: faster toward the drop */
        gr.mute_kick = gr.style_now == BS_KICK ? 0 : len - in <= 16u && len > 16u;
        gr.hush = len - in <= 4u || (gr.drop_now && s >= 12u);
        /* STUTTER: the last bar played from PERFORM's history, eighths then sixteenths */
        gr.pf_auto = gr.style_now != BS_STUTTER || gr.hush || len - in > 16u ? 0u
                   : len - in > 8u ? 1u << PF_STUT8 : 1u << PF_STUT16;
    }
    /* RISE, by itself: ending on the bar the drop (or every 8 or 4 bars) comes */
    if (rs.autom != RA_OFF) {
        uint32_t l16 = RS_LEN_16TH[rs.lenix % 5u], go = 0;
        if (s == 16u - l16) {
            if (rs.autom == RA_DROP)
                go = gr.bd == BD_BUILD && (gr.drop_now || step + 16u - s - gr.bd_from >= gr.bd_bars * 16u);
            else
                go = (bar + 1u) % (rs.autom == RA_8BARS ? 8u : 4u) == 0u;
        }
        if (go)
            rs_start(40 + (int32_t)((gr.key + 5u) % 12u), (uint32_t)((float)l16 * SR * 15.0f / ck.bpm), 0.9f);
    }
    /* the chords: one every two bars */
    if (gr.prog && !gr.hush && (gr.flags & SF_CHORDS)) {
        uint32_t which = (step >> 5) & 3u, at = step & 31u;
        if (gr.rhythm == CR_HOLD && at == 0u)
            gr_chord_on(which, 0.8f);
        else if (gr.rhythm == CR_STABS && (s == 0u || s == 3u || s == 6u || s == 10u))
            gr_chord_on(which, s ? 0.6f : 0.85f);
        else if (gr.rhythm == CR_STABS && (s == 2u || s == 5u || s == 8u || s == 12u))
            gr_chord_off();
        else if (gr.rhythm == CR_PULSE && (s & 1u) == 0u)
            gr_chord_on(which, s % 4u ? 0.55f : 0.8f);
        else if (gr.rhythm == CR_PULSE)
            gr_chord_off();
    } else
        gr_chord_off();
    if (gr.hush)
        return;
    /* the drums */
    for (d = 0; d < NDR; d++) {
        char c = p->d[d][s];
        float v = c == 'c' ? (gr_rand() < 32768u ? 0.78f : 0.0f)
                : c == 'a' ? (bar & 1u ? 0.0f : 0.78f)
                : c == 'b' ? (bar & 1u ? 0.78f : 0.0f) : gr_vel(c);
        uint32_t on = d == DR_KICK ? gr.flags & SF_KICK : d == DR_HAT || d == DR_OPEN || d == DR_SHAKER ? gr.flags & SF_HATS
                    : gr.flags & SF_CLAP;
        if (gr.vary > 0.0f && d != DR_KICK) {    /* VARY: a hit left out now and then, a ghost added */
            if (v > 0.0f && c != 'X' && (float)gr_rand() < gr.vary * 0.2f * 65536.0f)
                v = 0.0f;
            else if (v == 0.0f && (d == DR_HAT || d == DR_SHAKER || d == DR_SNARE) &&
                     (float)gr_rand() < gr.vary * 0.12f * 65536.0f)
                v = 0.26f;
        }
        if (!on)
            v = 0.0f;
        if (gr.bd == BD_BUILD) {
            uint32_t every = gr.bd_p < 0.5f ? 4u : gr.bd_p < 0.75f ? 2u : 1u;
            float roll = s % every == 0u ? 0.3f + 0.7f * gr.bd_p : 0.0f;
            if (d == DR_SNARE)                   /* the roll: beats, eighths, sixteenths, louder as it goes */
                v = gr.style_now == BS_KICK ? 0.0f : gr.style_now == BS_STUTTER ? (s % 4u ? 0.0f : roll) : roll;
            else if (d == DR_KICK)
                v = gr.style_now == BS_KICK ? fm_maxf(roll, 0.0f) : gr.mute_kick ? 0.0f : v;
            else if (d == DR_CLAP && gr.bd_p > 0.5f)
                v = 0.0f;
        }
        if (v > 0.0f) {
            dr_hit(d, v);
            if (d == DR_KICK && pm.src == PS_KICK)
                pm_pull();
        }
    }
    if (pm.src == PS_BEAT && s % 4u == 0u && !gr.mute_kick && (gr.flags & SF_KICK))
        pm_pull();
}

/* stopped (the audio interrupt, after the main loop's PLAY): the chords let go, the build forgotten */
static void gr_stop(void)
{
    gr_chord_off();
    gr.bd = BD_NONE;
    gr.drop_now = 0;
    gr.hp_fc = 20.0f;
    gr.riser = 0.0f;
    gr.mute_kick = gr.hush = 0;
    gr.bpm_add = 0.0f;
    gr.pf_auto = 0;
    gr.flags = 0xFF;
    gr.song_lp = 20000.0f;
}
