// An emulated CAPT printer (the device side of capt.h) for the host tests.
// Time is virtual: it advances when the host sleeps or a read waits.
//
// What it models is our reading of the protocol (docs/capt.md): it shows
// that the engine does what we intend, not that a real printer agrees. It
// flags every protocol violation it can see (a command sent while a reply is
// unread, raster without a page, a page whose bands do not decode to its
// size, data while the buffer is full, a StartPrint for the wrong page, an
// engine starved of data, a job left reserved).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "capt.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FAKE_CAPT_MAX_PAGES 32

typedef struct {
    // --- scenario (set before or during the run)
    int paper;                  // sheets in the tray, -1 = endless
    bool unplugged;             // every transfer fails
    int drop_reply_of;          // the nth command (1-based) gets no reply, 0 = none
    bool stuck_busy;            // the engine never finishes a page
    size_t ram;                 // printer memory for compressed data (2 MB)
    int print_ms;               // StartPrint to page out
    bool bcd_sizes;             // reply sizes in BCD (a firmware quirk of some models)

    // --- results
    char error[160];            // first protocol violation, "" = none
    int pages_printed;          // sheets out
    uint32_t page_crc[FAKE_CAPT_MAX_PAGES];   // CRC-32 of each printed page's decoded raster
    int line_size[FAKE_CAPT_MAX_PAGES], lines[FAKE_CAPT_MAX_PAGES];
    uint8_t *last_page;         // decoded raster of the last printed page (line_size * lines)
    int commands;               // commands received
    bool in_job;                // a unit is reserved
    bool online;
    bool paper_out;
    int max_ram_used;
    bool streamed;              // a page was started before all its data arrived

    // --- internal
    const capt_model_t *model;
    uint32_t now;
    uint8_t in[70000];          // bytes from the host not parsed yet
    size_t in_len;
    uint8_t reply[128];         // reply waiting to be read
    size_t reply_len, reply_pos;
    bool reply_split_done;
    bool stale_job;             // a reserve was refused: the next GoOnline drops the old job
    bool button;                // the printer's button pressed (LBP2900: aux 0x20)
    uint16_t job_id;
    uint16_t decoding, printing, out, completed, received;
    bool ext_changed, input_changed;
    // the page being received / printed
    struct {
        bool open, complete, started, data_ended;
        uint8_t params[48];
        int line_size, lines;
        uint8_t *data;
        size_t len, cap;
        size_t consumed;        // bytes the engine used so far
        uint32_t start_at;      // when StartPrint came
        bool decoded_ok;
    } pg;
} fake_capt_t;

void fake_capt_init(fake_capt_t *f, const capt_model_t *model);
void fake_capt_free(fake_capt_t *f);
// A capt_io_t that talks to f (ctx = f).
capt_io_t fake_capt_io(fake_capt_t *f);
// The printer's button (after reloading paper).
void fake_capt_press_button(fake_capt_t *f);

#ifdef __cplusplus
}
#endif
