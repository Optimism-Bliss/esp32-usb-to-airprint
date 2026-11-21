// Hi-SCoA, the band compression of Canon's CAPT laser printers (LBP2900,
// LBP3000, LBP3010/3018/3050, ...). Written from the format description in
// the SPECS file of captdriver (Alexey Galakhov's reverse engineering, see
// docs/capt.md); no code was taken from captdriver.
//
// A band is `lines` raster lines of `line_size` bytes, 1 bit per pixel,
// bit 7 of the first byte leftmost, 1 = toner. Each band is compressed on
// its own into a bit stream (MSB first, padded with 1s to 32 bits, every
// byte XORed with 0x43) of commands: literal byte, zero byte, one of the 16
// last literal bytes again, or a copy of N bytes from one of five distances
// before the current position (the line above, a few bytes back, ...).
// Portable C, no allocation.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The copy distances. The printer learns them from the 0xD0A4 command
// (hiscoa_params_bytes). LONGREP0 copies from line_size + l0 back, LONGREP2
// from line_size + l2, LONGREP3/4/5 from l3/l4/l5 back; 0 disables one.
typedef struct {
    int8_t l0, l2, l3, l5;
    int16_t l4;
} hiscoa_params_t;

// L0 = 0, L2 = -7, L3 = 1, L4 = unused, L5 = 4: what captdriver sends to the
// LBP2900 and LBP3010 (printed by its users). Canon's Windows driver sends
// L4 = 128 to the LBP3000.
extern const hiscoa_params_t hiscoa_params_default;

// End of band marker: the last band of a page may say "end of page".
#define HISCOA_EOB_BAND 0
#define HISCOA_EOB_PAGE 1

// Largest compressed size of a band of n bytes (all literals, 12 bits each,
// plus the end marker and the padding).
static inline size_t hiscoa_bound(size_t n) { return (n * 3 + 1) / 2 + 12; }

// Compresses one band into out (out_max >= hiscoa_bound(line_size * lines)
// never fails). Returns the size, a multiple of 4; 0 if it did not fit.
size_t hiscoa_encode(const uint8_t *band, unsigned line_size, unsigned lines, int eob,
                     const hiscoa_params_t *p, uint8_t *out, size_t out_max);

// The 8 bytes of the 0xD0A4 command for these parameters.
void hiscoa_params_bytes(const hiscoa_params_t *p, uint8_t out[8]);

// Decompresses one band (tests and the emulated printer). *in_used = bytes
// consumed (padding included), *out_len = bytes produced. Returns the end
// marker (HISCOA_EOB_*), or -1 for a stream without end marker, a copy from
// before the band, or more output than out_max.
int hiscoa_decode(const uint8_t *in, size_t in_len, size_t *in_used, unsigned line_size,
                  const hiscoa_params_t *p, uint8_t *out, size_t out_max, size_t *out_len);

#ifdef __cplusplus
}
#endif
