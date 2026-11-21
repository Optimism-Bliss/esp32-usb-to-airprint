// CAPT protocol engine (capt.h, docs/capt.md).
#include "capt.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "hiscoa.h"

// --- model constants (protocol data: captdriver's prn_lbp2900.c)
static const uint8_t led_3010_init[16] = {0x13};
static const uint8_t led_3010_blink[16] = {0x31};
static const uint8_t led_2900_idle[12] = {0};
static const uint8_t led_2900_blink[12] = {0x00, 0x00, 0x01, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00};

// captdriver's LBP2900 path: basic status, the LBP3010 LED payload at job
// start (as captdriver sends it), 70-line bands, plain paper = 0x00
const capt_model_t capt_model_lbp2900 = {
    .name = "LBP2900",
    .poll = CAPT_POLL_BASIC,
    .model_const = 0x2a30,
    .reserve = {0x00, 0x00, 0x1e, 0x00},
    .led_init = led_3010_init, .led_init_len = sizeof(led_3010_init),
    .led_idle = led_2900_idle, .led_idle_len = sizeof(led_2900_idle),
    .led_blink = led_2900_blink, .led_blink_len = sizeof(led_2900_blink),
    .button_is_aux20 = true,
    .media_plain = 0x00,
    .page_params_len = 40,
    .band_lines = 70,
    .eop_marker = true,         // ValdikSS's fork (tested on the LBP2900); captdriver sends "end of band"
};

// captdriver's LBP3010/LBP3018/LBP3050 path: extended status only,
// plain paper = 0x01, "end of band" on every band. Bands of 70 lines (not
// captdriver's 160) so a band always fits one 0xc0a0 chunk.
const capt_model_t capt_model_lbp3010 = {
    .name = "LBP3010/LBP3018/LBP3050",
    .poll = CAPT_POLL_EXT,
    .model_const = 0x2a30,
    .reserve = {0x00, 0x00, 0x1e, 0x00},
    .led_init = led_3010_init, .led_init_len = sizeof(led_3010_init),
    .led_idle = led_3010_init, .led_idle_len = sizeof(led_3010_init),
    .led_blink = led_3010_blink, .led_blink_len = sizeof(led_3010_blink),
    .button_is_aux20 = false,
    .media_plain = 0x01,
    .page_params_len = 40,
    .band_lines = 70,
    .eop_marker = false,
};

capt_settings_t capt_settings = {
    .poll_ms = 100,
    .reply_timeout_ms = 5000,
    .ready_timeout_ms = 180000,
    .page_timeout_ms = 120000,
    .paper_timeout_ms = 600000,
    .pipeline = 0,
    .input_status = true,
    .trace = false,
};

const char *capt_strerror(int err)
{
    switch (err) {
    case CAPT_OK: return "ok";
    case CAPT_ERR_IO: return "USB link failed";
    case CAPT_ERR_TIMEOUT: return "printer did not answer";
    case CAPT_ERR_PROTOCOL: return "unexpected reply";
    case CAPT_ERR_PRINTER: return "printer stayed busy";
    case CAPT_ERR_PAPER: return "out of paper";
    case CAPT_ERR_CANCELED: return "canceled";
    case CAPT_ERR_JAM: return "paper jam or cover open";
    }
    return "error";
}

#define LO(x) ((uint8_t)((x) & 0xff))
#define HI(x) ((uint8_t)(((x) >> 8) & 0xff))
#define LE16(p) ((uint16_t)((p)[0] | (p)[1] << 8))

// basic status bits (byte 0) and their names in captdriver / Canon's driver
#define B_PROCESSING 0x0001     // RCF_PRINTER_FREE
#define B_NOT_READY 0x0002      // "paper out" in captdriver
#define B_CMD_BUSY 0x0004       // "buffer full" in captdriver: busy, e.g. printing
#define B_OFFLINE 0x0010        // "uninitialized" in captdriver
#define B_UNINIT 0x0020
#define B_BUSY80 0x0080         // "busy" in captdriver, ERROR_BIT in Canon's driver
#define B_EXT_CHANGED 0x0100    // byte 1: read the extended status
#define B_INPUT_CHANGED 0x0200  // byte 1: read the input status

static void logf_(capt_t *c, int level, const char *fmt, ...)
{
    if (!c->io.log || (level >= 3 && !capt_settings.trace))
        return;
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    c->io.log(c->io.ctx, level, buf);
}

static void hex(char *out, size_t n, const uint8_t *d, size_t len)
{
    size_t o = 0;
    out[0] = 0;
    for (size_t i = 0; i < len && o + 4 < n; i++)
        o += (size_t)snprintf(out + o, n - o, "%02x ", d[i]);
    if (len && o + 4 >= n)
        snprintf(out + n - 4, 4, "..");
}

static uint32_t now(capt_t *c) { return c->io.now_ms(c->io.ctx); }
static bool expired(capt_t *c, uint32_t t0, int ms) { return (int32_t)(now(c) - t0) >= ms; }
static void nap(capt_t *c) { c->io.sleep_ms(c->io.ctx, capt_settings.poll_ms); }
static bool cancelled(capt_t *c) { return c->io.cancelled && c->io.cancelled(c->io.ctx); }

void capt_init(capt_t *c, const capt_io_t *io, const capt_model_t *model)
{
    memset(c, 0, sizeof(*c));
    c->io = *io;
    c->model = model;
    c->chunk_max = 0xff00;
}

// --- framing

static int send_cmd(capt_t *c, uint16_t cmd, const void *payload, size_t n)
{
    uint8_t h[4] = {LO(cmd), HI(cmd), LO(n + 4), HI(n + 4)};
    if (capt_settings.trace && cmd != CAPT_VIDEO_DATA) {
        char hx[100];
        hex(hx, sizeof(hx), payload, n);
        logf_(c, 3, "> %04x [%u] %s", cmd, (unsigned)n, hx);
    }
    if (c->io.write(c->io.ctx, h, 4) < 0 || (n && c->io.write(c->io.ctx, payload, n) < 0))
        return CAPT_ERR_IO;
    return CAPT_OK;
}

static unsigned bcd16(uint16_t v)
{
    return (v >> 12 & 15) * 1000 + (v >> 8 & 15) * 100 + (v >> 4 & 15) * 10 + (v & 15);
}

// Reads replies left over from a command that timed out (they would be taken
// for the next command's reply).
static int drain(capt_t *c, int rounds)
{
    int dropped = 0;
    for (int i = 0; i < rounds; i++) {
        int r = c->io.read(c->io.ctx, c->rx, sizeof(c->rx), 30);
        if (r < 0)
            return CAPT_ERR_IO;
        if (r == 0)
            break;
        dropped += r;
    }
    if (dropped)
        logf_(c, 1, "dropped %d stale bytes from the printer", dropped);
    return CAPT_OK;
}

int capt_command(capt_t *c, uint16_t cmd, const void *payload, size_t n, uint8_t *reply, size_t max, size_t *len)
{
    int r = send_cmd(c, cmd, payload, n);
    if (r)
        return c->last_err = r;
    // A reply comes as a 6-byte packet, then the rest; some models give the
    // size in BCD. USB transfers end at short packets, so read until the
    // size (binary or BCD) is reached.
    size_t got = 0;
    for (int tries = 0; tries < 6; tries++) {
        int k = c->io.read(c->io.ctx, c->rx + got, sizeof(c->rx) - got, capt_settings.reply_timeout_ms);
        if (k < 0)
            return c->last_err = CAPT_ERR_IO;
        if (k == 0) {
            logf_(c, 0, "no reply to %04x after %d bytes", cmd, (int)got);
            drain(c, 4);
            return c->last_err = CAPT_ERR_TIMEOUT;
        }
        got += (size_t)k;
        if (got < 4)
            continue;
        if (LE16(c->rx) != cmd) {
            char hx[60];
            hex(hx, sizeof(hx), c->rx, got);
            logf_(c, 0, "reply to %04x is %s", cmd, hx);
            drain(c, 4);
            return c->last_err = CAPT_ERR_PROTOCOL;
        }
        uint16_t sz = LE16(c->rx + 2);
        unsigned bin = sz, bcd = bcd16(sz);
        if (got == bin || got == bcd)
            break;
        if (got > bin && got > bcd) {
            logf_(c, 0, "reply to %04x: %d bytes, size field %04x", cmd, (int)got, sz);
            return c->last_err = CAPT_ERR_PROTOCOL;
        }
        if (got >= sizeof(c->rx)) {
            logf_(c, 0, "reply to %04x too long (%u)", cmd, bin);
            drain(c, 8);
            return c->last_err = CAPT_ERR_PROTOCOL;
        }
    }
    if (got < 4) {
        logf_(c, 0, "short reply to %04x", cmd);
        return c->last_err = CAPT_ERR_PROTOCOL;
    }
    size_t plen = got - 4;
    if (capt_settings.trace) {
        char hx[100];
        hex(hx, sizeof(hx), c->rx + 4, plen);
        logf_(c, 3, "< %04x [%u] %s", cmd, (unsigned)plen, hx);
    }
    if (reply)
        memcpy(reply, c->rx + 4, plen < max ? plen : max);
    if (len)
        *len = plen;
    return CAPT_OK;
}

// --- status

static void classify(capt_t *c)
{
    capt_status_t *s = &c->st;
    bool printing = (s->aux & 0x84) || (s->basic & B_CMD_BUSY);
    if (s->eng & 0x4000)
        s->cond = CAPT_COND_DOOR_OPEN;
    else if (s->eng & 0x2000)
        s->cond = CAPT_COND_NO_CARTRIDGE;
    else if (s->eng & 0x0100)
        s->cond = CAPT_COND_JAM;
    else if ((s->cnt & 0x40) && !printing)
        s->cond = CAPT_COND_PAPER_OUT;
    else if (printing)
        s->cond = CAPT_COND_PRINTING;
    else if (s->basic & B_NOT_READY)
        s->cond = CAPT_COND_NOT_READY;
    else if (s->basic & (B_OFFLINE | B_UNINIT))
        s->cond = CAPT_COND_OFFLINE;
    else
        s->cond = CAPT_COND_READY;
}

static void parse_basic(capt_t *c, const uint8_t *p, size_t n)
{
    if (n >= 2)
        c->st.basic = LE16(p);
    if (n >= 6) {
        c->st.buf_level = p[4] & 0x0f;
        if (c->st.buf_level)
            c->buf_level_seen = true;
    }
}

static int read_ext(capt_t *c)
{
    uint8_t p[64];
    size_t n = 0;
    int r = capt_command(c, CAPT_GET_EXT_STATUS, NULL, 0, p, sizeof(p), &n);
    if (r)
        return r;
    if (n > sizeof(p))
        n = sizeof(p);
    capt_status_t *s = &c->st;
    parse_basic(c, p, n);
    if (n >= 14) {
        s->aux = p[8];
        s->cnt = p[9];
        s->pap = p[10];
        s->eng = LE16(p + 12);
    }
    if (n >= 22) {
        s->page_decoding = LE16(p + 14);
        s->page_printing = LE16(p + 16);
        s->page_out = LE16(p + 18);
        s->page_completed = LE16(p + 20);
    }
    if (n >= 36)
        s->page_received = LE16(p + 34);
    memcpy(s->ext, p, n);
    s->ext_len = (uint8_t)n;
    if (capt_settings.input_status && (s->basic & B_INPUT_CHANGED)) {
        uint8_t in[32];
        r = capt_command(c, CAPT_GET_INPUT_STATUS, NULL, 0, in, sizeof(in), NULL);
    }
    return r;
}

// The model's status query. force_ext: also the extended status (page
// counters) when the basic status does not ask for it.
static int poll(capt_t *c, bool force_ext)
{
    int r;
    if (c->model->poll == CAPT_POLL_EXT) {
        r = read_ext(c);
    } else {
        uint8_t p[16];
        size_t n = 0;
        r = capt_command(c, CAPT_GET_BASIC_STATUS, NULL, 0, p, sizeof(p), &n);
        if (!r) {
            parse_basic(c, p, n < sizeof(p) ? n : sizeof(p));
            if (force_ext || (c->st.basic & B_EXT_CHANGED))
                r = read_ext(c);
            else if (capt_settings.input_status && (c->st.basic & B_INPUT_CHANGED))
                r = capt_command(c, CAPT_GET_INPUT_STATUS, NULL, 0, p, sizeof(p), NULL);
        }
    }
    if (r) {
        c->st.cond = CAPT_COND_ERROR;
        if (c->io.on_status)
            c->io.on_status(c->io.ctx, c);
        return r;
    }
    c->st.valid = true;
    classify(c);
    if (c->io.on_status)
        c->io.on_status(c->io.ctx, c);
    return 0;
}

// Waits until none of `bits` is set in the basic status. hard: a timeout is
// an error; else (bits captdriver waits for whose meaning is unsure) the job
// carries on after the deadline.
static int wait_clear(capt_t *c, uint16_t bits, int timeout_ms, bool hard)
{
    uint32_t t0 = now(c);
    for (;;) {
        int r = poll(c, false);
        if (r)
            return r;
        if (!(c->st.basic & bits))
            return 0;
        if (expired(c, t0, timeout_ms)) {
            logf_(c, hard ? 0 : 1, "status %04x still has %04x after %d ms", c->st.basic, bits, timeout_ms);
            return hard ? CAPT_ERR_PRINTER : 0;
        }
        nap(c);
    }
}

#define WAIT_READY(c) wait_clear(c, B_BUSY80, 10000, false)

static int simple(capt_t *c, uint16_t cmd, const void *p, size_t n)
{
    return capt_command(c, cmd, p, n, NULL, 0, NULL);
}

static int led(capt_t *c, const uint8_t *p, size_t n)
{
    return simple(c, CAPT_SET_LED, p, n);
}

// SetJobInfo2: 72 bytes; flags 1 start, 2 page, 6 page started, 4 end
static int job_info(capt_t *c, uint8_t flag)
{
    uint8_t b[72] = {0};
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    b[16] = flag;
    b[17] = 1;                  // pages per sheet
    b[18] = LO(c->job_id);
    b[19] = HI(c->job_id);
    unsigned year = (unsigned)tm.tm_year + 1900;
    b[24] = LO(year);
    b[25] = HI(year);
    b[26] = (uint8_t)(tm.tm_mon + 1);
    b[27] = (uint8_t)tm.tm_mday;
    b[28] = (uint8_t)tm.tm_hour;
    b[29] = (uint8_t)tm.tm_min;
    b[30] = (uint8_t)tm.tm_sec;
    return simple(c, CAPT_SET_JOB_INFO, b, sizeof(b));
}

static int printer_info(capt_t *c)
{
    uint8_t p[64];
    size_t n = 0;
    int r = capt_command(c, CAPT_GET_PRINTER_INFO, NULL, 0, p, sizeof(p), &n);
    if (r)
        return r;
    if (n >= 10) {
        c->dev_id = LE16(p + 2);
        uint16_t blk = LE16(p + 6);
        c->chunk_max = (blk >= 4096 && blk < 0xff00) ? blk : 0xff00;
        logf_(c, 2, "printer info: id %04x, block %u, buffers %u", c->dev_id, blk, LE16(p + 8));
    }
    c->have_info = true;
    return 0;
}

int capt_query(capt_t *c)
{
    int r = drain(c, 2);
    if (!r && !c->have_info)
        r = printer_info(c);
    if (!r)
        r = poll(c, true);
    return r;
}

// Clear the printer's error state and bring it online (captdriver does this
// before a page when the status says offline / not initialised).
static int clear_and_online(capt_t *c)
{
    static const uint8_t online[16] = {0xee, 0xdb, 0xea, 0xad};
    int r;
    if ((r = simple(c, CAPT_CLEAR_MISPRINT, NULL, 0)) || (r = simple(c, CAPT_CLEAR_ERROR, NULL, 0)) ||
        (r = simple(c, CAPT_DISCARD_DATA, NULL, 0)) || (r = WAIT_READY(c)) ||
        (r = simple(c, CAPT_GO_ONLINE, online, sizeof(online))) || (r = WAIT_READY(c)))
        return r;
    return 0;
}

int capt_job_begin(capt_t *c)
{
    int r;
    uint8_t p[16];
    size_t n = 0;
    c->phase = CAPT_PHASE_STARTING;
    c->last_err = 0;            // a failed job before must not fail this one
    c->pages_sent = 0;
    c->bytes_sent = 0;
    c->page_open = false;
    if ((r = drain(c, 8)) || (r = printer_info(c)) || (r = poll(c, true)))
        goto fail;
    if ((r = simple(c, CAPT_START_0, NULL, 0)))
        goto fail;
    r = capt_command(c, CAPT_RESERVE_UNIT, c->model->reserve, sizeof(c->model->reserve), p, sizeof(p), &n);
    if (!r && n >= 1 && (p[0] & 0x80)) {
        // a job left over (a crash, a pulled cable): clear it and try again
        logf_(c, 1, "reserve refused (%02x), clearing the printer", p[0]);
        if ((r = clear_and_online(c)))
            goto fail;
        r = capt_command(c, CAPT_RESERVE_UNIT, c->model->reserve, sizeof(c->model->reserve), p, sizeof(p), &n);
        if (!r && n >= 1 && (p[0] & 0x80)) {
            r = CAPT_ERR_PRINTER;
            goto fail;
        }
    }
    if (r)
        goto fail;
    c->job_id = n >= 4 ? LE16(p + 2) : 0;
    c->in_job = true;
    logf_(c, 2, "job %u reserved", c->job_id);
    if ((r = led(c, c->model->led_init, c->model->led_init_len)) || (r = WAIT_READY(c)) || (r = job_info(c, 1)) ||
        (r = WAIT_READY(c)))
        goto fail;
    return 0;
fail:
    logf_(c, 0, "job start failed: %s", capt_strerror(r));
    return c->last_err = r;
}

size_t capt_page_params(const capt_t *c, const capt_page_t *pg, uint8_t *b)
{
    size_t n = c->model->page_params_len;
    memset(b, 0, n);
    b[2] = LO(c->model->model_const);
    b[3] = HI(c->model->model_const);
    b[4] = pg->paper_code;
    uint8_t d = (uint8_t)((pg->density & 15) << 2);
    b[8] = b[9] = b[10] = b[11] = d;
    b[12] = c->model->media_plain;
    b[13] = 0x11;               // 600 dpi
    b[14] = 0x04;
    b[16] = 0x01;
    b[17] = 0x01;
    b[18] = 0x02;               // automatic image refinement
    b[19] = pg->toner_save;
    b[22] = LO(pg->margin_x);
    b[23] = HI(pg->margin_x);
    b[24] = LO(pg->margin_y);
    b[25] = HI(pg->margin_y);
    b[26] = LO(pg->line_size);
    b[27] = HI(pg->line_size);
    b[28] = LO(pg->lines);
    b[29] = HI(pg->lines);
    b[30] = LO(pg->paper_w);
    b[31] = HI(pg->paper_w);
    b[32] = LO(pg->paper_h);
    b[33] = HI(pg->paper_h);
    if (n >= 37)
        b[36] = 0x01;           // fuser mode: plain paper
    return n;
}

static int send_page_header(capt_t *c, const capt_page_t *pg)
{
    uint8_t m[128];
    size_t o = 4;
    uint8_t prm[48];
    size_t pn = capt_page_params(c, pg, prm);
    uint8_t hs[8];
    hiscoa_params_bytes(&hiscoa_params_default, hs);
    struct {
        uint16_t cmd;
        const uint8_t *p;
        size_t n;
    } parts[] = {{CAPT_BEGIN_PAGE, prm, pn}, {CAPT_HISCOA_PARAMS, hs, 8}, {CAPT_BEGIN_DATA, NULL, 0},
                 {CAPT_END_PAGE, NULL, 0}};
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        m[o++] = LO(parts[i].cmd);
        m[o++] = HI(parts[i].cmd);
        m[o++] = LO(parts[i].n + 4);
        m[o++] = HI(parts[i].n + 4);
        if (parts[i].n)
            memcpy(m + o, parts[i].p, parts[i].n);
        o += parts[i].n;
    }
    m[0] = LO(CAPT_MULTI);
    m[1] = HI(CAPT_MULTI);
    m[2] = LO(o);
    m[3] = HI(o);
    if (capt_settings.trace) {
        char hx[100];
        hex(hx, sizeof(hx), prm, pn);
        logf_(c, 3, "> d0a9 page: %s", hx);
    }
    return c->io.write(c->io.ctx, m, o) < 0 ? CAPT_ERR_IO : 0;
}

static int start_print(capt_t *c, uint16_t page)
{
    uint8_t b[2] = {LO(page), HI(page)};
    return simple(c, CAPT_START_PRINT, b, 2);
}

// The raster, in chunks of whole bands up to chunk_max (a band larger than
// that is split). After each chunk, the status: when the printer's buffer is
// full, start the engine if the page is not started yet (it then prints
// while it receives the rest) and wait for room.
static int send_raster(capt_t *c, const capt_page_t *pg, bool *started, uint16_t expect_page)
{
    size_t pos = 0;
    int band = 0;
    while (pos < pg->len) {
        size_t end = pos;
        while (band < pg->nbands && pg->band_end[band] - pos <= c->chunk_max) {
            end = pg->band_end[band];
            band++;
        }
        if (end == pos) {               // one band larger than a chunk
            end = pos + c->chunk_max;
            if (end > pg->band_end[band])
                end = pg->band_end[band];
            if (end == pg->band_end[band])
                band++;
        }
        int r = send_cmd(c, CAPT_VIDEO_DATA, pg->data + pos, end - pos);
        if (r)
            return r;
        c->bytes_sent += (uint32_t)(end - pos);
        pos = end;
        if ((r = poll(c, false)))
            return r;
        if (!c->buf_level_seen || c->st.buf_level)
            continue;
        uint32_t t0 = now(c);
        if (!*started && pos < pg->len) {
            if ((r = poll(c, true)))
                return r;
            if (c->st.page_decoding == expect_page) {
                logf_(c, 2, "page larger than the printer's memory: printing while sending");
                if ((r = start_print(c, expect_page)))
                    return r;
                *started = true;
            }
        }
        while (c->st.buf_level == 0) {
            if (expired(c, t0, 60000)) {
                logf_(c, 0, "printer buffer stayed full");
                return CAPT_ERR_PRINTER;
            }
            nap(c);
            if ((r = poll(c, false)))
                return r;
        }
    }
    return 0;
}

static bool paper_reloaded(capt_t *c)
{
    return c->model->button_is_aux20 ? (c->st.aux & 0x20) != 0 : (c->st.pap & 0x80) != 0;
}

// Out of paper: blink, wait for paper (and the printer's button), stop
// blinking. CAPT_OK = print the page again.
static int wait_paper(capt_t *c)
{
    capt_phase_t before = c->phase;
    c->phase = CAPT_PHASE_WAIT_PAPER;
    logf_(c, 1, "out of paper: load paper (and press the printer's button)");
    int r = led(c, c->model->led_blink, c->model->led_blink_len);
    if (!r)
        r = WAIT_READY(c);
    uint32_t t0 = now(c);
    bool was_out = false;               // the "paper ready" flag must go off before it counts
    while (!r) {
        if (cancelled(c)) {
            r = CAPT_ERR_CANCELED;
            break;
        }
        if (expired(c, t0, capt_settings.paper_timeout_ms)) {
            r = CAPT_ERR_PAPER;
            break;
        }
        if ((r = poll(c, true)))
            break;
        bool ready = paper_reloaded(c);
        if (ready && (was_out || c->model->button_is_aux20)) {
            logf_(c, 2, "paper reloaded");
            break;
        }
        if (!ready)
            was_out = true;
        c->io.sleep_ms(c->io.ctx, 500);
    }
    if (r != CAPT_ERR_IO && r != CAPT_ERR_TIMEOUT) {
        int r2 = led(c, c->model->led_idle, c->model->led_idle_len);
        if (!r2)
            r2 = WAIT_READY(c);
        if (!r)
            r = r2;
    }
    c->phase = before;
    return r;
}

static int engine_error(capt_t *c)
{
    switch (c->st.cond) {
    case CAPT_COND_JAM:
    case CAPT_COND_DOOR_OPEN:
    case CAPT_COND_NO_CARTRIDGE:
        return CAPT_ERR_JAM;
    default:
        return 0;
    }
}

// One attempt at a page. *paper_out = the printer ran out: print it again.
static int page_once(capt_t *c, const capt_page_t *pg, bool *paper_out)
{
    int r;
    *paper_out = false;
    c->phase = CAPT_PHASE_SENDING;
    if ((r = poll(c, true)))
        return r;
    if ((r = engine_error(c)))
        return r;
    if (c->st.basic & (B_OFFLINE | B_UNINIT))
        if ((r = clear_and_online(c)))
            return r;
    // the previous page (or a warm-up) still busy: wait
    uint32_t t0 = now(c);
    for (;;) {
        if ((r = poll(c, false)))
            return r;
        if (!(c->st.basic & B_CMD_BUSY))
            break;
        if ((r = engine_error(c)))
            return r;
        if (cancelled(c))
            return CAPT_ERR_CANCELED;
        if (expired(c, t0, capt_settings.ready_timeout_ms))
            return CAPT_ERR_PRINTER;
        nap(c);
    }
    uint16_t base = c->st.page_decoding;
    if ((r = send_page_header(c, pg)))
        return r;
    c->page_open = true;
    bool started = false;
    if ((r = send_raster(c, pg, &started, (uint16_t)(base + 1))) || (r = send_cmd(c, CAPT_BLACK_END, NULL, 0)))
        return r;
    // the printer has the whole page when it says received == decoding
    t0 = now(c);
    for (;;) {
        if ((r = poll(c, true)))
            return r;
        if (c->st.page_received == c->st.page_decoding && c->st.page_decoding != base)
            break;
        if (c->st.page_received == c->st.page_decoding && expired(c, t0, 2000))
            break;                      // counters that do not move: carry on as captdriver does
        if (expired(c, t0, 30000)) {
            logf_(c, 0, "page not taken: received %u, decoding %u", c->st.page_received, c->st.page_decoding);
            return CAPT_ERR_PRINTER;
        }
        nap(c);
    }
    uint16_t page = c->st.page_decoding;
    if (cancelled(c))
        return CAPT_ERR_CANCELED;       // never started: discarded by the job end
    if ((r = job_info(c, 2)) || (r = WAIT_READY(c)))
        return r;
    if (!started && ((r = start_print(c, page)) || (r = WAIT_READY(c))))
        return r;
    c->page_open = false;
    c->pages_sent++;
    if ((r = job_info(c, 6)))
        return r;
    c->phase = CAPT_PHASE_PRINTING;
    t0 = now(c);
    uint32_t stop_since = 0;
    bool stopped = false;
    for (;;) {
        if ((r = poll(c, true)))
            return r;
        if (capt_settings.pipeline ? c->st.page_printing >= page : c->st.page_out == page)
            return 0;
        if ((r = engine_error(c)))
            return r;
        // out of paper: the engine stopped and says not ready / print
        // rejected, for 1.5 s (not a moment between two steps: printing the
        // page again would print it twice)
        bool moving = (c->st.aux & 0x84) != 0;
        if (!moving && ((c->st.basic & B_NOT_READY) || (c->st.cnt & 0x40))) {
            if (!stopped)
                stop_since = now(c);
            stopped = true;
            if (expired(c, stop_since, 1500)) {
                c->pages_sent--;
                *paper_out = true;
                return 0;
            }
        } else {
            stopped = false;
        }
        if (expired(c, t0, capt_settings.page_timeout_ms)) {
            logf_(c, 0, "page %u not out after %d s", page, capt_settings.page_timeout_ms / 1000);
            return CAPT_ERR_PRINTER;
        }
        nap(c);
    }
}

int capt_print_page(capt_t *c, const capt_page_t *pg)
{
    if (!c->in_job)
        return CAPT_ERR_PROTOCOL;
    for (;;) {
        if (cancelled(c))
            return c->last_err = CAPT_ERR_CANCELED;
        bool paper_out;
        int r = page_once(c, pg, &paper_out);
        if (r)
            return c->last_err = r;
        if (!paper_out)
            return 0;
        if ((r = wait_paper(c)))
            return c->last_err = r;
    }
}

int capt_job_end(capt_t *c, bool ok)
{
    if (!c->in_job)
        return 0;
    c->phase = CAPT_PHASE_ENDING;
    int r = 0;
    bool link = c->last_err != CAPT_ERR_IO && c->last_err != CAPT_ERR_TIMEOUT;
    if (link)
        r = drain(c, 2);
    // started pages finish before the unit is released, also after a cancel
    if (!r && link && c->pages_sent && c->last_err != CAPT_ERR_JAM && c->last_err != CAPT_ERR_PRINTER) {
        uint32_t t0 = now(c);
        for (;;) {
            if ((r = poll(c, true)))
                break;
            if (c->st.page_completed == c->st.page_decoding)
                break;
            if (engine_error(c) || expired(c, t0, capt_settings.page_timeout_ms)) {
                logf_(c, 1, "last page not reported done (%u/%u)", c->st.page_completed, c->st.page_decoding);
                break;
            }
            nap(c);
        }
    }
    if (!r && link) {
        if (!ok || c->page_open) {
            // a page sent but not started (cancel) or refused: drop it
            if (c->page_open)
                r = simple(c, CAPT_DISCARD_DATA, NULL, 0);
            if (!r)
                r = led(c, c->model->led_idle, c->model->led_idle_len);
        }
        uint8_t j[2] = {LO(c->job_id), HI(c->job_id)};
        if (!r)
            r = job_info(c, 4);
        if (!r)
            r = simple(c, CAPT_RELEASE_UNIT, j, 2);
    }
    c->in_job = false;
    c->page_open = false;
    c->phase = CAPT_PHASE_IDLE;
    if (!link)
        r = c->last_err;
    if (r == CAPT_ERR_IO || r == CAPT_ERR_TIMEOUT || r == CAPT_ERR_PROTOCOL) {
        c->have_info = false;   // ask again next time
        return r;
    }
    logf_(c, 2, "job %u ended (%d pages)", c->job_id, c->pages_sent);
    return 0;
}
