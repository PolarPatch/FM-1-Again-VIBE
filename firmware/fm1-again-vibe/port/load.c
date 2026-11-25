/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 PolarPatch */
/* FM-1 Again VIBE: a WAV let go on the plugin's window (SDK 14, vibe_media_dropped) becomes CHOP's sample: up to a
 * minute (the FM-1 holds 8 s), any rate, 16, 24 or 32 bit or float, mono or stereo (the channels mixed), read a piece
 * a frame (vibe_media_read: never from the audio), taken down to 22050 Hz into the recording's buffer, then coded and
 * cut as a recording is (rec.c). Part of main.c (the control flow). */

static struct {
    char path[VIBE_MEDIA_PATH];
    char name[16];
    uint8_t on;                                  /* busy */
    uint16_t fmt;                                /* 1 PCM, 3 float */
    uint16_t ch, bits;
    uint32_t rate, data, size, at;               /* the data chunk's offset and bytes; read up to at */
    float acc, sum;                              /* the rate taken down: as rec_audio */
    uint32_t cnt, n, carry;                      /* (carry: the bytes of a frame cut in two by a read) */
    int stage;                                   /* 0 the head, 1 the samples */
} ld;
static uint8_t ld_buf[65536];

static uint32_t le32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static void ld_start(const char *path)
{
    const char *b = path;
    uint32_t i, k = 0;
    if (rec_on || enc.busy || ld.on)
        return;
    memset(&ld, 0, sizeof ld);
    strncpy(ld.path, path, sizeof ld.path - 1u);
    for (i = 0; path[i]; i++)                    /* its name, for the screen: the file's, without its folders */
        if (path[i] == '/')
            b = path + i + 1;
    for (i = 0; b[i] && b[i] != '.' && k < 15u; i++)
        ld.name[k++] = (char)(b[i] >= 'a' && b[i] <= 'z' ? b[i] - 32 : b[i]);
    ld.on = 1;
    say("LOADING", ld.name);
}

static int ld_head(void)                         /* RIFF/WAVE: fmt and data. 1 = ready, 0 = not yet, -1 = not a WAV */
{
    int n = vibe_media_read(ld.path, 0, ld_buf, 4096);
    uint32_t p = 12;
    if (n == VIBE_MEDIA_PENDING)
        return 0;
    if (n < 44 || memcmp(ld_buf, "RIFF", 4) || memcmp(ld_buf + 8, "WAVE", 4))
        return -1;
    while (p + 8u <= (uint32_t)n) {
        uint32_t len = le32(ld_buf + p + 4u);
        if (!memcmp(ld_buf + p, "fmt ", 4) && p + 24u <= (uint32_t)n) {
            ld.fmt = le16(ld_buf + p + 8u);
            ld.ch = le16(ld_buf + p + 10u);
            ld.rate = le32(ld_buf + p + 12u);
            ld.bits = le16(ld_buf + p + 22u);
            if (ld.fmt == 0xFFFEu && len >= 40u)     /* (WAVE_FORMAT_EXTENSIBLE: its subformat's first two bytes) */
                ld.fmt = le16(ld_buf + p + 32u);
        } else if (!memcmp(ld_buf + p, "data", 4)) {
            ld.data = p + 8u, ld.size = len;
            break;
        }
        p += 8u + len + (len & 1u);
    }
    if (!ld.data || !ld.ch || ld.rate < 4000u || !((ld.fmt == 1 && (ld.bits == 16 || ld.bits == 24 || ld.bits == 32)) ||
                                                   (ld.fmt == 3 && ld.bits == 32)))
        return -1;
    return 1;
}

static void ld_take(float x)                     /* a mono sample at the file's rate */
{
    ld.sum += x, ld.cnt++;
    ld.acc += (float)REC_RATE / (float)ld.rate;
    while (ld.acc >= 1.0f && ld.n < REC_MAX) {
        float m = ld.sum / (float)(ld.cnt ? ld.cnt : 1u);
        ld.acc -= 1.0f;
        rec_buf[ld.n++] = (int16_t)clamp((int32_t)(m * 32767.0f), -32768, 32767);
        if (ld.cnt)
            ld.sum = 0.0f, ld.cnt = 0;
    }
}

static void ld_poll(void)                        /* (from vibe_on_draw, and on the host's MEDIA events) */
{
    char path[VIBE_MEDIA_PATH];
    int pad, x, y, n;
    uint32_t frame, i, c, got;
    if (!ld.on && !rec_on && !enc.busy && vibe_media_dropped(path, (int)sizeof path, &pad, &x, &y) == 1)
        ld_start(path);
    if (!ld.on)
        return;
    if (ld.stage == 0) {
        int r = ld_head();
        if (r < 0) {
            ld.on = 0;
            say("NOT A WAV", ld.name);
        } else if (r > 0)
            ld.stage = 1, ld.at = 0;
        return;
    }
    frame = (uint32_t)ld.ch * (ld.bits / 8u);
    for (got = 0; got < 32u && ld.at < ld.size && ld.n < REC_MAX; got++) {   /* (up to 32 reads a frame) */
        uint32_t room = (uint32_t)sizeof ld_buf - ld.carry, want = ld.size - ld.at < room ? ld.size - ld.at : room, total;
        n = vibe_media_read(ld.path, ld.data + ld.at, ld_buf + ld.carry, (int)want);
        if (n == VIBE_MEDIA_PENDING)
            return;
        if (n <= 0) {
            ld.size = ld.at;                     /* (shorter than it said, or gone: what came) */
            break;
        }
        ld.at += (uint32_t)n;                    /* (a read may stop anywhere: a frame cut in two waits for its rest) */
        total = ld.carry + (uint32_t)n;
        for (i = 0; i + frame <= total; i += frame) {
            float m = 0.0f;
            for (c = 0; c < ld.ch; c++) {
                const uint8_t *s = ld_buf + i + c * (ld.bits / 8u);
                float v;
                if (ld.fmt == 3) {
                    union { uint32_t u; float f; } q;
                    q.u = le32(s);
                    v = q.f;
                } else if (ld.bits == 16)
                    v = (float)(int16_t)le16(s) * (1.0f / 32768.0f);
                else if (ld.bits == 24)
                    v = (float)((int32_t)((uint32_t)s[0] << 8 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 24) >> 8) *
                        (1.0f / 8388608.0f);
                else
                    v = (float)(int32_t)le32(s) * (1.0f / 2147483648.0f);
                m += v;
            }
            ld_take(m / (float)ld.ch);
        }
        ld.carry = total - i;
        if (ld.carry)
            memmove(ld_buf, ld_buf + i, ld.carry);
    }
    if (ld.at >= ld.size || ld.n >= REC_MAX) {   /* all of it (or a minute): coded and cut as a recording */
        ld.on = 0;
        strcpy(rec_name, ld.name);
        rec_loaded = 1;
        if (ld.n)
            rec_done = ld.n + 1u;
    }
}

/* ---- LOAD (F6 on CHOP's page): the host's media – the VIBE's card and USB sticks, the browser's and the plugin's
 * folders – as a list: the sources, then their folders and WAV files; DATA chooses, ENTER opens a folder or loads a
 * file, RETURN goes up (at the top: closes). The device has no drag and drop: this is its way in. */
#define BR_N 96
static struct {
    uint8_t open, pending;
    char path[VIBE_MEDIA_PATH];                  /* "" the sources, else "<key>/<folders>" */
    char name[BR_N][48];
    char key[BR_N][24];                          /* (the sources: their keys) */
    uint8_t dir[BR_N];
    int n, sel;
} br;
static char br_buf[8192];

static void br_list(void)                        /* the list for br.path (again while the host says not yet) */
{
    int n;
    char *p, *e;
    br.pending = 0, br.n = 0;
    n = br.path[0] ? vibe_media_list(br.path, 0, br_buf, (int)sizeof br_buf - 1) : vibe_media_sources(br_buf, (int)sizeof br_buf - 1);
    if (n == VIBE_MEDIA_PENDING) {
        br.pending = 1;
        return;
    }
    if (!br.path[0] && vibe_media_add(0) == 1 && br.n < BR_N) {   /* (SDK 22: the browser's own folder, offered) */
        strcpy(br.name[br.n], "+ ADD FOLDER");
        strcpy(br.key[br.n], "+");
        br.dir[br.n++] = 0;
    }
    if (n <= 0)
        return;
    br_buf[n] = 0;
    for (p = br_buf; *p && br.n < BR_N; p = e + 1) {
        char *f1, *f2;
        e = strchr(p, '\n');
        if (!e)
            break;
        *e = 0;
        f1 = strchr(p, '\t');
        f2 = f1 ? strchr(f1 + 1, '\t') : 0;
        if (!f1 || !f2)
            continue;
        *f1 = 0;
        if (!br.path[0]) {                       /* a source: "<key>\t<flags>\t<label>" */
            strncpy(br.key[br.n], p, 23), br.key[br.n][23] = 0;
            strncpy(br.name[br.n], f2 + 1, 47), br.name[br.n][47] = 0;
            br.dir[br.n++] = 1;
        } else {                                 /* "d\t\t<name>" or "f\t<bytes>\t<name>": folders and WAVs */
            const char *nm = f2 + 1;
            size_t l = strlen(nm);
            int wav = l > 4 && (nm[l - 4] == '.') && (nm[l - 3] | 32) == 'w' && (nm[l - 2] | 32) == 'a' && (nm[l - 1] | 32) == 'v';
            if (p[0] != 'd' && !wav)
                continue;
            strncpy(br.name[br.n], nm, 47), br.name[br.n][47] = 0;
            br.key[br.n][0] = 0;
            br.dir[br.n++] = p[0] == 'd';
        }
    }
    if (br.sel >= br.n)
        br.sel = br.n ? br.n - 1 : 0;
}
static void br_open(void)
{
    memset(&br, 0, sizeof br);
    br.open = 1;
    vf_enable(0);                                /* (DATA and ENTER are the list's now) */
    br_list();
}
static void br_close(void)
{
    br.open = 0;
    vf_enable(1);
}
static void br_enter(void)
{
    char p[VIBE_MEDIA_PATH];
    if (br.sel >= br.n)
        return;
    if (!br.path[0] && !strcmp(br.key[br.sel], "+")) {   /* the browser asks the user for a folder (a new source) */
        vibe_media_add(1);
        return;
    }
    if (!br.path[0])
        strcpy(p, br.key[br.sel]);
    else {
        strcpy(p, br.path);
        cat(p, "/");
        cat(p, br.name[br.sel]);
    }
    if (br.dir[br.sel]) {
        strcpy(br.path, p);
        br.sel = 0;
        br_list();
    } else {
        br_close();
        ld_start(p);
    }
}
static void br_up(void)
{
    char *s = 0, *q;
    for (q = br.path; *q; q++)                   /* (the last '/': no strrchr in the SDK) */
        if (*q == '/')
            s = q;
    if (!br.path[0]) {
        br_close();
        return;
    }
    if (s)
        *s = 0;
    else
        br.path[0] = 0;
    br.sel = 0;
    br_list();
}
static int br_event(const vibe_event *e)         /* 1: the list took it */
{
    if (!br.open)
        return 0;
    if (e->type == VIBE_EV_ENCODER && e->id == VE_DATA) {
        static float acc;
        int n;
        acc += e->value, n = (int)acc, acc -= (float)n;
        br.sel = br.n ? (br.sel + n + br.n * 8) % br.n : 0;
        return 1;
    }
    if (e->type == VIBE_EV_BUTTON && e->down) {
        if (e->id == VB_ENTER || e->id == VB_DATA_PUSH)
            br_enter();
        else if (e->id == VB_RETURN || e->id == VB_F6)
            e->id == VB_F6 ? br_close() : br_up();
        else
            return 0;
        return 1;
    }
    return 0;
}
static void br_draw(void)
{
    int i, top = br.sel > 8 ? br.sel - 8 : 0;
    if (br.pending)
        br_list();
    vg_rect(20, 20, 440, 250, 0x000000);
    vg_frame(20, 20, 440, 250, 2, 0xFFFFFF);
    vg_text(br.path[0] ? br.path : "LOAD A WAV INTO CHOP", 32, 28, VG_BOLD, 0xFFFFFF, VG_LEFT);
    if (!br.n)
        vg_text(br.pending ? "..." : "NOTHING HERE", 32, 60, VG_MED, 0x8A8A8A, VG_LEFT);
    for (i = top; i < br.n && i < top + 10; i++) {
        int y = 52 + (i - top) * 21;
        if (i == br.sel)
            vg_rect(24, y - 1, 432, 20, 0xFFFFFF);
        vg_text(br.name[i], 32, y + 1, VG_MED, i == br.sel ? 0x000000 : 0xFFFFFF, VG_LEFT);
        if (br.dir[i])
            vg_text(">", 448, y + 1, VG_MED, i == br.sel ? 0x000000 : 0x8A8A8A, VG_RIGHT);
    }
    vg_text("DATA CHOOSE  ENTER OPEN  RETURN BACK", 240, 250, VG_SMALL, 0x8A8A8A, VG_CENTER);
}
