// Compares our Hi-SCoA encoder (components/capt/hiscoa.c) with captdriver's,
// whose output has printed on real LBP2900 and LBP3010 printers. Built by
// tools/captdriver_ref/run.sh against a captdriver checkout; captdriver is
// GPL and only used here, at test time, as a reference: nothing of it goes
// into the firmware.
//
// For many bands (text-like, halftone, noise, edge cases): both encoders must
// give the same bytes, and each decoder must give the band back from both.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hiscoa.h"

// captdriver's API (hiscoa-compress.h / tests/hiscoa-decompress.h)
#include "hiscoa-common.h"
#include "hiscoa-compress.h"
#include "hiscoa-decompress.h"

static uint32_t rng = 1;
static uint32_t rnd(void)
{
    rng = rng * 1103515245u + 12345u;
    return rng >> 8;
}

static void fill(uint8_t *b, unsigned ls, unsigned lines, int kind)
{
    size_t n = (size_t)ls * lines;
    memset(b, 0, n);
    switch (kind) {
    case 0: break;                                              // white
    case 1: memset(b, 0xff, n); break;                          // black
    case 2: for (size_t i = 0; i < n; i++) b[i] = (uint8_t)rnd(); break;   // noise
    case 3:                                                     // "text": runs of glyph-like bytes
        for (unsigned y = 0; y < lines; y++)
            for (unsigned x = 0; x < ls; x++)
                if ((x / 7 + y / 9) % 3 == 0 && (rnd() & 3))
                    b[y * ls + x] = (uint8_t)(0x18 << (y % 3)) | (uint8_t)(rnd() & 0x81);
        break;
    case 4:                                                     // halftone 8x8 pattern, varying density
        for (unsigned y = 0; y < lines; y++)
            for (unsigned x = 0; x < ls; x++)
                b[y * ls + x] = (uint8_t)((x * 8 / ls) > (y % 8) ? 0xaa >> (y & 1) : 0x11 << (y & 3));
        break;
    case 5:                                                     // sparse dots and long equal runs
        for (size_t i = 0; i < n; i++) b[i] = (rnd() % 97) == 0 ? (uint8_t)rnd() : (i % 1000 < 300 ? 0xff : 0);
        break;
    case 6:                                                     // few distinct bytes (stash hits)
        for (size_t i = 0; i < n; i++) { static const uint8_t v[5] = {0x01, 0x80, 0x3c, 0xe7, 0x55}; b[i] = v[rnd() % 5]; }
        break;
    }
}

int main(void)
{
    static const unsigned sizes[][2] = {{592, 70}, {595, 160}, {608, 70}, {592, 1}, {16, 3}, {600, 8}, {4, 2}};
    int fails = 0, cases = 0;
    for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++)
        for (int kind = 0; kind <= 6; kind++)
            for (int eob = 0; eob <= 1; eob++)
                for (int rep = 0; rep < 3; rep++) {
                    unsigned ls = sizes[s][0], lines = sizes[s][1];
                    size_t n = (size_t)ls * lines, cap = hiscoa_bound(n);
                    uint8_t *band = malloc(n), *a = malloc(cap), *b = malloc(cap + 64), *back = malloc(n + 16);
                    fill(band, ls, lines, kind);
                    size_t la = hiscoa_encode(band, ls, lines, eob, &hiscoa_params_default, a, cap);
                    memset(b, 0, cap + 64);
                    size_t lb = hiscoa_compress_band(b, cap + 64, band, ls, lines, eob, &hiscoa_default_params);
                    cases++;
                    if (la != lb || memcmp(a, b, la)) {
                        size_t i = 0;
                        while (i < la && i < lb && a[i] == b[i]) i++;
                        printf("DIFF ls=%u lines=%u kind=%d eob=%d: ours %zu bytes, captdriver %zu, first diff at %zu\n",
                               ls, lines, kind, eob, la, lb, i);
                        fails++;
                    }
                    // our decoder on captdriver's stream
                    size_t used, got;
                    hiscoa_params_t p = hiscoa_params_default;
                    int e = hiscoa_decode(b, lb, &used, ls, &p, back, n + 16, &got);
                    // an end marker that ends on a word boundary is followed by a whole word
                    // of padding (NOPs, 0xbc after the XOR) that the next band starts with
                    bool pad_ok = used == lb || (used + 4 == lb && !memcmp(b + used, "\xbc\xbc\xbc\xbc", 4));
                    if (e != eob || got != n || memcmp(back, band, n) || !pad_ok) {
                        printf("our decoder on captdriver's stream: eob %d got %zu/%zu used %zu/%zu\n", e, got, n, used, lb);
                        fails++;
                    }
                    // captdriver's decoder on our stream
                    const void *ptr = a;
                    size_t sz = la, osz = n + 16;
                    memset(back, 0, n + 16);
                    unsigned ce = hiscoa_decompress_band(&ptr, &sz, back, &osz, ls, &hiscoa_default_params);
                    if ((int)ce != eob || osz != n || memcmp(back, band, n)) {
                        printf("captdriver's decoder on our stream: eob %u got %zu/%zu\n", ce, osz, n);
                        fails++;
                    }
                    if (rep == 0 && s == 0)
                        printf("ls=%u lines=%u kind=%d eob=%d: %zu -> %zu bytes\n", ls, lines, kind, eob, n, la);
                    free(band); free(a); free(b); free(back);
                }
    printf("%d cases, %d failures\n", cases, fails);
    return fails != 0;
}
