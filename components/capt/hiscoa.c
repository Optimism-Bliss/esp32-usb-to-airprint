// Hi-SCoA band compression (see hiscoa.h and docs/capt.md).
//
// Bit stream, MSB first. Commands, opcodes in unary:
//   0          LONGREP0 n      copy n bytes from line_size + L0 back
//   10 iiii    REPBYTE         stash byte 15 - i again (moves to the front)
//   110 1 b8   BYTE            literal byte (pushed onto the stash)
//   110 0 n    LONGREP2 n      copy from line_size + L2 back, then swap L0/L2
//   1110 n     LONGREP3 n      copy from L3 back
//   11110 n    LONGREP4 n      copy from L4 back
//   111110 n   LONGREP5 n      copy from L5 back, then swap L3/L5
//   1111110 1  ZEROBYTE        literal 0
//   1111110 0  PREFIX oo v     adds 128 * ((2 << oo) - 1 - v) (oo bits of v) to the next copy
//   11111110 ee END            ee: 0 end of band, 1 end of page
//   11111111   NOP (padding)
// Copy lengths 0..127: 0 = 111111, 1 = 00, 2 = 011, 3 = 010, else k ones,
// a 0, then k + 1 bits of v: n = (4 << k) - 1 - v.
// The stash holds the last 16 literal bytes, most recent first.
#include "hiscoa.h"

#include <string.h>

const hiscoa_params_t hiscoa_params_default = {.l0 = 0, .l2 = -7, .l3 = 1, .l5 = 4, .l4 = 0};

#define XOR_KEY 0x43
#define MAX_COPY (512 + 127)    // longest copy we emit (what captdriver emits; the format allows 1023)

typedef struct {
    uint8_t *out;
    size_t max, len;
    uint32_t acc;               // pending bits, right-aligned
    int nacc;
    bool overflow;
} bitw_t;

static inline void put(bitw_t *w, uint32_t bits, int n)
{
    // n <= 24, so acc never holds more than 31 bits
    w->acc = (w->acc << n) | (bits & ((1u << n) - 1));
    w->nacc += n;
    while (w->nacc >= 8) {
        w->nacc -= 8;
        if (w->len < w->max)
            w->out[w->len++] = (uint8_t)(w->acc >> w->nacc) ^ XOR_KEY;
        else
            w->overflow = true;
    }
}

static inline int bit_length(unsigned v)
{
    return v ? 32 - __builtin_clz(v) : 0;
}

// the length of a copy: 0..127
static void put_len(bitw_t *w, unsigned n)
{
    switch (n) {
    case 0: put(w, 0x3f, 6); return;
    case 1: put(w, 0x0, 2); return;
    case 2: put(w, 0x3, 3); return;
    case 3: put(w, 0x2, 3); return;
    }
    int k = bit_length(n) - 1;          // 2..6 value bits (order k - 1)
    put(w, (1u << k) - 2, k);           // k - 1 ones and a zero
    put(w, ~n, k);
}

static void put_copy(bitw_t *w, int cmd, unsigned n)
{
    if (n > 127) {
        unsigned v = n / 128;           // 1..7
        int k = bit_length(v) - 1;
        put(w, 0xfc, 8);
        put(w, (unsigned)k, 2);
        if (k)
            put(w, ~v, k);
        n %= 128;
    }
    put(w, 0xfffffffe, cmd + 1);        // cmd ones and a zero (LONGREP0 = "0")
    if (cmd == 2)
        put(w, 0, 1);
    put_len(w, n);
}

// bytes equal to the ones `d` back from pos, up to the end of the line
static inline unsigned match_len(const uint8_t *b, size_t pos, size_t n, size_t d, unsigned line_size)
{
    size_t p = pos, lim = pos + MAX_COPY;
    size_t eol = (pos / line_size + 1) * line_size;
    if (lim > eol)
        lim = eol;
    if (lim > n)
        lim = n;
    const uint8_t *s = b - d;
    while (p < lim && b[p] == s[p])
        p++;
    return (unsigned)(p - pos);
}

size_t hiscoa_encode(const uint8_t *band, unsigned line_size, unsigned lines, int eob,
                     const hiscoa_params_t *prm, uint8_t *out, size_t out_max)
{
    bitw_t w = {.out = out, .max = out_max};
    size_t n = (size_t)line_size * lines;
    // copy distances by command number (1 is REPBYTE: no distance)
    long dist[6] = {line_size + prm->l0, 0, line_size + prm->l2, prm->l3, prm->l4, prm->l5};
    uint8_t stash[16];
    unsigned nstash = 0;

    size_t pos = 0;
    while (pos < n) {
        unsigned best = 0;
        int cmd = -1;
        for (int c = 0; c < 6; c++) {
            if (dist[c] <= 0 || pos < (size_t)dist[c])
                continue;
            unsigned len = match_len(band, pos, n, (size_t)dist[c], line_size);
            if (len > best) {
                best = len;
                cmd = c;
            }
        }
        if (best > 1) {
            put_copy(&w, cmd, best);
            if (cmd == 2) {
                long t = dist[0];
                dist[0] = dist[2];
                dist[2] = t;
            } else if (cmd == 5) {
                long t = dist[3];
                dist[3] = dist[5];
                dist[5] = t;
            }
            pos += best;
            continue;
        }
        uint8_t b = band[pos++];
        unsigned i;
        for (i = 0; i < nstash && stash[i] != b; i++)
            ;
        if (i < nstash) {
            memmove(stash + 1, stash, i);
            stash[0] = b;
            put(&w, 0x20 | (15 - i), 6);
            continue;
        }
        if (nstash < 16)
            nstash++;
        memmove(stash + 1, stash, nstash - 1);
        stash[0] = b;
        if (b == 0)
            put(&w, 0xfd, 8);
        else
            put(&w, 0xd00 | b, 12);
    }
    put(&w, 0xfe, 8);
    put(&w, (unsigned)eob & 3, 2);
    // padding with 1s to a 32-bit boundary; a whole word when already
    // aligned, as captdriver does (and its users' printers accept)
    int bits = (int)((w.len * 8 + w.nacc) % 32);
    int pad = 32 - bits;
    while (pad > 0) {
        int k = pad > 16 ? 16 : pad;
        put(&w, 0xffff, k);
        pad -= k;
    }
    return w.overflow ? 0 : w.len;
}

void hiscoa_params_bytes(const hiscoa_params_t *p, uint8_t out[8])
{
    out[0] = (uint8_t)p->l3;
    out[1] = (uint8_t)p->l5;
    out[2] = 0x01;
    out[3] = 0x01;                      // 1 bit per pixel
    out[4] = (uint8_t)p->l0;
    out[5] = (uint8_t)p->l2;
    out[6] = (uint8_t)((uint16_t)p->l4 & 0xff);
    out[7] = (uint8_t)((uint16_t)p->l4 >> 8);
}

// ---------------------------------------------------------------------------
// decoder

typedef struct {
    const uint8_t *in;
    size_t len;
    size_t pos;                         // bit position
    bool past_end;
} bitr_t;

static inline int get1(bitr_t *r)
{
    if (r->pos / 8 >= r->len) {
        r->past_end = true;
        return 1;
    }
    int b = ((r->in[r->pos / 8] ^ XOR_KEY) >> (7 - r->pos % 8)) & 1;
    r->pos++;
    return b;
}

static unsigned getn(bitr_t *r, int n)
{
    unsigned v = 0;
    while (n-- > 0)
        v = (v << 1) | (unsigned)get1(r);
    return v;
}

static unsigned ones(bitr_t *r, unsigned limit)
{
    unsigned k = 0;
    while (k < limit && get1(r))
        k++;
    return k;
}

static unsigned get_len(bitr_t *r)
{
    unsigned k = ones(r, 6);
    if (k == 6)
        return 0;
    if (k == 0) {
        if (!get1(r))
            return 1;
        return get1(r) ? 2 : 3;
    }
    return (4u << k) - 1 - getn(r, (int)k + 1);
}

int hiscoa_decode(const uint8_t *in, size_t in_len, size_t *in_used, unsigned line_size,
                  const hiscoa_params_t *prm, uint8_t *out, size_t out_max, size_t *out_len)
{
    bitr_t r = {.in = in, .len = in_len};
    long dist[6] = {line_size + prm->l0, 0, line_size + prm->l2, prm->l3, prm->l4, prm->l5};
    uint8_t stash[16];
    unsigned nstash = 0, prefix = 0;
    size_t o = 0;
    int eob = -1;
    while (eob < 0 && !r.past_end) {
        unsigned op = ones(&r, 8);
        unsigned pending = prefix;
        prefix = 0;
        int cmd = -1;
        switch (op) {
        case 1: {                       // REPBYTE
            unsigned i = 15 - getn(&r, 4);
            if (i >= nstash)
                goto fail;
            uint8_t b = stash[i];
            memmove(stash + 1, stash, i);
            stash[0] = b;
            if (o >= out_max)
                goto fail;
            out[o++] = b;
            continue;
        }
        case 2:
            if (!get1(&r)) {
                cmd = 2;
                break;
            }
            /* fall through */          // BYTE
        case 6:
            if (op == 6 && !get1(&r)) { // PREFIX
                unsigned k = getn(&r, 2);
                unsigned v = getn(&r, (int)k);
                prefix = 128 * ((2u << k) - 1 - v);
                continue;
            } else {
                uint8_t b = op == 6 ? 0 : (uint8_t)getn(&r, 8);
                if (nstash < 16)
                    nstash++;
                memmove(stash + 1, stash, nstash - 1);
                stash[0] = b;
                if (o >= out_max)
                    goto fail;
                out[o++] = b;
                continue;
            }
        case 0: cmd = 0; break;
        case 3: cmd = 3; break;
        case 4: cmd = 4; break;
        case 5: cmd = 5; break;
        case 7:
            eob = (int)getn(&r, 2);
            continue;
        default:                        // 8 ones: NOP
            continue;
        }
        unsigned n = pending + get_len(&r);
        long d = dist[cmd];
        if (d <= 0 || (size_t)d > o || o + n > out_max)
            goto fail;
        for (unsigned i = 0; i < n; i++, o++)
            out[o] = out[o - d];
        if (cmd == 2) {
            long t = dist[0];
            dist[0] = dist[2];
            dist[2] = t;
        } else if (cmd == 5) {
            long t = dist[3];
            dist[3] = dist[5];
            dist[5] = t;
        }
    }
    if (eob < 0 || r.past_end)
        goto fail;
    // skip the padding up to the next 32-bit boundary
    r.pos = (r.pos + 31) / 32 * 32;
    if (r.pos / 8 > in_len)
        goto fail;
    *in_used = r.pos / 8;
    *out_len = o;
    return eob;
fail:
    *in_used = r.pos / 8 < in_len ? r.pos / 8 : in_len;
    *out_len = o;
    return -1;
}
