/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 PolarPatch */
/* The screensaver's film, apart from any screen: its frames decoded one after the other into sv_fb (240 x 240, a bit
 * a pixel, the most significant bit the leftmost pixel, 1 white). Each frame is a raw DEFLATE stream of ops that
 * rebuild it over the frame before (the format: tools/again_saver.py); the inflater is written here from RFC 1951
 * (canonical Huffman codes decoded a bit at a time). The film itself is the USB002 tour's creative drop's footage
 * (app/media: not GPL, see LICENSING.md). Shared by the FM-1 (saver.h) and FM-1 Again VIBE. Needs stdint, string. */

#define SV_IDLE_MS 60000u                        /* untouched this long: the film */
#define SV_FB 7200u                              /* a frame: 240 x 240, a bit a pixel */
#define SV_OPS 4096u                             /* a frame's ops at the most (again_saver.py checks) */
#define SV_BACK 240u                             /* UP: 8 rows back */

static uint8_t sv_fb[SV_FB], sv_ops[SV_OPS];
static struct {
    const uint8_t *p;                            /* the film (0: none) */
    uint32_t len, nf, ms;                        /* its bytes, frames, ms a frame */
    uint32_t at, t;                              /* the frame shown, when */
    uint32_t touch;                              /* the last touch */
    uint8_t on;
} sv;

/* ---- inflate (RFC 1951) */
typedef struct {
    const uint8_t *in;
    uint32_t n, pos, bb, bc;                     /* the input, read up to pos; the bit buffer and its bits */
    uint8_t *out;
    uint32_t on, op;                             /* the output's room, written up to op */
    int err;
} inf_t;
typedef struct {
    uint16_t count[16];                          /* the codes of each length */
    uint16_t sym[288];                           /* the symbols, in canonical order */
} huff_t;
static huff_t inf_lc, inf_dc, inf_fl, inf_fd;    /* (inf_fl, inf_fd: the fixed codes, made once) */

static uint32_t inf_bits(inf_t *s, uint32_t need)
{
    uint32_t v = s->bb;
    while (s->bc < need) {
        if (s->pos >= s->n) {
            s->err = 1;
            return 0;
        }
        v |= (uint32_t)s->in[s->pos++] << s->bc;
        s->bc += 8u;
    }
    s->bb = v >> need;
    s->bc -= need;
    return v & ((1u << need) - 1u);
}

/* the code of lengths len[0 .. n-1]: < 0 over-subscribed, else 0 complete or > 0 incomplete */
static int huff_build(huff_t *h, const uint8_t *len, uint32_t n)
{
    uint16_t offs[16];
    uint32_t i;
    int left = 1;
    memset(h->count, 0, sizeof h->count);
    for (i = 0; i < n; i++)
        h->count[len[i]]++;
    if (h->count[0] == n)
        return 0;
    for (i = 1; i < 16u; i++) {
        left <<= 1;
        left -= h->count[i];
        if (left < 0)
            return left;
    }
    offs[1] = 0;
    for (i = 1; i < 15u; i++)
        offs[i + 1u] = (uint16_t)(offs[i] + h->count[i]);
    for (i = 0; i < n; i++)
        if (len[i])
            h->sym[offs[len[i]]++] = (uint16_t)i;
    return left;
}

static int huff_dec(inf_t *s, const huff_t *h)
{
    int code = 0, first = 0, index = 0, len, count;
    for (len = 1; len < 16; len++) {
        code |= (int)inf_bits(s, 1);
        count = h->count[len];
        if (code - count < first)
            return h->sym[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

static const uint16_t INF_LBASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83,
                                       99, 115, 131, 163, 195, 227, 258};
static const uint8_t INF_LEXT[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t INF_DBASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                       1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t INF_DEXT[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12,
                                     12, 13, 13};

static int inf_codes(inf_t *s, const huff_t *lc, const huff_t *dc)
{
    for (;;) {
        int sym = huff_dec(s, lc), d;
        uint32_t len, dist;
        if (sym < 0 || s->err)
            return -1;
        if (sym < 256) {
            if (s->op >= s->on)
                return -1;
            s->out[s->op++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29)
                return -1;
            len = INF_LBASE[sym] + inf_bits(s, INF_LEXT[sym]);
            d = huff_dec(s, dc);
            if (d < 0 || d >= 30)
                return -1;
            dist = INF_DBASE[d] + inf_bits(s, INF_DEXT[d]);
            if (s->err || dist > s->op || len > s->on - s->op)
                return -1;
            while (len--) {
                s->out[s->op] = s->out[s->op - dist];
                s->op++;
            }
        }
    }
}

static int inf_dynamic(inf_t *s)
{
    static const uint8_t ORDER[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    uint8_t len[320];
    uint32_t nlen = inf_bits(s, 5) + 257u, ndist = inf_bits(s, 5) + 1u, ncode = inf_bits(s, 4) + 4u, i;
    if (s->err || nlen > 286u || ndist > 30u)
        return -1;
    memset(len, 0, 19);
    for (i = 0; i < ncode; i++)
        len[ORDER[i]] = (uint8_t)inf_bits(s, 3);
    if (huff_build(&inf_lc, len, 19) != 0)       /* (the code lengths' code: complete) */
        return -1;
    for (i = 0; i < nlen + ndist;) {
        int sym = huff_dec(s, &inf_lc);
        uint32_t rep, val = 0;
        if (sym < 0 || s->err)
            return -1;
        if (sym < 16) {
            len[i++] = (uint8_t)sym;
            continue;
        }
        if (sym == 16) {
            if (!i)
                return -1;
            val = len[i - 1u];
            rep = 3u + inf_bits(s, 2);
        } else if (sym == 17) {
            rep = 3u + inf_bits(s, 3);
        } else {
            rep = 11u + inf_bits(s, 7);
        }
        if (i + rep > nlen + ndist)
            return -1;
        while (rep--)
            len[i++] = (uint8_t)val;
    }
    if (!len[256] || huff_build(&inf_lc, len, nlen) < 0 || huff_build(&inf_dc, len + nlen, ndist) < 0)
        return -1;
    return inf_codes(s, &inf_lc, &inf_dc);
}

/* in[0 .. n-1], a raw DEFLATE stream, into out (room bytes): its length, or -1 */
static int32_t inflate_raw(const uint8_t *in, uint32_t n, uint8_t *out, uint32_t room)
{
    static uint8_t fixed;
    inf_t s;
    uint32_t last;
    if (!fixed) {
        uint8_t len[288];
        uint32_t i;
        for (i = 0; i < 288u; i++)
            len[i] = i < 144u ? 8u : i < 256u ? 9u : i < 280u ? 7u : 8u;
        huff_build(&inf_fl, len, 288);
        for (i = 0; i < 30u; i++)
            len[i] = 5u;
        huff_build(&inf_fd, len, 30);
        fixed = 1;
    }
    memset(&s, 0, sizeof s);
    s.in = in, s.n = n, s.out = out, s.on = room;
    do {
        uint32_t type;
        int rc;
        last = inf_bits(&s, 1);
        type = inf_bits(&s, 2);
        if (s.err)
            return -1;
        if (type == 0) {                         /* stored */
            uint32_t k;
            s.bb = s.bc = 0;
            if (s.pos + 4u > s.n)
                return -1;
            k = s.in[s.pos] | (uint32_t)s.in[s.pos + 1u] << 8;
            if ((k ^ 0xFFFFu) != (s.in[s.pos + 2u] | (uint32_t)s.in[s.pos + 3u] << 8))
                return -1;
            s.pos += 4u;
            if (k > s.n - s.pos || k > s.on - s.op)
                return -1;
            memcpy(s.out + s.op, s.in + s.pos, k);
            s.op += k, s.pos += k;
            rc = 0;
        } else if (type == 1) {
            rc = inf_codes(&s, &inf_fl, &inf_fd);
        } else if (type == 2) {
            rc = inf_dynamic(&s);
        } else {
            return -1;
        }
        if (rc)
            return -1;
    } while (!last);
    return (int32_t)s.op;
}

/* ---- the film */
static uint32_t sv_u32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* the film at p, len bytes ("AGS1": tools/again_saver.py); a bad one (or none): no film, sv.p 0 */
static void sv_film(const uint8_t *p, uint32_t len)
{
    sv.p = 0;
    if (!p || len < 8u || memcmp(p, "AGS1", 4))
        return;
    sv.nf = p[4] | (uint32_t)p[5] << 8;
    sv.ms = p[6] | (uint32_t)p[7] << 8;
    if (!sv.nf || !sv.ms || 8u + 8u * sv.nf + 4u > len || sv_u32(p + 8u + 4u * sv.nf) > len)
        return;
    sv.p = p, sv.len = len;
}

/* frame f over the one in sv_fb (frame 0: over black): 0, or -1 (the film is not as it should be) */
static int sv_decode(uint32_t f)
{
    uint32_t a = sv_u32(sv.p + 8u + 4u * f), b = sv_u32(sv.p + 12u + 4u * f), i = 0, k = 0;
    int32_t n;
    if (!f)
        memset(sv_fb, 0, SV_FB);
    if (a > b || b > sv.len || (n = inflate_raw(sv.p + a, b - a, sv_ops, SV_OPS)) < 0)
        return -1;
    while (k < (uint32_t)n) {
        uint32_t op = sv_ops[k] >> 6, c = (sv_ops[k] & 63u) + 1u;
        k++;
        if (c > SV_FB - i || (op == 1u && i < SV_BACK) || (op == 2u && !i) || (op == 3u && c > (uint32_t)n - k))
            return -1;
        if (op == 1u)
            memcpy(sv_fb + i, sv_fb + i - SV_BACK, c);   /* (c <= 64 < SV_BACK: no overlap) */
        else if (op == 2u)
            memset(sv_fb + i, sv_fb[i - 1u], c);
        else if (op == 3u)
            memcpy(sv_fb + i, sv_ops + k, c), k += c;
        i += c;
    }
    return i == SV_FB ? 0 : -1;
}

