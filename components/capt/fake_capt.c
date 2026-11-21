// Emulated CAPT printer for the host tests (fake_capt.h).
#include "fake_capt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hiscoa.h"

#define LE16(p) ((uint16_t)((p)[0] | (p)[1] << 8))

static uint32_t crc32_update(uint32_t crc, const uint8_t *d, size_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *d++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1));
    }
    return ~crc;
}

static void violation(fake_capt_t *f, const char *fmt, int a, int b)
{
    if (f->error[0])
        return;
    snprintf(f->error, sizeof(f->error), fmt, a, b);
    fprintf(stderr, "fake_capt: %s\n", f->error);
}

void fake_capt_init(fake_capt_t *f, const capt_model_t *model)
{
    memset(f, 0, sizeof(*f));
    f->model = model;
    f->paper = -1;
    f->ram = 2u << 20;
    f->print_ms = 6000;
    f->online = false;
}

void fake_capt_free(fake_capt_t *f)
{
    free(f->pg.data);
    free(f->last_page);
    f->pg.data = f->last_page = NULL;
}

void fake_capt_press_button(fake_capt_t *f)
{
    if (f->paper != 0)
        f->paper_out = false;
    f->button = true;
    f->input_changed = true;
    f->ext_changed = true;
}

// --- the engine

static bool engine_busy(fake_capt_t *f)
{
    return f->pg.started && f->completed != f->decoding;
}

static void decode_page(fake_capt_t *f)
{
    int ls = f->pg.line_size, lines = f->pg.lines;
    size_t total = (size_t)ls * lines;
    uint8_t *img = calloc(1, total + 1);
    size_t pos = 0, o = 0;
    int bands = 0;
    hiscoa_params_t prm = hiscoa_params_default;
    while (o < total) {
        size_t used, got;
        int eob = hiscoa_decode(f->pg.data + pos, f->pg.len - pos, &used, (unsigned)ls, &prm, img + o, total - o, &got);
        if (eob < 0) {
            violation(f, "page %d: band %d does not decode", f->decoding, bands);
            free(img);
            return;
        }
        if (got % (size_t)ls) {
            violation(f, "band %d is not whole lines (%d bytes)", bands, (int)got);
            free(img);
            return;
        }
        o += got;
        pos += used;
        bands++;
        int want = o == total && f->model->eop_marker ? HISCOA_EOB_PAGE : HISCOA_EOB_BAND;
        if (eob != want)
            violation(f, "band %d ends with marker %d", bands, eob);
    }
    // only padding words may follow
    for (; pos < f->pg.len; pos++)
        if (f->pg.data[pos] != 0xbc) {
            violation(f, "%d bytes of raster after the page's %d lines", (int)(f->pg.len - pos), lines);
            break;
        }
    free(f->last_page);
    f->last_page = img;
    f->pg.decoded_ok = true;
}

static void update(fake_capt_t *f)
{
    if (!f->pg.started || f->stuck_busy)
        return;
    uint32_t t = f->now - f->pg.start_at;
    if (f->paper == 0 && f->completed != f->decoding) {
        if (t >= 800 && !f->paper_out) {
            // the pick roller found nothing: the page is refused
            f->paper_out = true;
            f->online = false;
            f->ext_changed = f->input_changed = true;
            f->pg.started = false;
            f->pg.open = false;
            f->decoding = f->completed;
            f->received = f->completed;
        }
        return;
    }
    // the engine uses the data during the first 70% of the page
    if (t >= 300) {
        uint32_t span = (uint32_t)f->print_ms * 7 / 10;
        size_t need = t >= span ? f->pg.len : (size_t)((double)f->pg.len * (t - 300) / (span - 300));
        if (!f->pg.data_ended && need > f->pg.len)
            violation(f, "engine starved of raster on page %d", f->decoding, 0);
        f->pg.consumed = need < f->pg.len ? need : f->pg.len;
        if (f->printing != f->decoding) {
            f->printing = f->decoding;
            f->ext_changed = true;
        }
    }
    if (t >= (uint32_t)f->print_ms && f->out != f->decoding) {
        f->out = f->decoding;
        f->ext_changed = true;
    }
    if (t >= (uint32_t)f->print_ms + 200 && f->completed != f->decoding) {
        f->completed = f->decoding;
        f->ext_changed = true;
        if (f->pg.decoded_ok && f->pages_printed < FAKE_CAPT_MAX_PAGES) {
            int i = f->pages_printed;
            f->page_crc[i] = crc32_update(0, f->last_page, (size_t)f->pg.line_size * f->pg.lines);
            f->line_size[i] = f->pg.line_size;
            f->lines[i] = f->pg.lines;
        }
        f->pages_printed++;
        if (f->paper > 0)
            f->paper--;
        f->pg.open = f->pg.started = false;
    }
}

static size_t ram_used(fake_capt_t *f)
{
    return f->pg.open ? f->pg.len - f->pg.consumed : 0;
}

static uint8_t buf_level(fake_capt_t *f)
{
    size_t used = ram_used(f);
    size_t free_ = used >= f->ram ? 0 : f->ram - used;
    return (uint8_t)(free_ * 15 / f->ram);
}

static uint16_t basic_word(fake_capt_t *f)
{
    uint16_t b = 0;
    if (!f->online)
        b |= 0x0010;
    if (f->paper_out)
        b |= 0x0002;
    if (engine_busy(f))
        b |= 0x0004;
    if (f->ext_changed)
        b |= 0x0100;
    if (f->input_changed)
        b |= 0x0200;
    return b;
}

// --- replies

static void set_reply(fake_capt_t *f, uint16_t cmd, const uint8_t *p, size_t n)
{
    if (f->drop_reply_of && f->commands == f->drop_reply_of)
        return;
    size_t total = n + 4;
    unsigned sz = (unsigned)total;
    if (f->bcd_sizes)
        sz = (unsigned)((total / 1000 % 10) << 12 | (total / 100 % 10) << 8 | (total / 10 % 10) << 4 | (total % 10));
    f->reply[0] = (uint8_t)cmd;
    f->reply[1] = (uint8_t)(cmd >> 8);
    f->reply[2] = (uint8_t)sz;
    f->reply[3] = (uint8_t)(sz >> 8);
    memcpy(f->reply + 4, p, n);
    f->reply_len = total;
    f->reply_pos = 0;
}

static void reply_ok(fake_capt_t *f, uint16_t cmd)
{
    static const uint8_t z[2];
    set_reply(f, cmd, z, 2);
}

static void status_reply(fake_capt_t *f, uint16_t cmd)
{
    uint8_t p[52] = {0};
    uint16_t b = basic_word(f);
    p[0] = (uint8_t)b;
    p[1] = (uint8_t)(b >> 8);
    p[4] = buf_level(f);
    if (cmd == CAPT_GET_BASIC_STATUS) {
        set_reply(f, cmd, p, 8);
        return;
    }
    bool moving = f->pg.started && f->printing == f->decoding && f->out != f->decoding;
    p[2] = 0x00;
    p[3] = 0x0f;
    p[8] = moving ? 0x84 : 0;
    if (f->button && f->model->button_is_aux20)
        p[8] |= 0x20;
    p[9] = f->paper_out ? 0x40 : 0;
    p[10] = f->paper_out ? 0x00 : 0x80;
    uint16_t c[] = {f->decoding, f->printing, f->out, f->completed};
    for (int i = 0; i < 4; i++) {
        p[14 + 2 * i] = (uint8_t)c[i];
        p[15 + 2 * i] = (uint8_t)(c[i] >> 8);
    }
    p[34] = (uint8_t)f->received;
    p[35] = (uint8_t)(f->received >> 8);
    f->ext_changed = false;
    set_reply(f, cmd, p, sizeof(p));
}

static void begin_page(fake_capt_t *f, const uint8_t *p, size_t n)
{
    if (!f->in_job)
        violation(f, "page parameters outside a job", 0, 0);
    if (!f->online)
        violation(f, "page parameters while offline", 0, 0);
    if (f->pg.open)
        violation(f, "page %d announced while page %d is not printed", f->decoding + 1, f->decoding);
    if (n != f->model->page_params_len)
        violation(f, "page parameters of %d bytes, expected %d", (int)n, f->model->page_params_len);
    if (n < 34)
        return;
    memcpy(f->pg.params, p, n < sizeof(f->pg.params) ? n : sizeof(f->pg.params));
    int ls = LE16(p + 26), lines = LE16(p + 28), pw = LE16(p + 30), ph = LE16(p + 32);
    int mx = LE16(p + 22), my = LE16(p + 24);
    if (LE16(p + 2) != f->model->model_const)
        violation(f, "model constant %04x", LE16(p + 2), 0);
    if (ls <= 0 || ls * 8 + mx > pw + 64 || lines <= 0 || lines + my > ph)
        violation(f, "image %d x %d does not fit the sheet", ls * 8, lines);
    f->pg.open = true;
    f->pg.complete = f->pg.started = f->pg.data_ended = f->pg.decoded_ok = false;
    f->pg.line_size = ls;
    f->pg.lines = lines;
    f->pg.len = f->pg.consumed = 0;
    f->decoding++;
    f->ext_changed = true;
}

static void video_data(fake_capt_t *f, const uint8_t *p, size_t n)
{
    if (!f->pg.open || f->pg.data_ended) {
        violation(f, "raster outside a page", 0, 0);
        return;
    }
    if (ram_used(f) + n > f->ram)
        violation(f, "raster while the buffer is full (%d + %d bytes)", (int)ram_used(f), (int)n);
    if (f->pg.len + n > f->pg.cap) {
        f->pg.cap = (f->pg.len + n) * 2;
        f->pg.data = realloc(f->pg.data, f->pg.cap);
    }
    memcpy(f->pg.data + f->pg.len, p, n);
    f->pg.len += n;
    if ((int)ram_used(f) > f->max_ram_used)
        f->max_ram_used = (int)ram_used(f);
}

static void handle(fake_capt_t *f, uint16_t cmd, const uint8_t *p, size_t n)
{
    f->commands++;
    switch (cmd) {
    case CAPT_GET_PRINTER_INFO: {
        uint8_t r[52] = {0x00, 0x0b, 0x30, 0x2a, 0x01, 0x01, 0xf0, 0xff, 0x40, 0x00, 0x04};
        set_reply(f, cmd, r, sizeof(r));
        break;
    }
    case CAPT_GET_BASIC_STATUS:
    case CAPT_GET_EXT_STATUS:
        status_reply(f, cmd);
        break;
    case CAPT_GET_INPUT_STATUS: {
        uint8_t r[18] = {0x00, 0x10, (uint8_t)(f->paper_out ? 0x80 : 0xd0), 0, 0, 0x02};
        f->input_changed = false;
        set_reply(f, cmd, r, sizeof(r));
        break;
    }
    case CAPT_RESERVE_UNIT: {
        uint8_t r[4] = {0};
        if (f->in_job) {
            r[0] = 0x80;            // a job left over: GoOnline clears it
            f->stale_job = true;
        } else {
            f->in_job = true;
            f->job_id++;
            r[2] = (uint8_t)f->job_id;
            r[3] = (uint8_t)(f->job_id >> 8);
        }
        set_reply(f, cmd, r, 4);
        break;
    }
    case CAPT_RELEASE_UNIT:
        if (!f->in_job || n < 2 || LE16(p) != f->job_id)
            violation(f, "release of job %d (reserved: %d)", n >= 2 ? LE16(p) : -1, f->in_job ? f->job_id : -1);
        if (engine_busy(f))
            violation(f, "release while page %d prints", f->decoding, 0);
        f->in_job = false;
        reply_ok(f, cmd);
        break;
    case CAPT_GO_ONLINE:
        if (n < 4 || LE16(p) != 0xdbee)
            violation(f, "go online without the magic", 0, 0);
        if (f->paper != 0)
            f->paper_out = false;
        if (!f->paper_out)
            f->online = true;
        f->decoding = f->printing = f->out = f->completed = f->received = 0;
        f->ext_changed = true;
        if (f->stale_job)
            f->in_job = f->stale_job = false;
        reply_ok(f, cmd);
        break;
    case CAPT_DISCARD_DATA:
        if (f->pg.open && !f->pg.started) {
            f->pg.open = false;
            f->decoding = f->completed;
            f->received = f->completed;
        }
        reply_ok(f, cmd);
        break;
    case CAPT_START_PRINT:
        if (!f->pg.open || n < 2 || LE16(p) != f->decoding)
            violation(f, "start of page %d, decoding %d", n >= 2 ? LE16(p) : -1, f->decoding);
        else if (!f->online)
            violation(f, "start while offline", 0, 0);
        else {
            f->pg.started = true;
            f->pg.start_at = f->now;
            if (!f->pg.data_ended)
                f->streamed = true;
        }
        reply_ok(f, cmd);
        break;
    case CAPT_SET_LED:
        if (f->model->led_idle && n == f->model->led_idle_len && !memcmp(p, f->model->led_idle, n))
            f->button = false;
        reply_ok(f, cmd);
        break;
    case CAPT_START_0:
    case CAPT_SET_JOB_INFO:
    case CAPT_CLEAR_ERROR:
    case CAPT_CLEAR_MISPRINT:
    case CAPT_GO_OFFLINE:
        reply_ok(f, cmd);
        break;
    case CAPT_MULTI: {
        size_t o = 0;
        while (o + 4 <= n) {
            uint16_t sc = LE16(p + o), ss = LE16(p + o + 2);
            if (ss < 4 || o + ss > n) {
                violation(f, "bad 0xd0a9 content", 0, 0);
                break;
            }
            if (sc == CAPT_BEGIN_PAGE)
                begin_page(f, p + o + 4, ss - 4u);
            else if (sc == CAPT_HISCOA_PARAMS && ss == 12) {
                uint8_t want[8];
                hiscoa_params_bytes(&hiscoa_params_default, want);
                if (memcmp(want, p + o + 4, 8))
                    violation(f, "unexpected Hi-SCoA parameters", 0, 0);
            }
            o += ss;
        }
        break;
    }
    case CAPT_VIDEO_DATA:
        video_data(f, p, n);
        break;
    case CAPT_BLACK_END:
        if (!f->pg.open)
            violation(f, "end of raster outside a page", 0, 0);
        else {
            f->pg.data_ended = true;
            decode_page(f);
            f->received = f->decoding;
            f->ext_changed = true;
        }
        break;
    default:
        violation(f, "unknown command %04x", cmd, 0);
        reply_ok(f, cmd);
    }
}

// --- transport

static int io_write(void *ctx, const uint8_t *d, size_t n)
{
    fake_capt_t *f = ctx;
    if (f->unplugged)
        return -1;
    while (n) {
        size_t k = sizeof(f->in) - f->in_len;
        if (k > n)
            k = n;
        memcpy(f->in + f->in_len, d, k);
        f->in_len += k;
        d += k;
        n -= k;
        // whole commands
        for (;;) {
            if (f->in_len < 4)
                break;
            uint16_t cmd = LE16(f->in), sz = LE16(f->in + 2);
            if (sz < 4) {
                violation(f, "command %04x of size %d", cmd, sz);
                f->in_len = 0;
                break;
            }
            if (f->in_len < sz)
                break;
            if (f->reply_len && f->reply_pos < f->reply_len)
                violation(f, "command %04x sent before the reply to %04x was read", cmd, LE16(f->reply));
            f->reply_len = 0;
            f->now += 1;
            update(f);
            handle(f, cmd, f->in + 4, sz - 4u);
            memmove(f->in, f->in + sz, f->in_len - sz);
            f->in_len -= sz;
        }
        if (f->in_len == sizeof(f->in)) {
            violation(f, "command larger than %d bytes", (int)sizeof(f->in), 0);
            f->in_len = 0;
        }
    }
    return 0;
}

static int io_read(void *ctx, uint8_t *buf, size_t max, int timeout_ms)
{
    fake_capt_t *f = ctx;
    if (f->unplugged)
        return -1;
    if (max == 0)
        return 0;
    if (!f->reply_len || f->reply_pos >= f->reply_len) {
        f->now += (uint32_t)timeout_ms;
        update(f);
        return 0;
    }
    // the first 6 bytes come alone, then the rest
    size_t k = f->reply_pos == 0 ? (f->reply_len < 6 ? f->reply_len : 6) : f->reply_len - f->reply_pos;
    if (k > max)
        return -1;      // the host's transfer is too small: data lost
    memcpy(buf, f->reply + f->reply_pos, k);
    f->reply_pos += k;
    return (int)k;
}

static void io_sleep(void *ctx, int ms)
{
    fake_capt_t *f = ctx;
    f->now += (uint32_t)ms;
    update(f);
}

static uint32_t io_now(void *ctx)
{
    return ((fake_capt_t *)ctx)->now;
}

capt_io_t fake_capt_io(fake_capt_t *f)
{
    return (capt_io_t){.write = io_write, .read = io_read, .sleep_ms = io_sleep, .now_ms = io_now, .ctx = f};
}
