/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 PolarPatch */
/* CHOP: a recording cut into slices over the keys, each slice pitched (varispeed, as a sampler does: higher is
 * shorter). SLICES: each key its slice (round the slices), PITCH over all; PITCHED: one slice (SLICE) played
 * chromatically, middle C as recorded. A slice plays while its key is held, up to LENGTH of it.
 * The sample: app/tools/again_chop.py's "AGC1" file (22050 Hz mono, IMA ADPCM in blocks of 256 with each block's
 * decoder state, so a slice starts anywhere: its block found, decoded up to it), sent over USB as the companion's
 * file "chop" (app.c), or the factory sample in the firmware. The IMA ADPCM decoder is the standard one (the IMA's
 * published tables). ch_post from the main loop, ch_render in the interrupt. Part of app.c. */

#define CH_NV 4u
#define CH_RATE 22050u
#define CH_BLOCK 256u
#define CH_BLOCK_BYTES 132u
#define CH_HEAD 160u
#define CH_MAX_SLICES 32u
enum { CM_SLICES, CM_PITCHED, NCM };
static const char *const CM_NAME[NCM] = {"SLICES", "PITCHED"};

static const int16_t IMA_STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
    118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767};
static const int8_t IMA_INDEX[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

/* the sample playing (the main loop swaps it with every voice off) */
typedef struct {
    const uint8_t *blocks;                       /* 0: none */
    uint32_t n, nslices;
    uint32_t slice[CH_MAX_SLICES + 1u];          /* starts, and the end after the last */
    char name[16];
} ch_sample_t;

typedef struct {                                 /* a decoder: the next sample's index, its state */
    uint32_t at;
    int32_t pred, idx;
} adp_t;

typedef struct {
    int8_t note;
    uint8_t held, rev;                           /* rev: the slice backwards, from its end */
    adp_t d;
    float s0, s1, pos, inc, amp;
    uint32_t end, t, slice;
    uint32_t rp, lim, cblk;                      /* backwards: the next sample, where it stops, the block decoded */
    int16_t cache[CH_BLOCK];                     /* .. into here (ADPCM decodes forwards only: a block at a time) */
} ch_voice_t;

static struct {
    ch_sample_t s;
    ch_voice_t v[CH_NV];
    uint32_t next;
    uint32_t mode, sel;                          /* MODE, SLICE */
    volatile uint32_t rev;                       /* the slices played backwards (a bit each: REVERSE) */
    float length, pitch, level;                  /* LENGTH 0.05..1, PITCH semitones, the level */
    volatile uint32_t last_slice;                /* the slice played last (the screen) */
    volatile uint32_t qw, qr;
    uint32_t q[16];
} ch = {.length = 1.0f, .level = 0.7f};

static inline int32_t adp_next(const uint8_t *blocks, adp_t *d)
{
    uint32_t i = d->at & (CH_BLOCK - 1u);
    const uint8_t *b = blocks + (d->at >> 8) * CH_BLOCK_BYTES;
    int32_t code, step, diff;
    if (!i) {                                    /* a block's start: its state */
        d->pred = (int16_t)(b[0] | b[1] << 8);
        d->idx = b[2] > 88u ? 88 : b[2];
    }
    code = (b[4u + (i >> 1)] >> ((i & 1u) * 4u)) & 15;
    step = IMA_STEP[d->idx];
    diff = step >> 3;
    if (code & 4)
        diff += step;
    if (code & 2)
        diff += step >> 1;
    if (code & 1)
        diff += step >> 2;
    d->pred = clamp(code & 8 ? d->pred - diff : d->pred + diff, -32768, 32767);
    d->idx = clamp(d->idx + IMA_INDEX[code], 0, 88);
    d->at++;
    return d->pred;
}

/* backwards: sample k of the sample, through v's block cache */
static int32_t ch_back(ch_voice_t *v, uint32_t k)
{
    uint32_t b = k >> 8, i;
    if (b != v->cblk) {
        adp_t d;
        d.at = b << 8;
        for (i = 0; i < CH_BLOCK && d.at < ch.s.n; i++)
            v->cache[i] = (int16_t)adp_next(ch.s.blocks, &d);
        v->cblk = b;
    }
    return v->cache[k & (CH_BLOCK - 1u)];
}
/* v's next sample, -1 .. 1 (0 past its end) */
static float ch_fetch(ch_voice_t *v)
{
    if (v->rev)
        return v->rp > v->lim ? (float)ch_back(v, --v->rp) * (1.0f / 32768.0f) : 0.0f;
    return v->d.at < ch.s.n ? (float)adp_next(ch.s.blocks, &v->d) * (1.0f / 32768.0f) : 0.0f;
}

/* the decoder at sample s: its block's start, then decoded up to s */
static void adp_seek(const uint8_t *blocks, adp_t *d, uint32_t s)
{
    d->at = s & ~(CH_BLOCK - 1u);
    while (d->at < s)
        adp_next(blocks, d);
}

/* an AGC1 file -> s (0: it is not one) */
static int ch_parse(const uint8_t *f, uint32_t size, ch_sample_t *s)
{
    uint32_t i, n, nb, ns, ok;
    if (!f || size < CH_HEAD || f[0] != 'A' || f[1] != 'G' || f[2] != 'C' || f[3] != '1')
        return 0;
    n = f[8] | f[9] << 8 | f[10] << 16 | (uint32_t)f[11] << 24;
    nb = f[12] | f[13] << 8;
    ns = f[14];
    if (!n || nb * CH_BLOCK < n || CH_HEAD + nb * CH_BLOCK_BYTES > size)
        return 0;
    s->blocks = f + CH_HEAD;
    s->n = n;
    ok = ns >= 1u && ns <= CH_MAX_SLICES;
    for (i = 0; ok && i < ns; i++) {
        const uint8_t *p = f + 16u + 4u * i;
        s->slice[i] = (p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24);
        ok = s->slice[i] < n && (!i || s->slice[i] > s->slice[i - 1]);
    }
    if (!ok) {                                   /* no slices given (or wrong): sixteen even ones */
        ns = 16;
        for (i = 0; i < ns; i++)
            s->slice[i] = n * i / ns;
    }
    s->nslices = ns;
    s->slice[ns] = n;
    for (i = 0; i < 15u; i++)
        s->name[i] = (char)f[144u + i];
    s->name[15] = 0;
    return 1;
}

static void ch_post(int32_t note, uint32_t vel, int on)
{
    uint32_t w = ch.qw;
    if (w - ch.qr >= 16u)
        return;
    ch.q[w & 15u] = (uint32_t)(note & 127) | (on ? 0u : 0x80u) | (vel & 127u) << 8;
    ch.qw = w + 1u;
}

static void ch_start(int32_t note, float vel)
{
    ch_voice_t *v;
    uint32_t sl, a, b;
    float semis;
    if (!ch.s.blocks)
        return;
    if (ch.mode == CM_PITCHED) {
        sl = ch.sel % ch.s.nslices;
        semis = (float)(note - 60) + ch.pitch;
    } else {
        sl = (uint32_t)(((note - 53) % (int32_t)ch.s.nslices + (int32_t)ch.s.nslices) % (int32_t)ch.s.nslices);
        semis = ch.pitch;
    }
    a = ch.s.slice[sl];
    b = ch.s.slice[sl + 1u];
    v = &ch.v[ch.next];
    ch.next = (ch.next + 1u) % CH_NV;
    v->note = (int8_t)note;
    v->held = 1;
    v->slice = sl;
    v->end = a + (uint32_t)((float)(b - a) * ch.length);
    if (v->end < a + 64u)
        v->end = a + 64u;
    if (v->end > ch.s.n - 1u)
        v->end = ch.s.n - 1u;
    v->rev = (uint8_t)(ch.rev >> sl & 1u);
    if (v->rev) {                                /* from the slice's end back, LENGTH of it */
        v->rp = b;
        v->lim = b - (v->end - a);
        v->cblk = 0xFFFFFFFFu;
    } else {
        adp_seek(ch.s.blocks, &v->d, a);
    }
    v->s0 = ch_fetch(v);
    v->s1 = ch_fetch(v);
    v->pos = 0.0f;
    v->inc = (float)CH_RATE / SR * fm_exp2f(semis / 12.0f);
    v->amp = 0.4f + 0.6f * vel;
    v->t = 0;
    ch.last_slice = sl;
}

/* n frames into l, r and send */
static void ch_render(float *l, float *r, float *send, uint32_t n)
{
    uint32_t i, j;
    while (ch.qr != ch.qw) {
        uint32_t e = ch.q[ch.qr & 15u];
        ch.qr++;
        if (e & 0x80u) {
            for (j = 0; j < CH_NV; j++)
                if (ch.v[j].note == (int8_t)(e & 127u))
                    ch.v[j].held = 0;
        } else
            ch_start((int32_t)(e & 127u), (float)((e >> 8) & 127u) / 127.0f);
    }
    if (!ch.s.blocks)
        return;
    for (j = 0; j < CH_NV; j++) {
        ch_voice_t *v = &ch.v[j];
        float env = 1.0f;
        if (v->note < 0)
            continue;
        for (i = 0; i < n; i++) {
            float s;
            int open = (v->rev ? v->rp > v->lim : v->d.at <= v->end) && (v->held || v->t < (uint32_t)(0.03f * SR));
            while (v->pos >= 1.0f) {             /* on to the next pair of samples */
                v->s0 = v->s1;
                v->s1 = ch_fetch(v);
                v->pos -= 1.0f;
            }
            s = v->s0 + (v->s1 - v->s0) * v->pos;
            v->pos += v->inc;
            env = v->t < 132u ? (float)v->t / 132.0f : 1.0f;   /* 3 ms in */
            if (!open)
                v->amp *= 0.995f;                /* ~5 ms out */
            s *= env * v->amp * ch.level;
            l[i] += s;
            r[i] += s;
            send[i] += s * 0.4f;
            v->t++;
            if (!open && v->amp < 1e-4f) {
                v->note = -1;
                break;
            }
        }
    }
}
