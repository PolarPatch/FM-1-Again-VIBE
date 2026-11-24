/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 PolarPatch */
/* FM-1 Again VIBE: the pages, the controls and the display (480 x 320), in FM-1 Again's look: black and white, big
 * type, the sound drawn as an audio editor draws a track (its blue the one colour). Part of main.c (the control flow:
 * vibe_on_event, vibe_on_draw).
 *
 * The controls: the 25 keys play what ALGORITHM chose (A); encoders 1-4 the page's four values, B its main choice;
 * K1-K6 the pages (SOUND, PUMP, GROOVE, BUILD, FX, MIX: a key again, the next page of its group), RETURN back to HOME,
 * HOME the controls' overview; PLAY, STOP; F1 HOME, F2 BUILD (then DROP), F3 what the keys play, F4 LOOP / SONG,
 * F5 SAVE, F6 the page's main choice on; the 16 pads PERFORM's thirteen tricks while held, BUILD, PLAY, STOP – and with
 * CHOP on the keys, its slices. DATA and ENTER reach every control on the display (FOCUS). */

/* the pages: FM-1 Again's (its app.c), their four values and their main choice */
enum { PG_HOME, PG_GROOVE, PG_RISE, PG_BUILD, PG_KEYS, PG_VOX, PG_CHOP, PG_PUMP, PG_SPACE, PG_ECHO, PG_SETUP, PG_TAPE,
       PG_MIX, NPG };
static const struct {
    const char *title;
    uint8_t k[4], preset, group;
} PG[NPG] = {
    [PG_HOME] = {"FM-1 AGAIN", {PA_DEPTH, PA_MIX, PA_FILTER, PA_TEMPO}, PA_PAT, 6},
    [PG_GROOVE] = {"GROOVE", {PA_SWING, PA_VARY, PA_KICK, PA_HATS}, PA_PAT, 2},
    [PG_RISE] = {"RISE", {PA_RLEN, PA_RBRIGHT, PA_RSUCK, PA_RAUTO}, PA_RAUTO, 2},
    [PG_BUILD] = {"BUILD", {PA_BUILD, PA_BSTYLE, PA_TEMPOUP, PA_PLAYMODE}, PA_BSTYLE, 3},
    [PG_KEYS] = {"KEYS", {PA_TONE, PA_DECAY, PA_PROG, PA_RHYTHM}, PA_CHORD, 0},
    [PG_VOX] = {"VOX", {PA_VOWELS, PA_GATE, PA_FORMANT, PA_BREATH}, PA_VOWELS, 0},
    [PG_CHOP] = {"CHOP", {PA_SLICE, PA_LENGTH, PA_PITCH, PA_CMODE}, PA_CREV, 0},
    [PG_PUMP] = {"PUMP", {PA_DEPTH, PA_RELEASE, PA_SHAPE, PA_SOURCE}, PA_SOURCE, 1},
    [PG_SPACE] = {"SPACE", {PA_SIZE, PA_DAMP, PA_MIX, PA_SHIMMER}, PA_SIZE, 4},
    [PG_ECHO] = {"ECHO", {PA_ETIME, PA_EFB, PA_EWEAR, PA_EMIX}, PA_EPING, 4},
    [PG_SETUP] = {"SETUP", {PA_KEY, PA_KEYS_LV, PA_VOX_LV, PA_RISE_LV}, PA_SAVER, 5},
    [PG_TAPE] = {"TAPE", {PA_TWOW, PA_TSAT, PA_TTONE, PA_THISS}, PA_TAPE, 4},
    [PG_MIX] = {"MIX", {PA_KICK, PA_HATS, PA_CLAP, PA_RISE_LV}, PA_PLAYMODE, 5},
};
/* K1-K6 (house style: their names on the display), each a group of pages */
static const char *const KG_NAME[6] = {"SOUND", "PUMP", "GROOVE", "BUILD", "FX", "MIX"};
static const uint8_t KG_PAGES[6][4] = {{PG_KEYS, PG_VOX, PG_CHOP, 99}, {PG_PUMP, 99, 99, 99},
                                       {PG_GROOVE, PG_RISE, 99, 99}, {PG_BUILD, 99, 99, 99},
                                       {PG_SPACE, PG_ECHO, PG_TAPE, 99}, {PG_MIX, PG_SETUP, 99, 99}};
static const char *const F_NAME[6] = {"HOME", "BUILD", "PLAYS", "SONG", "SAVE", "PRESET"};
static const char *const IN_NAMES[NIN] = {"KEYS", "VOX", "CHOP", "RISE", "DRUMS"};

/* the pads: PERFORM's tricks in the FM-1's SEL + white key order, then BUILD, PLAY, STOP */
#define PAD_BUILD 13
#define PAD_PLAY 14
#define PAD_STOP 15
static const char *const PAD_TAG[16] = {"R16", "R8", "R4", "REV", "STP", "LO", "HI", "ECH", "SPC", "KIK", "DRM",
                                        "R32", "GAT", "BLD", "PLY", "STP"};

static const char *pg_group(uint32_t i)          /* a parameter's Editor tab and card: "SOUND · KEYS" */
{
    static char g[NPA][24];
    uint32_t p, k;
    for (p = 0; p < NPG; p++)
        for (k = 0; k < 5u; k++)
            if ((k < 4u ? PG[p].k[k] : PG[p].preset) == i) {
                g[i][0] = 0;
                strcpy(g[i], PG[p].group == 6 ? "HOME" : KG_NAME[PG[p].group]);
                strcpy(g[i] + strlen(g[i]), " · ");
                strcpy(g[i] + strlen(g[i]), PG[p].title);
                return g[i];
            }
    return "MIX · SETUP";
}

/* ---- the state of the display (the control flow's) */
static uint8_t help;                             /* HOME: the controls' overview */
static uint8_t pad_held[16];
static uint8_t shift;                            /* SHIFT held */
static uint8_t lit[4];                           /* a value just turned: its cell lit (frames left) */
static char msg[2][32];
static uint64_t msg_until;
static float sr_host = 44100.0f;
static void rec_to_chop(void) { set.inst = IN_CHOP, set.page = PG_CHOP; }

static void say(const char *a, const char *b)
{
    strncpy(msg[0], a, 31), msg[0][31] = 0;
    strncpy(msg[1], b ? b : "", 31), msg[1][31] = 0;
    msg_until = vibe_frames() + (uint64_t)(sr_host * 1.3f);
}

static void cat(char *d, const char *s) { strcpy(d + strlen(d), s); }
static void num(char *d, int32_t v) { char t[16]; cat(d, vg_itoa(v, t)); }

/* a parameter's value as the screen shows it */
static void value_text(uint32_t i, char *t)
{
    int32_t v = set.p[i];
    t[0] = 0;
    switch (PA[i].fmt) {
    case FMT_PCT: num(t, v * 100 / PA[i].top); cat(t, "%"); break;
    case FMT_BPM: num(t, 80 + v); break;
    case FMT_LIST: cat(t, PA[i].list[v]); break;
    case FMT_MS: num(t, i == PA_GATE ? 40 + v * 6 : 60 + v * 6); cat(t, "MS"); break;
    case FMT_KEY: cat(t, KEY_NAMES[v]); break;
    case FMT_BARS: cat(t, BARS_NAME[v]); cat(t, " BARS"); break;
    case FMT_FILTER:
        if (v > 52) cat(t, "HP "), num(t, (v - 52) * 100 / 48);
        else if (v < 48) cat(t, "LP "), num(t, (48 - v) * 100 / 48);
        else cat(t, "OPEN");
        break;
    case FMT_SIZE:
        if (v >= 127) cat(t, "INF");
        else { int32_t d = RV_RT60_X10[v]; num(t, d / 10); cat(t, "."); num(t, d % 10); cat(t, "S"); }
        break;
    case FMT_SEMI: if (v > 12) cat(t, "+"); num(t, v - 12); break;
    case FMT_ONE: num(t, v + 1); break;
    case FMT_UP: cat(t, "+"); num(t, v); cat(t, " BPM"); break;
    default: num(t, v); break;
    }
}

/* CHOP: the slice in hand (PITCHED: SLICE; SLICES: the one played last) */
static uint32_t chop_cur(void)
{
    uint32_t n = ch.s.nslices ? ch.s.nslices : 1u;
    return (set.p[PA_CMODE] == CM_PITCHED ? set.p[PA_SLICE] : ch.last_slice) % n;
}

/* parameter i to v: what goes with it (FM-1 Again's param_set), the host told, the engine after */
static void param_set_ui(uint32_t i, int32_t v)
{
    char t[32];
    uint32_t k;
    v = clamp(v, 0, PA[i].top);
    if (v == set.p[i])
        return;
    set.p[i] = (uint8_t)v;
    if (i == PA_PAT)                             /* a new groove brings its own swing */
        set.p[PA_SWING] = (uint8_t)(GR_PAT[v].swing / 0.33f * 100.0f + 0.5f);
    if (i == PA_TAPE)
        for (k = 0; k < 4u; k++)
            set.p[PA_TWOW + k] = TP_SET[v][k];
    if (i == PA_CREV)
        set.crev = v ? set.crev | 1u << chop_cur() : set.crev & ~(1u << chop_cur());
    for (k = 0; k < NPA; k++)                    /* (the ones it moved too) */
        vibe_param_set((int)k, (float)(set.p[k] + pa_off(k)));
    q_put(Q_APPLY, 0, 0, 0);
    value_text(i, t);
    say(PA[i].name, t);
}
static void param_turn(uint32_t i, float d)
{
    static float acc[NPA];                       /* (fractional detents add up) */
    int32_t n;
    acc[i] += d;
    n = (int32_t)acc[i];
    acc[i] -= (float)n;
    if (n)
        param_set_ui(i, (int32_t)set.p[i] + n);
}

static void page_go(uint32_t p)
{
    set.page = (uint8_t)p;
    say(PG[p].title, "");
}
static void group_key(uint32_t g)                /* K1-K6: the group's first page; again: its next */
{
    uint32_t j, p = KG_PAGES[g][0];
    for (j = 0; j < 4u && KG_PAGES[g][j] != 99; j++)
        if (KG_PAGES[g][j] == set.page) {
            p = j + 1u < 4u && KG_PAGES[g][j + 1u] != 99 ? KG_PAGES[g][j + 1u] : KG_PAGES[g][0];
            break;
        }
    page_go(p);
}

static void ui_init(void)
{
    uint32_t k;
    vf_mode(VF_ALWAYS);
    vf_clicks(1);
    for (k = 0; k < 6u; k++) {
        vibe_label(VB_K1 + (int)k, KG_NAME[k]);
        vibe_label(VB_F1 + (int)k, F_NAME[k]);
    }
}

static void ui_event(const vibe_event *e)
{
    int id, r;
    if (br_event(e))                             /* LOAD's list open: DATA, ENTER, RETURN are its */
        return;
    r = vf_event(e, &id);
    if (r != VF_NO)
        return;
    switch (e->type) {
    case VIBE_EV_NOTE: {
        uint32_t v = (uint32_t)(e->value * 127.0f + 0.5f);
        q_put(Q_NOTE, e->id, (uint8_t)(e->down ? (v ? v : 1u) : 0u), e->down);
        break;
    }
    case VIBE_EV_PAD:
        if (e->id >= 16)
            break;
        if (set.inst == IN_CHOP && shift) {      /* CHOP: SHIFT + a pad turns its slice around (REVERSE) */
            if (e->down && e->id < ch.s.nslices) {
                char t[12] = "SLICE ";
                set.crev ^= 1u << e->id;
                q_put(Q_APPLY, 0, 0, 0);
                num(t, (int32_t)e->id + 1);
                say(t, set.crev >> e->id & 1u ? "REVERSE" : "FORWARD");
            }
            break;
        }
        if (set.inst == IN_CHOP && e->id < 16) {  /* CHOP on the keys: the pads are its slices */
            uint32_t v = (uint32_t)(e->value * 127.0f + 0.5f);
            q_put(Q_SLICE, e->id, (uint8_t)(v ? v : 100u), e->down);
        } else if (e->id < NPF) {
            q_put(Q_TRICK, e->id, 0, e->down);
            if (e->down)
                say(PF_NAME[e->id], "");
        } else if (e->down)
            q_put(e->id == PAD_BUILD ? Q_BUILD : e->id == PAD_PLAY ? Q_PLAY : Q_STOP, 0, 0, 0);
        pad_held[e->id] = e->down;
        break;
    case VIBE_EV_ENCODER:
        if (e->id <= VE_4) {
            param_turn(PG[set.page].k[e->id], e->value);
            lit[e->id] = 30;
        } else if (e->id == VE_A) {
            static float acc;
            int32_t n;
            acc += e->value, n = (int32_t)acc, acc -= (float)n;
            if (n) {
                set.inst = (uint8_t)clamp((int32_t)set.inst + n, 0, NIN - 1);
                say("KEYS PLAY", IN_NAMES[set.inst]);
            }
        } else if (e->id == VE_B)
            param_turn(PG[set.page].preset, e->value);
        break;
    case VIBE_EV_BUTTON:
        if (e->id == VB_SHIFT)
            shift = e->down;
        if (!e->down)
            break;
        if (e->id >= VB_K1 && e->id <= VB_K6)
            group_key((uint32_t)(e->id - VB_K1));
        else if (e->id == VB_RETURN)
            help ? (void)(help = 0) : page_go(PG_HOME);
        else if (e->id == VB_HOME)
            help = !help;
        else if (e->id == VB_PLAY)
            q_put(Q_PLAY, 0, 0, 0);
        else if (e->id == VB_STOP)
            q_put(Q_STOP, 0, 0, 0);
        else if (e->id == VB_REC)
            rec_button();
        else if (e->id == VB_F1)
            page_go(PG_HOME);
        else if (e->id == VB_F2) {
            q_put(Q_BUILD, 0, 0, 0);
            say(gr.bd == BD_BUILD ? "DROP" : "BUILD", gr.bd == BD_BUILD ? "AT THE BAR" : BARS_NAME[set.p[PA_BUILD]]);
        } else if (e->id == VB_F3) {
            set.inst = (uint8_t)((set.inst + 1u) % NIN);
            say("KEYS PLAY", IN_NAMES[set.inst]);
        } else if (e->id == VB_F4)
            param_set_ui(PA_PLAYMODE, !set.p[PA_PLAYMODE]);
        else if (e->id == VB_F5) {
            settings_write();
            say("SAVED", "");
        } else if (e->id == VB_F6 && set.page == PG_CHOP)
            br_open();                           /* CHOP: LOAD a WAV from the host's media */
        else if (e->id == VB_F6)
            param_set_ui(PG[set.page].preset, set.p[PG[set.page].preset] + 1 > PA[PG[set.page].preset].top
                                                  ? 0 : set.p[PG[set.page].preset] + 1);
        break;
    default:
        break;
    }
}

/* ---- the display */
#define C_K 0x000000
#define C_W 0xFFFFFF
#define C_DIM 0x8A8A8A
#define C_BG 0xC0C0C0
#define C_PEAK 0x3232C8
#define C_RMS 0x6464DA
#define C_BAR 0xE4E4E4
#define C_INK 0x181818

static void header_text(char *t)                 /* line 1: the trick held, the drop, the build, the bar, the page */
{
    uint32_t held = pfx.held | gr.pf_auto, step = ck.step & 255u, k;
    t[0] = 0;
    if (rec_on) {                                /* recording: its seconds */
        cat(t, "REC ");
        num(t, (int32_t)(rec_shown_n / REC_RATE));
        cat(t, ".");
        num(t, (int32_t)(rec_shown_n % REC_RATE * 10u / REC_RATE));
        cat(t, " S");
        return;
    }
    if (enc.busy) {
        cat(t, "CUTTING...");
        return;
    }
    if (ld.on) {
        cat(t, "LOADING ");
        num(t, (int32_t)(ld.n / REC_RATE));
        cat(t, " S");
        return;
    }
    if (held) {
        for (k = 0; k < NPF && !(held >> k & 1u); k++)
            ;
        cat(t, PF_NAME[k]);
    } else if (set.page != PG_HOME)
        cat(t, PG[set.page].title);
    else if (ck.run && gr.drops && ck.step - gr.drop_step < 8u)
        cat(t, "DROP");
    else if (ck.run && gr.bd == BD_BUILD) {
        cat(t, "DROP IN ");
        num(t, (int32_t)(gr.bd_bars - (ck.step - gr.bd_from) / 16u));
    } else if (ck.run && gr.bd == BD_ARMED)
        cat(t, "BUILD...");
    else if (ck.run) {
        cat(t, "BAR ");
        num(t, (int32_t)(step / 16u) + 1);
        cat(t, ".");
        num(t, (int32_t)(step % 16u) / 4 + 1);
    } else
        cat(t, "FM-1 AGAIN");
}

static void draw_lanes(int y0)                   /* the last 16 bars, left over right, beats ticked */
{
    uint32_t c, ch2, head = wv_head;
    vg_rect(0, y0, 480, 132, C_BG);
    for (ch2 = 0; ch2 < 2u; ch2++) {
        int mid = y0 + 33 + (int)ch2 * 66;
        for (c = 0; c < WV_W; c++) {
            int x = 8 + (int)c * 2, p = wv_pk[ch2][c], q = wv_rms[ch2][c];
            if (!p)
                vg_rect(x, mid, 2, 1, C_PEAK);
            else {
                vg_rect(x, mid - p * 30 / 50, 2, 2 * (p * 30 / 50) + 1, C_PEAK);
                if (q)
                    vg_rect(x, mid - q * 30 / 50, 2, 2 * (q * 30 / 50) + 1, C_RMS);
            }
            if (c * 16u / WV_W != (c ? (c - 1u) * 16u / WV_W : 99u))
                vg_rect(x, mid - 33, 1, 6, C_INK), vg_rect(x, mid + 27, 1, 6, C_INK);
            else if (c * 64u / WV_W != (c ? (c - 1u) * 64u / WV_W : 99u))
                vg_rect(x, mid - 33, 1, 3, C_INK), vg_rect(x, mid + 30, 1, 3, C_INK);
        }
    }
    vg_rect(0, y0 + 66, 480, 1, C_K);
    if (ck.run)
        vg_rect(8 + (int)head * 2, y0, 2, 132, C_INK);   /* the playhead */
}

static void draw_sample(int y0)                  /* CHOP's page: its sample and slices in the panel's place */
{
    uint32_t k, c;
    char t[8];
    vg_rect(0, y0, 480, 132, C_BG);
    if (!ch.s.blocks || !ch.s.n)
        return;
    for (k = 0; k < ch.s.nslices; k++) {
        int x0 = 8 + (int)((uint64_t)ch.s.slice[k] * 464u / ch.s.n), x1 = 8 + (int)((uint64_t)ch.s.slice[k + 1u] * 464u / ch.s.n);
        if (k == chop_cur())
            vg_rect(x0, y0, x1 - x0, 132, C_BAR);
        vg_rect(x0, y0, 1, 132, C_INK);
        t[0] = 0;
        if (set.crev >> k & 1u)
            cat(t, "<");
        num(t, (int32_t)k + 1);
        if (x1 - x0 >= 16)
            vg_text(t, x0 + 3, y0 + 2, VG_SMALL, C_INK, VG_LEFT);
    }
    for (c = 0; c < 232u; c++) {                 /* the peaks: a look at every 64th sample of each column */
        uint32_t a = ch.s.n * c / 232u, b = ch.s.n * (c + 1u) / 232u, i;
        int32_t pk = 0;
        adp_t d;
        adp_seek(ch.s.blocks, &d, a);
        for (i = a; i < b && i < a + 512u; i++) {
            int32_t v = adp_next(ch.s.blocks, &d);
            v = v < 0 ? -v : v;
            pk = v > pk ? v : pk;
        }
        pk = pk * 60 / 32768;
        vg_rect(8 + (int)c * 2, y0 + 66 - pk, 2, 2 * pk + 1, C_PEAK);
    }
}

static void ui_draw(uint16_t *fb)
{
    static const char *const HELP[] = {
        "KEYS", "play what A chose", "ENCODER 1-4", "the page's values", "ENCODER A", "what the keys play",
        "ENCODER B", "the page's main choice", "K1-K6", "SOUND PUMP GROOVE BUILD FX MIX", "RETURN", "HOME page",
        "PADS 1-13", "tricks while held", "PADS 14-16", "BUILD, PLAY, STOP", "PADS (CHOP)", "its slices (SHIFT: reversed)",
        "PLAY / STOP", "the groove", "F2", "build, then drop", "F3", "what the keys play", "F4", "LOOP / SONG",
        "F5", "save", "F6", "the main choice on (CHOP: LOAD a WAV)", "REC", "IN 1 into CHOP (again: stop)", "A WAV", "dropped on the window: CHOP", "DATA + ENTER", "every control"};
    char t[48], v[24];
    uint32_t k, p = set.page;
    vg_begin(fb);
    if (help) {
        vg_controls(PG[p].title, HELP, (int)(sizeof HELP / sizeof HELP[0] / 2u));
        return;
    }
    vg_clear(C_K);
    /* the header: the big line, the tempo, a message or the page's choice */
    header_text(t);
    vg_text(t, 10, 2, VG_HUGE, C_W, VG_LEFT);
    if (vibe_frames() < msg_until) {
        strcpy(t, msg[0]);
        cat(t, "  ");
        cat(t, msg[1]);
        vg_text(t, 470, 8, VG_BIG, C_W, VG_RIGHT);
    } else {
        t[0] = 0;
        num(t, (int32_t)(ck.bpm + 0.5f));
        cat(t, " BPM");
        vg_text(t, 470, 8, VG_BIG, C_W, VG_RIGHT);
        value_text(PG[p].preset, v);
        vg_text(v, 470, 36, VG_MED, C_DIM, VG_RIGHT);
    }
    /* the bar: the group's pages (the one open inverted), the groove and what the keys play */
    vg_rect(0, 66, 480, 18, C_BAR);
    {
        int x = 6;
        uint32_t g = PG[p].group;
        if (g < 6u)
            for (k = 0; k < 4u && KG_PAGES[g][k] != 99; k++) {
                const char *n = PG[KG_PAGES[g][k]].title;
                int w = vg_text_width(n, VG_BOLD) + 10;
                if (KG_PAGES[g][k] == p)
                    vg_rect(x - 4, 67, w, 16, C_INK);
                vg_text(n, x + 1, 68, VG_BOLD, KG_PAGES[g][k] == p ? C_W : C_INK, VG_LEFT);
                x += w + 4;
            }
        else
            vg_text("HOME", x, 68, VG_BOLD, C_INK, VG_LEFT);
        strcpy(t, PA[PA_PAT].list ? PAT_NAME[set.p[PA_PAT]] : "");
        cat(t, "   KEYS PLAY ");
        cat(t, IN_NAMES[set.inst]);
        vg_text(t, 474, 68, VG_BOLD, C_INK, VG_RIGHT);
    }
    if (p == PG_CHOP)
        draw_sample(84);
    else
        draw_lanes(84);
    /* the pads' strip: what each does, the ones held inverted */
    for (k = 0; k < 16u; k++) {
        int x = (int)k * 30;
        const char *n = set.inst == IN_CHOP ? 0 : PAD_TAG[k];
        uint32_t on = pad_held[k] || (k < NPF && (pfx.held >> k & 1u));
        if (on)
            vg_rect(x, 217, 30, 16, C_W);
        if (n)
            vg_text(n, x + 15, 218, VG_SMALL, on ? C_K : C_DIM, VG_CENTER);
        else {
            t[0] = 0;
            num(t, (int32_t)k + 1);
            vg_text(t, x + 15, 218, VG_SMALL, on ? C_K : C_DIM, VG_CENTER);
        }
        vibe_led_pad((int)k, set.inst == IN_CHOP ? C_PEAK : k >= PAD_BUILD ? 0xA78BFA : C_W, on ? 1.0f : 0.15f);
    }
    /* encoders 1-4: the page's four values, the one turned lit */
    for (k = 0; k < 4u; k++) {
        int x = (int)k * 120;
        uint32_t i = PG[p].k[k], on = lit[k] > 0;
        if (lit[k])
            lit[k]--;
        if (on)
            vg_rect(x, 236, 119, 46, C_W);
        else
            vg_rect(x + 119, 240, 1, 38, 0x333333);
        vg_text(PA[i].name, x + 8, 238, VG_SMALL, on ? C_K : C_DIM, VG_LEFT);
        value_text(i, v);
        vg_text(v, x + 8, 254, VG_BIG, on ? C_K : C_W, VG_LEFT);
    }
    /* F1-F6: their captions */
    vg_rect(0, 282, 480, 1, 0x333333);
    for (k = 0; k < 6u; k++)
        vg_text(k == 5u && p == PG_CHOP ? "LOAD" : F_NAME[k], (int)k * 80 + 40, 293, VG_BOLD, C_W, VG_CENTER);
    /* FOCUS: DATA walks the four values and the soft keys */
    vf_begin();
    for (k = 0; k < 4u; k++)
        vf_encoder(VE_1 + (int)k, (int)k * 120, 236, 119, 46);
    for (k = 0; k < 6u; k++)
        vf_key(VB_F1 + (int)k, (int)k * 80, 284, 80, 36);
    vf_end(C_W);
    if (br.open)
        br_draw();
    vibe_link_page(PG[p].title);
    for (k = 0; k < 4u; k++) {
        value_text(PG[p].k[k], v);
        vibe_link_encoder(VE_1 + (int)k, PA[PG[p].k[k]].name, v);
    }
}
