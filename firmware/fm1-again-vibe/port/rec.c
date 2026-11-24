/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 PolarPatch */
/* FM-1 Again VIBE: sound in, straight into CHOP. REC starts recording IN 1 (a voice, a guitar, a room), REC again (or
 * a minute) stops it; the recording is cut where its sound starts again (its onsets, as app/tools/again_chop.py and
 * the editor page cut), coded as FM-1 Again's sample file ("AGC1": 22050 Hz, IMA ADPCM in blocks of 256) and played by
 * CHOP at once: the keys and the pads play its slices. The host keeps it ("chop.agc": in the plugin, in the project).
 *
 * The sound flow records (rec_audio: the input taken down to 22050 Hz, each output sample the mean of the input's since
 * the last); the control flow codes it a piece a frame (rec_poll, from vibe_on_draw) and hands CHOP the new sample as
 * FM-1 Again's main loop does (ch_next, ch_swap). Part of main.c. */

#define REC_RATE 22050u
#define REC_MAX (60u * REC_RATE)                 /* a minute */
#define REC_BLOCKS ((REC_MAX + CH_BLOCK - 1u) / CH_BLOCK)
static int16_t rec_buf[REC_MAX];
static uint8_t rec_agc[CH_HEAD + REC_BLOCKS * CH_BLOCK_BYTES];
static volatile uint32_t rec_on;                 /* (the sound flow's: recording) */
static volatile uint32_t rec_n;                  /* (the sound flow's: samples recorded) */
static volatile uint32_t rec_done;               /* (the sound flow's: a recording ended, its length + 1) */
static float rec_acc, rec_sum, rec_peak;
static uint32_t rec_cnt;
static float rec_step = REC_RATE / 44100.0f;     /* output samples an input sample */
static float rec_mon = 0.5f;                     /* what is recorded, heard (half) */

/* (the sound flow) a block of IN 1 */
static void rec_audio(const float *in, float *out_l, float *out_r, int frames)
{
    int k;
    if (!rec_on || !in)
        return;
    for (k = 0; k < frames; k++) {
        float x = in[k];
        out_l[k] += x * rec_mon;
        out_r[k] += x * rec_mon;
        rec_sum += x, rec_cnt++;
        rec_peak = fm_fabsf(x) > rec_peak ? fm_fabsf(x) : rec_peak;
        rec_acc += rec_step;
        if (rec_acc >= 1.0f) {
            float m = rec_sum / (float)rec_cnt;
            rec_acc -= 1.0f, rec_sum = 0.0f, rec_cnt = 0;
            if (rec_n < REC_MAX)
                rec_buf[rec_n++] = (int16_t)clamp((int32_t)(m * 32767.0f), -32768, 32767);
            else {
                rec_on = 0;
                rec_done = rec_n + 1u;
            }
        }
    }
}
static void rec_start(void)                      /* (the sound flow) */
{
    rec_n = 0, rec_acc = rec_sum = rec_peak = 0.0f, rec_cnt = 0, rec_done = 0;
    rec_on = 1;
}
static void rec_stop(void)                       /* (the sound flow) */
{
    if (!rec_on)
        return;
    rec_on = 0;
    rec_done = rec_n + 1u;
}

/* ---- (the control flow) the recording coded, cut and handed to CHOP */
static const int16_t ENC_STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
    118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767};
static const int8_t ENC_INDEX[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
static struct {
    uint32_t n, at, nb;                          /* the samples; coded up to at; the blocks */
    int32_t pred, idx;
    float gain;                                  /* to -1 dB at the peak */
    uint8_t busy;
} enc;

static int16_t rec_sample(uint32_t i)            /* the recording, brought up to -1 dB */
{
    return (int16_t)clamp((int32_t)((float)rec_buf[i] * enc.gain), -32768, 32767);
}
static void enc_block(uint32_t b)                /* IMA ADPCM: each block its state, then its nibbles (again_chop.py) */
{
    uint8_t *o = rec_agc + CH_HEAD + b * CH_BLOCK_BYTES;
    uint32_t i, lo = 0;
    o[0] = (uint8_t)(enc.pred & 255), o[1] = (uint8_t)((enc.pred >> 8) & 255), o[2] = (uint8_t)enc.idx, o[3] = 0;
    for (i = 0; i < CH_BLOCK; i++) {
        uint32_t j = b * CH_BLOCK + i;
        int32_t v = j < enc.n ? rec_sample(j) : 0, step = ENC_STEP[enc.idx], d = v - enc.pred, code = d < 0 ? 8 : 0, diff;
        d = d < 0 ? -d : d;
        diff = step >> 3;
        if (d >= step) code |= 4, d -= step, diff += step;
        step >>= 1;
        if (d >= step) code |= 2, d -= step, diff += step;
        step >>= 1;
        if (d >= step) code |= 1, diff += step;
        enc.pred = clamp(code & 8 ? enc.pred - diff : enc.pred + diff, -32768, 32767);
        enc.idx = clamp(enc.idx + ENC_INDEX[code], 0, 88);
        if (i & 1u)
            o[4u + i / 2u] = (uint8_t)(lo | (uint32_t)code << 4);
        else
            lo = (uint32_t)code;
    }
}

/* where the sound starts again: the rise of its energy in 10 ms frames, the strongest, at least 150 ms apart (the
 * editor's and again_chop.py's onsets) -> up to `most` slice starts, sorted, the first 0 */
#define ON_F 220u
#define ON_MAX (REC_MAX / ON_F + 1u)
static float on_le[ON_MAX], on_fx[ON_MAX];
static uint32_t onsets(uint32_t *out, uint32_t most)
{
    uint32_t nf = enc.n / ON_F, i, j, k, n = 1;
    float top = -99.0f;
    out[0] = 0;
    for (i = 0; i < nf; i++) {
        float e = 1e-9f;
        for (j = 0; j < ON_F; j++) {
            float x = (float)rec_buf[i * ON_F + j] * (1.0f / 32768.0f);
            e += x * x;
        }
        on_le[i] = fm_log2f(e) * 0.30103f;
        top = on_le[i] > top ? on_le[i] : top;
    }
    for (i = 0; i < nf; i++)
        on_fx[i] = on_le[i] < top - 2.5f || !i ? 0.0f : fm_maxf(0.0f, on_le[i] - on_le[i - 1u]);
    while (n < most) {                           /* the strongest left, far enough from those taken */
        float best = 0.4f;
        uint32_t bi = 0;
        for (i = 1; i < nf; i++) {
            int ok = on_fx[i] >= best;
            for (k = 0; ok && k < n; k++)
                ok = (i * ON_F > out[k] ? i * ON_F - out[k] : out[k] - i * ON_F) >= REC_RATE * 15u / 100u;
            if (ok)
                best = on_fx[i], bi = i;
        }
        if (!bi)
            break;
        out[n++] = bi * ON_F;
        on_fx[bi] = 0.0f;
    }
    for (i = 1; i < n; i++)                      /* sorted */
        for (k = i; k > 0 && out[k - 1u] > out[k]; k--) {
            uint32_t t = out[k];
            out[k] = out[k - 1u], out[k - 1u] = t;
        }
    return n;
}

static uint32_t rec_shown_n;                     /* (the screen: seconds recorded so far) */
static void rec_finish(void)                     /* the head, the slices, CHOP takes it, the host keeps it */
{
    uint32_t sl[CH_MAX_SLICES], ns = onsets(sl, 16), i, size = CH_HEAD + enc.nb * CH_BLOCK_BYTES;
    ch_sample_t s;
    memset(rec_agc, 0, CH_HEAD);
    memcpy(rec_agc, "AGC1", 4);
    rec_agc[4] = REC_RATE & 255, rec_agc[5] = (REC_RATE >> 8) & 255;
    for (i = 0; i < 4u; i++)
        rec_agc[8u + i] = (uint8_t)(enc.n >> (8u * i));
    rec_agc[12] = (uint8_t)(enc.nb & 255), rec_agc[13] = (uint8_t)(enc.nb >> 8);
    rec_agc[14] = (uint8_t)ns;
    for (i = 0; i < ns; i++) {
        rec_agc[16u + 4u * i] = (uint8_t)sl[i], rec_agc[17u + 4u * i] = (uint8_t)(sl[i] >> 8);
        rec_agc[18u + 4u * i] = (uint8_t)(sl[i] >> 16), rec_agc[19u + 4u * i] = (uint8_t)(sl[i] >> 24);
    }
    memcpy(rec_agc + 144, "RECORDING", 9);
    if (!ch_parse(rec_agc, size, &s))
        return;
    ch_swap = 0;
    ch_next = s;
    VIBE_RELEASE();
    ch_swap = 1;                                 /* (the next block lets every voice of the old one go first) */
    set.crev = 0;
    q_put(Q_APPLY, 0, 0, 0);
    rec_to_chop();
    vibe_store_write("chop.agc", rec_agc, (int)size);
    {
        char t[24];
        t[0] = 0;
        num(t, (int32_t)ns);
        cat(t, ns == 1u ? " SLICE" : " SLICES");
        say("RECORDED", t);
    }
}

static void rec_poll(void)                       /* (from vibe_on_draw) a piece of the coding a frame */
{
    uint32_t d = rec_done, b, end;
    if (rec_on)
        rec_shown_n = rec_n;
    if (d && !enc.busy) {                        /* a recording ended: code it */
        rec_done = 0;
        if (d - 1u < REC_RATE / 4u) {            /* (under a quarter of a second: nothing) */
            say("REC", "TOO SHORT");
            return;
        }
        enc.n = d - 1u, enc.at = 0, enc.nb = (enc.n + CH_BLOCK - 1u) / CH_BLOCK, enc.pred = 0, enc.idx = 0;
        {
            int32_t pk = 1;
            uint32_t i;
            for (i = 0; i < enc.n; i++)
                pk = rec_buf[i] > pk ? rec_buf[i] : -rec_buf[i] > pk ? -rec_buf[i] : pk;
            enc.gain = 29205.0f / (float)pk;     /* (-1 dB) */
            enc.gain = enc.gain > 16.0f ? 16.0f : enc.gain;
        }
        enc.busy = 1;
    }
    if (!enc.busy)
        return;
    for (b = enc.at, end = enc.at + 200u; b < enc.nb && b < end; b++)   /* (200 blocks: 51 200 samples a frame) */
        enc_block(b);
    enc.at = b;
    if (enc.at >= enc.nb) {
        enc.busy = 0;
        rec_finish();
    }
}

static void rec_button(void)                     /* (the control flow) REC */
{
    if (rec_on) {
        q_put(Q_REC, 0, 0, 0);
        return;
    }
    vibe_request(VIBE_REQ_AUDIO_INPUT);           /* (IN 1 on: the host asks the player once) */
    q_put(Q_REC, 1, 0, 0);
    say("REC", "IN 1 INTO CHOP");
}

static void rec_load(void)                       /* (vibe_on_init) the recording the host kept */
{
    int n = vibe_store_read("chop.agc", rec_agc, (int)sizeof rec_agc);
    ch_sample_t s;
    if (n > (int)CH_HEAD && ch_parse(rec_agc, (uint32_t)n, &s)) {
        ch_next = s;
        ch.s = s;
        ch_swap = 0;
    }
}
