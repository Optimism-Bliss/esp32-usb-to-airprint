// Canon CAPT laser printers: LBP2900 (and LBP2900B), LBP3010 / LBP3018 /
// LBP3050. Host-based: the driver turns the client's gray (or RGB) raster
// into the printer's 600 dpi 1-bit image, compresses it band by band
// (Hi-SCoA) into one page buffer, then drives the printer through the CAPT
// engine (components/capt). See docs/capt.md.
//
// Rendering: crop to the image area, gray = luma, a 45 degree clustered-dot
// screen (8x8 cell, 2 dots: 106 lpi, 33 levels) that a laser reproduces
// better than dispersed dots; pure black and white stay pure, so text is
// unaffected. 300 dpi sources are doubled in both directions.
//
// Memory: a band (70 lines x 600 bytes) in RAM, the compressed page in
// io->page_buffer (PSRAM). A4 / Letter / A5 at worst (noise) need 6.1 MB,
// so every page fits the board's 7.5 MB buffer; Legal (7.7 MB worst) is not
// offered.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "capt.h"
#include "hiscoa.h"
#include "canon_capt.h"
#include "printer_driver.h"

#define DPI 600

typedef struct {
    const char *name;
    uint8_t code;               // 0xd0a0 byte 4
    uint16_t w, h;              // sheet at 600 dpi (Canon's table)
} capt_paper_t;

// same order as capt_papers[] below
static const capt_paper_t paper_px[] = {
    {"A4", 0x02, 4960, 7014},
    {"Letter", 0x0d, 5100, 6600},
    {"A5", 0x03, 3496, 4960},
};

// Canon's image area: 120 px (5.08 mm) left and right, 96 px (4.06 mm) at
// the top, at least 141 px (5.97 mm) at the bottom; width rounded up to 32
// pixels, height down to 8 lines (6776 lines on A4, as Canon's driver sends).
#define MARGIN_X 120
#define MARGIN_TOP 96
#define MARGIN_BOTTOM 141

static const printer_paper_t capt_papers[] = {
    {"A4", "iso_a4_210x297mm", 21000, 29700},
    {"Letter", "na_letter_8.5x11in", 21590, 27940},
    {"A5", "iso_a5_148x210mm", 14800, 21000},
};

// --- settings shared by the models (the debug server may change them)
static int s_density = 7;       // 0..15; 7 = Canon's default 0x1c
static bool s_toner_save;

// the 45 degree clustered-dot cell (two dots per 8x8 tile)
static const uint8_t screen8[8][8] = {
    {24, 10, 12, 26, 35, 47, 49, 37}, {8, 0, 2, 14, 45, 59, 61, 51},  {22, 6, 4, 16, 43, 57, 63, 53},
    {30, 20, 18, 28, 33, 41, 55, 39}, {34, 46, 48, 36, 25, 11, 13, 27}, {44, 58, 60, 50, 9, 1, 3, 15},
    {42, 56, 62, 52, 23, 7, 5, 17},   {32, 40, 54, 38, 31, 21, 19, 29},
};

typedef struct {
    const printer_driver_t *drv;
    const capt_model_t *model;
    printer_io_t io;
    capt_t *eng;
    bool started;               // capt_job_begin done
    bool failed;
    int err;
    // geometry
    int dpi, scale;
    const capt_paper_t *paper;
    int line_size, lines;       // image: bytes per line, lines
    int src_w, src_h, bpp;
    // the page being rendered
    int out_y;                  // next image line
    uint8_t *band;
    int band_y;                 // lines in band
    uint8_t thr[8][8];
    uint8_t *page;              // compressed page
    size_t page_cap, page_len;
    bool page_own;
    uint32_t band_end[160];
    int nbands;
    bool overflow;
    // stats
    int pages;
    long render_us, compress_us, send_us;
} capt_job_t;

static capt_t s_engine;         // one printer: the job's and the status polls'
static const capt_model_t *s_engine_model;

static long now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000L + t.tv_nsec / 1000;
}

// --- capt_io_t on top of printer_io_t

static int a_write(void *ctx, const uint8_t *d, size_t n)
{
    const printer_io_t *io = ctx;
    return io->write(io->ctx, d, n);
}

static int a_read(void *ctx, uint8_t *b, size_t max, int timeout_ms)
{
    const printer_io_t *io = ctx;
    return io->read ? io->read(io->ctx, b, max, timeout_ms) : -1;
}

static void a_sleep(void *ctx, int ms)
{
    const printer_io_t *io = ctx;
    if (io->sleep_ms)
        io->sleep_ms(io->ctx, ms);
    else
        usleep((useconds_t)ms * 1000);
}

static uint32_t a_now(void *ctx)
{
    const printer_io_t *io = ctx;
    return io->now_ms ? io->now_ms(io->ctx) : (uint32_t)(now_us() / 1000);
}

static bool a_cancelled(void *ctx)
{
    const printer_io_t *io = ctx;
    return io->cancelled && io->cancelled(io->ctx);
}

static void a_log(void *ctx, int level, const char *msg)
{
    const printer_io_t *io = ctx;
    if (io->log)
        io->log(io->ctx, level, msg);
}

static const char *cond_detail(capt_cond_t c)
{
    switch (c) {
    case CAPT_COND_PAPER_OUT: return "media-empty";
    case CAPT_COND_JAM: return "media-jam";
    case CAPT_COND_DOOR_OPEN: return "door-open";
    case CAPT_COND_NO_CARTRIDGE: return "toner-missing";
    case CAPT_COND_NOT_READY: return "not-ready";
    case CAPT_COND_OFFLINE: return "offline";
    case CAPT_COND_ERROR: return "link-error";
    default: return "";
    }
}

static void a_on_status(void *ctx, const capt_t *c)
{
    const printer_io_t *io = ctx;
    if (!io->status)
        return;
    const char *state = c->phase == CAPT_PHASE_IDLE ? "idle" : "processing";
    if (c->phase == CAPT_PHASE_WAIT_PAPER || c->st.cond == CAPT_COND_JAM || c->st.cond == CAPT_COND_DOOR_OPEN ||
        c->st.cond == CAPT_COND_NO_CARTRIDGE || c->st.cond == CAPT_COND_ERROR)
        state = "stopped";
    const char *detail = c->phase == CAPT_PHASE_WAIT_PAPER ? "media-empty" : cond_detail(c->st.cond);
    char data[96];
    snprintf(data, sizeof(data), "st %04x buf %u aux %02x cnt %02x pap %02x eng %04x pages %u/%u/%u/%u",
             c->st.basic, c->st.buf_level, c->st.aux, c->st.cnt, c->st.pap, c->st.eng, c->st.page_decoding,
             c->st.page_printing, c->st.page_out, c->st.page_completed);
    io->status(io->ctx, state, detail, data);
}

static capt_t *engine_for(const printer_driver_t *d, const printer_io_t *io)
{
    const capt_model_t *m = d->model;
    capt_io_t cio = {.write = a_write, .read = a_read, .sleep_ms = a_sleep, .now_ms = a_now,
                     .cancelled = a_cancelled, .log = a_log, .on_status = a_on_status, .ctx = (void *)io};
    if (s_engine_model != m) {
        capt_init(&s_engine, &cio, m);
        s_engine_model = m;
    } else {
        s_engine.io = cio;      // same printer: keep what it told us (printer info)
    }
    return &s_engine;
}

// --- the job

static void *capt_job_begin_(const printer_driver_t *d, const printer_io_t *io, int dpi, int paper)
{
    if (dpi != 300 && dpi != 600)
        return NULL;
    capt_job_t *j = calloc(1, sizeof(*j));
    if (!j)
        return NULL;
    j->drv = d;
    j->model = d->model;
    j->io = *io;
    j->dpi = dpi;
    j->scale = DPI / dpi;
    j->paper = &paper_px[paper >= 0 && paper < (int)(sizeof(paper_px) / sizeof(paper_px[0])) ? paper : 0];
    int w = j->paper->w - 2 * MARGIN_X;
    j->line_size = ((w + 31) & ~31) / 8;
    j->lines = (j->paper->h - MARGIN_TOP - MARGIN_BOTTOM) & ~7;
    j->band = malloc((size_t)j->line_size * j->model->band_lines);
    if (io->page_buffer)
        j->page = io->page_buffer(io->ctx, &j->page_cap);
    if (!j->page) {
        j->page_cap = hiscoa_bound((size_t)j->line_size * j->lines) + 4096;
        j->page = malloc(j->page_cap);
        j->page_own = true;
    }
    if (!j->band || !j->page) {
        free(j->band);
        if (j->page_own)
            free(j->page);
        free(j);
        return NULL;
    }
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            j->thr[y][x] = (uint8_t)((screen8[y][x] * 4 + 2) > 255 ? 255 : screen8[y][x] * 4 + 2);
    j->eng = engine_for(d, &j->io);
    return j;
}

static void flush_band(capt_job_t *j, bool last)
{
    if (!j->band_y || j->overflow)
        return;
    long t0 = now_us();
    size_t room = j->page_cap - j->page_len;
    size_t n = hiscoa_encode(j->band, (unsigned)j->line_size, (unsigned)j->band_y,
                             last && j->model->eop_marker ? HISCOA_EOB_PAGE : HISCOA_EOB_BAND, &hiscoa_params_default,
                             j->page + j->page_len, room);
    if (!n || j->nbands >= (int)(sizeof(j->band_end) / sizeof(j->band_end[0]))) {
        j->overflow = true;
    } else {
        j->page_len += n;
        j->band_end[j->nbands++] = (uint32_t)j->page_len;
    }
    j->band_y = 0;
    j->compress_us += now_us() - t0;
}

static uint8_t *next_line(capt_job_t *j)
{
    if (j->band_y == j->model->band_lines)
        flush_band(j, false);
    return j->band + (size_t)j->line_size * j->band_y++;
}

static int capt_page_begin(void *job, int src_w, int src_h, int bytes_pp)
{
    capt_job_t *j = job;
    if (j->failed)
        return -1;
    j->src_w = src_w;
    j->src_h = src_h;
    j->bpp = bytes_pp;
    j->out_y = 0;
    j->band_y = 0;
    j->page_len = 0;
    j->nbands = 0;
    j->overflow = false;
    return 0;
}

// one image line from source line px (NULL = white) at image line oy
static void render(capt_job_t *j, uint8_t *out, const uint8_t *px, int oy)
{
    memset(out, 0, (size_t)j->line_size);
    if (!px)
        return;
    const uint8_t *thr = j->thr[oy & 7];
    int s = j->scale, bpp = j->bpp;
    int x0 = MARGIN_X;                      // sheet x of image pixel 0
    int width = j->line_size * 8;
    int src_end = j->src_w * s;             // sheet x where the source ends
    for (int ob = 0; ob < j->line_size; ob++) {
        int sx0 = x0 + ob * 8;
        if (sx0 >= src_end)
            break;
        uint8_t byte = 0;
        for (int k = 0; k < 8; k++) {
            int sx = (sx0 + k) / s;
            if (sx >= j->src_w || ob * 8 + k >= width)
                break;
            unsigned g;
            if (bpp == 1) {
                g = px[sx];
            } else {
                const uint8_t *p = px + sx * 3;
                g = (p[0] * 77u + p[1] * 151u + p[2] * 28u) >> 8;
            }
            if (g == 255)
                continue;
            if (g < thr[k])
                byte |= (uint8_t)(0x80 >> k);
        }
        out[ob] = byte;
    }
}

static int capt_page_line(void *job, int y, const uint8_t *px)
{
    capt_job_t *j = job;
    if (j->failed)
        return -1;
    long t0 = now_us();
    // sheet lines y*s .. y*s+s-1 are image lines minus the top margin
    for (int k = 0; k < j->scale; k++) {
        int oy = y * j->scale + k - MARGIN_TOP;
        if (oy < j->out_y || oy >= j->lines)
            continue;
        while (j->out_y < oy)               // lines the source skipped
            memset(next_line(j), 0, (size_t)j->line_size), j->out_y++;
        render(j, next_line(j), px, oy);
        j->out_y++;
    }
    j->render_us += now_us() - t0;
    return 0;
}

static int capt_page_end(void *job)
{
    capt_job_t *j = job;
    if (j->failed)
        return -1;
    while (j->out_y < j->lines)
        memset(next_line(j), 0, (size_t)j->line_size), j->out_y++;
    flush_band(j, true);
    if (j->overflow) {
        a_log(&j->io, 0, "page does not fit the page buffer");
        j->failed = true;
        j->err = CAPT_ERR_PRINTER;
        return -1;
    }
    long t0 = now_us();
    int r = 0;
    if (!j->started) {
        r = capt_job_begin(j->eng);
        j->started = r == 0;
    }
    if (!r) {
        capt_page_t pg = {.paper_code = j->paper->code, .margin_x = MARGIN_X, .margin_y = MARGIN_TOP,
                          .line_size = (uint16_t)j->line_size, .lines = (uint16_t)j->lines, .paper_w = j->paper->w,
                          .paper_h = j->paper->h, .density = (uint8_t)s_density, .toner_save = s_toner_save,
                          .data = j->page, .len = j->page_len, .band_end = j->band_end, .nbands = j->nbands};
        r = capt_print_page(j->eng, &pg);
    }
    j->send_us += now_us() - t0;
    if (r) {
        char m[80];
        snprintf(m, sizeof(m), "page %d: %s", j->pages + 1, capt_strerror(r));
        a_log(&j->io, r == CAPT_ERR_CANCELED ? 2 : 0, m);
        j->failed = true;
        j->err = r;
        return -1;
    }
    j->pages++;
    return 0;
}

static int capt_job_end_(void *job)
{
    capt_job_t *j = job;
    int r = 0;
    if (j->started) {
        bool ok = !j->failed && !a_cancelled(&j->io);
        r = capt_job_end(j->eng, ok);
    }
    free(j->band);
    if (j->page_own)
        free(j->page);
    free(j);
    // a link failure: the platform resets the printer
    return (r == CAPT_ERR_IO || r == CAPT_ERR_TIMEOUT || r == CAPT_ERR_PROTOCOL) ? -1 : 0;
}

static void capt_job_stats(void *job, char *buf, size_t n)
{
    capt_job_t *j = job;
    snprintf(buf, n, "CAPT %s: %d pages, %u KB sent; render %.2f s, compress %.2f s, printer %.2f s",
             j->model->name, j->pages, (unsigned)(j->eng->bytes_sent / 1024), j->render_us / 1e6,
             j->compress_us / 1e6, j->send_us / 1e6);
}

static int capt_poll_(const printer_driver_t *d, const printer_io_t *io)
{
    capt_t *c = engine_for(d, io);
    int r = capt_query(c);
    c->io.ctx = NULL;           // io is the caller's, valid only during the call
    return r;
}

// --- status: what the driver reported (a_on_status) into IPP terms
static void capt_status(const char *state, const char *detail, const char *data, airprint_status_t *st)
{
    struct {
        const char *detail, *reason, *message;
    } map[] = {
        {"media-empty", "media-empty-error", "Out of paper: load paper, then press the printer's button"},
        {"media-jam", "media-jam-error", "Paper jam"},
        {"door-open", "door-open-error", "Cover open"},
        {"toner-missing", "toner-empty-error", "No toner cartridge"},
        {"not-ready", "other-warning", "Printer not ready (warming up or no paper)"},
        {"offline", NULL, NULL},
        {"link-error", "other-error", "The printer does not answer: switch it off and on"},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(detail, map[i].detail) || !map[i].reason)
            continue;
        if (st->nreasons < 6)
            st->reasons[st->nreasons++] = map[i].reason;
        snprintf(st->message, sizeof(st->message), "%s", map[i].message);
    }
    (void)state;
    (void)data;
}

// --- test page: frame, title, gray steps, line widths, a grid

static const uint8_t font5x7[][7] = {
    // ' ' .. 'Z' (ASCII 32..90), 5 bits per row, bit 4 leftmost
    {0, 0, 0, 0, 0, 0, 0},           {4, 4, 4, 4, 4, 0, 4},           {10, 10, 0, 0, 0, 0, 0},
    {10, 31, 10, 10, 31, 10, 0},     {4, 15, 20, 14, 5, 30, 4},       {24, 25, 2, 4, 8, 19, 3},
    {12, 18, 20, 8, 21, 18, 13},     {4, 4, 0, 0, 0, 0, 0},           {2, 4, 8, 8, 8, 4, 2},
    {8, 4, 2, 2, 2, 4, 8},           {0, 4, 21, 14, 21, 4, 0},        {0, 4, 4, 31, 4, 4, 0},
    {0, 0, 0, 0, 12, 4, 8},          {0, 0, 0, 31, 0, 0, 0},          {0, 0, 0, 0, 0, 12, 12},
    {0, 1, 2, 4, 8, 16, 0},          {14, 17, 19, 21, 25, 17, 14},    {4, 12, 4, 4, 4, 4, 14},
    {14, 17, 1, 2, 4, 8, 31},        {31, 2, 4, 2, 1, 17, 14},        {2, 6, 10, 18, 31, 2, 2},
    {31, 16, 30, 1, 1, 17, 14},      {6, 8, 16, 30, 17, 17, 14},      {31, 1, 2, 4, 8, 8, 8},
    {14, 17, 17, 14, 17, 17, 14},    {14, 17, 17, 15, 1, 2, 12},      {0, 12, 12, 0, 12, 12, 0},
    {0, 12, 12, 0, 12, 4, 8},        {2, 4, 8, 16, 8, 4, 2},          {0, 0, 31, 0, 31, 0, 0},
    {8, 4, 2, 1, 2, 4, 8},           {14, 17, 1, 2, 4, 0, 4},         {14, 17, 1, 13, 21, 21, 14},
    {14, 17, 17, 31, 17, 17, 17},    {30, 17, 17, 30, 17, 17, 30},    {14, 17, 16, 16, 16, 17, 14},
    {28, 18, 17, 17, 17, 18, 28},    {31, 16, 16, 30, 16, 16, 31},    {31, 16, 16, 30, 16, 16, 16},
    {14, 17, 16, 23, 17, 17, 15},    {17, 17, 17, 31, 17, 17, 17},    {14, 4, 4, 4, 4, 4, 14},
    {7, 2, 2, 2, 2, 18, 12},         {17, 18, 20, 24, 20, 18, 17},    {16, 16, 16, 16, 16, 16, 31},
    {17, 27, 21, 21, 17, 17, 17},    {17, 17, 25, 21, 19, 17, 17},    {14, 17, 17, 17, 17, 17, 14},
    {30, 17, 17, 30, 16, 16, 16},    {14, 17, 17, 17, 21, 18, 13},    {30, 17, 17, 30, 20, 18, 17},
    {15, 16, 16, 14, 1, 1, 30},      {31, 4, 4, 4, 4, 4, 4},          {17, 17, 17, 17, 17, 17, 14},
    {17, 17, 17, 17, 17, 10, 4},     {17, 17, 17, 21, 21, 21, 10},    {17, 17, 10, 4, 10, 17, 17},
    {17, 17, 10, 4, 4, 4, 4},        {31, 1, 2, 4, 8, 16, 31},
};

// is sheet pixel (x, y) of text `s` at (x0, y0), cell size u dots, black?
static bool text_px(const char *s, int x0, int y0, int u, int x, int y)
{
    if (y < y0 || y >= y0 + 7 * u || x < x0)
        return false;
    int ci = (x - x0) / (6 * u);
    if (ci >= (int)strlen(s))
        return false;
    int cx = ((x - x0) % (6 * u)) / u, cy = (y - y0) / u;
    if (cx >= 5)
        return false;
    unsigned char ch = (unsigned char)s[ci];
    if (ch >= 'a' && ch <= 'z')
        ch = (unsigned char)(ch - 32);
    if (ch < 32 || ch > 90)
        ch = '?';
    return (font5x7[ch - 32][cy] >> (4 - cx)) & 1;
}

static int capt_test_page(const printer_driver_t *d, const printer_io_t *io, int dpi, const char *label)
{
    capt_job_t *j = capt_job_begin_(d, io, DPI, 0);
    if (!j)
        return -1;
    int W = j->paper->w, H = j->paper->h;
    uint8_t *line = malloc((size_t)W);
    int r = line ? capt_page_begin(j, W, H, 1) : -1;
    char title[64], info[96];
    snprintf(title, sizeof(title), "ESP32 CAPT TEST - %s", j->model->name);
    snprintf(info, sizeof(info), "%s", label ? label : "");
    const int e = 236;          // 10 mm at 600 dpi
    for (int y = 0; y < H && !r; y++) {
        for (int x = 0; x < W; x++) {
            // frame 10 mm from the edges, 2 dots wide
            bool blk = (x >= e && x < W - e && (y == e || y == e + 1 || y == H - e - 1 || y == H - e - 2)) ||
                       (y >= e && y < H - e && (x == e || x == e + 1 || x == W - e - 1 || x == W - e - 2));
            // 10 mm grid in the lower third
            if (y > H * 2 / 3 && y < H - e && x > e && x < W - e && ((x - e) % e == 0 || (y - e) % e == 0))
                blk = true;
            if (text_px(title, 400, 400, 12, x, y) || text_px(info, 400, 600, 6, x, y))
                blk = true;
            uint8_t g = blk ? 0 : 255;
            // 11 gray steps, 0..100 %
            if (y >= 900 && y < 1500 && x >= 400 && x < 400 + 11 * 360) {
                int step = (x - 400) / 360;
                g = (uint8_t)(255 - step * 255 / 10);
            }
            // lines 1..8 dots wide
            if (y >= 1700 && y < 2300)
                for (int k = 1; k <= 8; k++) {
                    int lx = 400 + k * 300;
                    if (x >= lx && x < lx + k)
                        g = 0;
                }
            line[x] = g;
        }
        r = capt_page_line(j, y, line);
        if (io->progress && (y & 63) == 63)
            io->progress(io->ctx, y, H);
    }
    free(line);
    if (!r)
        r = capt_page_end(j);
    int end = capt_job_end_(j);
    return r ? r : end;
}

// --- the drivers

#define CAPT_DRIVER(var, id_, model_name, mdl, pid, m)                                                       \
    const printer_driver_t var = {                                                                       \
        .id = id_, .make_model = model_name, .usb_mfg = "Canon", .usb_mdl = mdl,                         \
        .usb_vid = 0x04a9, .usb_pid = pid, .color = false, .scanner = false,                             \
        .resolutions = {300, 600}, .nresolutions = 2,                                                    \
        .papers = capt_papers, .npapers = sizeof(capt_papers) / sizeof(capt_papers[0]),                  \
        .margin_left = 508, .margin_right = 508, .margin_top = 406, .margin_bottom = 610,                \
        .model = &m, .job_begin = capt_job_begin_, .page_begin = capt_page_begin,                        \
        .page_line = capt_page_line, .page_end = capt_page_end, .job_end = capt_job_end_,                \
        .job_stats = capt_job_stats, .test_page = capt_test_page, .status = capt_status,                 \
        .poll = capt_poll_, .raw_back_channel = true, .soft_cancel = true,                               \
    }

// USB ids: 04a9:2676 LBP2900 (seen in many lsusb logs), 04a9:26da the
// LBP3010/3018/3050 (captdriver's wiki); the MDL strings are the ones
// captdriver matches (the printers' IEEE 1284 ids).
CAPT_DRIVER(canon_lbp2900_driver, "canon-lbp2900", "Canon LBP2900", "LBP2900", 0x2676, capt_model_lbp2900);
CAPT_DRIVER(canon_lbp3050_driver, "canon-lbp3050", "Canon LBP3010/LBP3018/LBP3050", "LBP3010/LBP3018/LBP3050",
            0x26da, capt_model_lbp3010);

// settings from the debug server
void canon_capt_set(int density, bool toner_save)
{
    if (density >= 0 && density <= 15)
        s_density = density;
    s_toner_save = toner_save;
}

void canon_capt_get(int *density, bool *toner_save)
{
    *density = s_density;
    *toner_save = s_toner_save;
}
