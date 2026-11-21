// CAPT: the host-based protocol of Canon's small laser printers (LBP2900,
// LBP3000, LBP3010/3018/3050, ...). The printer has no page description
// language: the host renders each page to 1-bit 600 dpi raster, compresses
// it (Hi-SCoA, hiscoa.h) and drives the engine with request/reply commands
// on the printer interface's bulk endpoints.
//
// The command sequence follows captdriver's (github.com/mounaiban/captdriver
// and ValdikSS's fork), the open-source driver whose users print with these
// models; see docs/capt.md for what each step is, where it comes from and
// what is verified on hardware. Differences from captdriver: every wait has a
// deadline (a printer that stops answering fails the job instead of hanging
// the server), stale replies are drained before a job, and paper-out, cancel
// and link failures end in a defined state.
//
// Portable C: the transport and the clock come through capt_io_t.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- commands (names from Canon's driver as recovered by ValdikSS)
#define CAPT_GET_INPUT_STATUS 0xa0a1
#define CAPT_GET_EXT_STATUS 0xa0a8
#define CAPT_GET_PRINTER_INFO 0xa1a1
#define CAPT_RESERVE_UNIT 0xa2a0
#define CAPT_START_0 0xa3a2         // purpose unknown; captdriver sends it before ReserveUnit
#define CAPT_VIDEO_DATA 0xc0a0      // compressed raster, no reply
#define CAPT_BLACK_END 0xc0a4       // end of a page's raster, no reply
#define CAPT_BEGIN_PAGE 0xd0a0      // page parameters (inside 0xd0a9), no reply
#define CAPT_BEGIN_DATA 0xd0a1
#define CAPT_END_PAGE 0xd0a2
#define CAPT_HISCOA_PARAMS 0xd0a4
#define CAPT_MULTI 0xd0a9           // container of 0xd0xx commands, no reply
#define CAPT_GET_BASIC_STATUS 0xe0a0
#define CAPT_CLEAR_ERROR 0xe0a2
#define CAPT_CLEAR_MISPRINT 0xe0a3
#define CAPT_DISCARD_DATA 0xe0a4
#define CAPT_GO_ONLINE 0xe0a5
#define CAPT_GO_OFFLINE 0xe0a6
#define CAPT_START_PRINT 0xe0a7
#define CAPT_RELEASE_UNIT 0xe0a9
#define CAPT_SET_JOB_INFO 0xe1a1
#define CAPT_SET_LED 0xe1a2

// --- results
#define CAPT_OK 0
#define CAPT_ERR_IO (-1)            // USB transfer failed (unplugged, stalled)
#define CAPT_ERR_TIMEOUT (-2)       // no reply in time: the link is out of step
#define CAPT_ERR_PROTOCOL (-3)      // a reply that does not fit the request
#define CAPT_ERR_PRINTER (-4)       // the printer refused or stayed busy past a deadline
#define CAPT_ERR_PAPER (-5)         // out of paper and nobody refilled it in time
#define CAPT_ERR_CANCELED (-6)
#define CAPT_ERR_JAM (-7)           // jam / door open / no cartridge (engine flags)
const char *capt_strerror(int err);

struct capt;

// --- the transport and the clock
typedef struct {
    // Bytes to the printer (may be buffered); < 0 = the link failed.
    int (*write)(void *ctx, const uint8_t *data, size_t len);
    // Sends what write buffered, then one bulk IN transfer of at most max
    // bytes: > 0 bytes read, 0 = nothing within timeout_ms, < 0 = the link
    // failed. max = 0: only flush.
    int (*read)(void *ctx, uint8_t *buf, size_t max, int timeout_ms);
    void (*sleep_ms)(void *ctx, int ms);
    uint32_t (*now_ms)(void *ctx);
    bool (*cancelled)(void *ctx);                       // optional
    // optional: level 0 error, 1 warning, 2 info, 3 debug (command traces)
    void (*log)(void *ctx, int level, const char *msg);
    // optional: after every status query (c->st is fresh)
    void (*on_status)(void *ctx, const struct capt *c);
    void *ctx;
} capt_io_t;

// --- printer state
typedef enum {
    CAPT_COND_UNKNOWN = 0,
    CAPT_COND_READY,
    CAPT_COND_PRINTING,
    CAPT_COND_PAPER_OUT,        // waiting for paper (and the printer's button on some models)
    CAPT_COND_JAM,
    CAPT_COND_DOOR_OPEN,
    CAPT_COND_NO_CARTRIDGE,
    CAPT_COND_NOT_READY,        // warming up, or no paper (the printer does not say which)
    CAPT_COND_OFFLINE,
    CAPT_COND_ERROR,            // link or protocol failure
} capt_cond_t;

typedef struct {
    bool valid;
    capt_cond_t cond;
    uint16_t basic;             // first 2 bytes of the last status reply (basic or extended)
    uint8_t buf_level;          // free buffer slots 0..15 (bytes 4-5)
    uint8_t aux, cnt, pap;      // extended status bytes 8, 9, 10
    uint16_t eng;               // extended status bytes 12-13
    uint16_t page_decoding, page_printing, page_out, page_completed, page_received;
    uint8_t ext[64];            // the last extended status reply's payload
    uint8_t ext_len;
} capt_status_t;

// --- models
typedef enum {
    CAPT_POLL_BASIC,            // 0xe0a0, 0xa0a8 when it says the extended status changed (LBP2900)
    CAPT_POLL_EXT,              // 0xa0a8 only (captdriver's LBP3010/3018/3050 path)
} capt_poll_t;

typedef struct {
    const char *name;
    capt_poll_t poll;
    uint16_t model_const;       // 0xd0a0 bytes 2-3 (0x2a30 in captdriver for all models)
    uint8_t reserve[8];         // 0xa2a0 payload
    const uint8_t *led_init;    // 0xe1a2 payload at job start
    uint8_t led_init_len;
    const uint8_t *led_idle;    // 0xe1a2 after a paper wait and at cancel
    uint8_t led_idle_len;
    const uint8_t *led_blink;   // 0xe1a2 while waiting for paper
    uint8_t led_blink_len;
    bool button_is_aux20;       // "paper reloaded" = aux 0x20 (LBP2900) else extended byte 10 bit 7
    uint8_t media_plain;        // 0xd0a0 byte 12 for plain paper
    uint8_t page_params_len;    // 0xd0a0 payload: 40
    int band_lines;             // raster lines per Hi-SCoA band
    bool eop_marker;            // the last band ends with "end of page" (else "end of band")
} capt_model_t;

extern const capt_model_t capt_model_lbp2900, capt_model_lbp3010;

// --- a page, compressed
typedef struct {
    uint8_t paper_code;         // 0xd0a0 byte 4: A4 0x02, A5 0x03, Letter 0x0d, Legal 0x0c
    uint16_t margin_x, margin_y;    // image origin on the sheet, 600 dpi pixels
    uint16_t line_size;         // bytes per raster line
    uint16_t lines;             // raster lines
    uint16_t paper_w, paper_h;  // sheet, 600 dpi pixels
    uint8_t density;            // toner density 0..15 (0xd0a0 bytes 8-11 = density << 2)
    bool toner_save;
    const uint8_t *data;        // the Hi-SCoA bands, back to back
    size_t len;
    const uint32_t *band_end;   // end offset of each band in data
    int nbands;
} capt_page_t;

// --- tunables (capt_settings, shared by all jobs; change between jobs)
typedef struct {
    int poll_ms;                // status poll interval while waiting (100)
    int reply_timeout_ms;       // one reply (5000)
    int ready_timeout_ms;       // printer busy before a command or page (180000)
    int page_timeout_ms;        // a page from StartPrint to out (120000)
    int paper_timeout_ms;       // waiting for paper before the job fails (600000)
    // 0: the next page waits until this one left the printer (captdriver,
    // slowest, safest); 1: until it is printing (ValdikSS's fork, faster)
    int pipeline;
    bool input_status;          // answer "input status changed" with 0xa0a1
    bool trace;                 // log every command and reply (log level 3)
} capt_settings_t;
extern capt_settings_t capt_settings;

// What the engine is doing, for the status page and the IPP state.
typedef enum {
    CAPT_PHASE_IDLE,
    CAPT_PHASE_STARTING,
    CAPT_PHASE_SENDING,         // raster to the printer
    CAPT_PHASE_PRINTING,
    CAPT_PHASE_WAIT_PAPER,
    CAPT_PHASE_ENDING,
} capt_phase_t;

typedef struct capt {
    capt_io_t io;
    const capt_model_t *model;
    capt_status_t st;
    capt_phase_t phase;
    bool have_info;
    uint16_t dev_id;            // from GetPrinterInfo, 0 = unknown
    uint16_t chunk_max;         // largest 0xc0a0 payload
    bool buf_level_seen;        // the status has shown a non-zero buffer level
    bool in_job, page_open;
    uint16_t job_id;
    int pages_sent;             // pages started this job
    int last_err;
    uint32_t bytes_sent;
    uint8_t rx[512];
} capt_t;

void capt_init(capt_t *c, const capt_io_t *io, const capt_model_t *model);
// The status, outside a job: GetPrinterInfo (once), then a status query.
int capt_query(capt_t *c);
// Starts a job: drains stale replies, identifies, reserves the engine.
int capt_job_begin(capt_t *c);
// Prints one page; returns once it is printing (pipeline 1) or out (0).
// Waits for paper when the printer runs out, then prints the page again.
int capt_print_page(capt_t *c, const capt_page_t *pg);
// Ends the job: ok waits for the last page; otherwise (error, cancel) the
// engine is released without waiting. Returns 0 or the link error.
int capt_job_end(capt_t *c, bool ok);

// A command with a reply: payload of the reply into reply (up to max),
// its length in *len. 0, or a CAPT_ERR_*.
int capt_command(capt_t *c, uint16_t cmd, const void *payload, size_t n, uint8_t *reply, size_t max, size_t *len);

// The 0xd0a0 payload for a page (exposed for the emulated printer's checks).
size_t capt_page_params(const capt_t *c, const capt_page_t *pg, uint8_t *out);

#ifdef __cplusplus
}
#endif
