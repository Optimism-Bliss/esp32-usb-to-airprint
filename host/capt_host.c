// Host tests of the CAPT code (components/capt): the Hi-SCoA codec, and the
// protocol engine against the emulated printer (fake_capt.c) in the cases a
// print server meets: one and several pages, a page larger than the
// printer's memory, out of paper in the middle of a job, cancel, a pulled
// cable, a lost reply, a printer that stays busy, BCD reply sizes.
//
//   ./capt_host            all tests
//   ./capt_host -v         with the engine's log
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "capt.h"
#include "fake_capt.h"
#include "hiscoa.h"
#include "printer_driver.h"

static int failures, verbose;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                      \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
            failures++;                                                        \
        }                                                                      \
    } while (0)

static uint32_t rng = 7;
static uint32_t rnd(void)
{
    rng = rng * 1103515245u + 12345u;
    return rng >> 8;
}

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

// --- Hi-SCoA round trips (our decoder; tools/captdriver_ref compares with captdriver's)

static void test_hiscoa(void)
{
    printf("hiscoa: round trips\n");
    static const unsigned sizes[][2] = {{592, 70}, {608, 70}, {595, 160}, {4, 1}, {64, 3}, {592, 1}};
    for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++)
        for (int kind = 0; kind < 6; kind++) {
            unsigned ls = sizes[s][0], lines = sizes[s][1];
            size_t n = (size_t)ls * lines;
            uint8_t *b = malloc(n), *c = malloc(hiscoa_bound(n)), *d = malloc(n);
            for (size_t i = 0; i < n; i++) {
                switch (kind) {
                case 0: b[i] = 0; break;
                case 1: b[i] = 0xff; break;
                case 2: b[i] = (uint8_t)rnd(); break;
                case 3: b[i] = (i / ls) % 2 ? b[i - ls] : (uint8_t)(rnd() % 3 ? 0 : rnd()); break;
                case 4: b[i] = (uint8_t)(0x55 << (i % 2)); break;
                default: b[i] = (uint8_t)((i % 997) < 400 ? 0 : (i * 31) >> 3); break;
                }
            }
            for (int eob = 0; eob < 2; eob++) {
                size_t cl = hiscoa_encode(b, ls, lines, eob, &hiscoa_params_default, c, hiscoa_bound(n));
                size_t used, got;
                int e = hiscoa_decode(c, cl, &used, ls, &hiscoa_params_default, d, n, &got);
                CHECK(cl && cl % 4 == 0, "size %zu", cl);
                CHECK(e == eob && got == n && !memcmp(b, d, n) && (used == cl || used + 4 == cl),
                      "ls %u lines %u kind %d: eob %d, %zu/%zu bytes", ls, lines, kind, e, got, n);
                // too small an output buffer is refused, not overrun
                size_t small = cl / 2;
                CHECK(small < 16 || hiscoa_encode(b, ls, lines, eob, &hiscoa_params_default, c, small) == 0,
                      "overflow not reported");
            }
            free(b);
            free(c);
            free(d);
        }
    // worst case stays within the bound
    size_t n = 592 * 70;
    uint8_t *b = malloc(n), *c = malloc(hiscoa_bound(n));
    for (size_t i = 0; i < n; i++)
        b[i] = (uint8_t)rnd();
    size_t cl = hiscoa_encode(b, 592, 70, 0, &hiscoa_params_default, c, hiscoa_bound(n));
    CHECK(cl > 0 && cl <= hiscoa_bound(n), "noise: %zu > bound %zu", cl, hiscoa_bound(n));
    printf("  noise band: %zu -> %zu bytes (bound %zu)\n", n, cl, hiscoa_bound(n));
    free(b);
    free(c);
}

// --- pages

typedef struct {
    capt_page_t pg;
    uint8_t *raster;
    uint8_t *data;
    uint32_t band_end[256];
    uint32_t crc;
} test_page_t;

// A4 at 600 dpi as the driver lays it out: 4736 x 6776 image at (120, 96)
static void make_page(test_page_t *t, const capt_model_t *m, int kind)
{
    memset(t, 0, sizeof(*t));
    int ls = 592, lines = 6776;
    size_t n = (size_t)ls * lines;
    t->raster = calloc(1, n);
    for (int y = 0; y < lines; y++)
        for (int x = 0; x < ls; x++) {
            uint8_t v = 0;
            if (kind == 0)          // text-ish: a few glyph rows
                v = (y % 120 < 40 && x % 50 < 30) ? (uint8_t)(rnd() & 0x7e) : 0;
            else if (kind == 1)     // noise: compresses worse than raw, larger than the printer's 2 MB
                v = (uint8_t)rnd();
            else                    // a frame and diagonal
                v = (y < 8 || y >= lines - 8 || x == 0 || x == ls - 1 || (y / 8 == x)) ? 0xff : 0;
            t->raster[(size_t)y * ls + x] = v;
        }
    t->crc = crc32_update(0, t->raster, n);
    t->data = malloc(hiscoa_bound(n) + 4096);
    size_t o = 0;
    int nb = 0;
    for (int y = 0; y < lines; y += m->band_lines) {
        int bl = lines - y < m->band_lines ? lines - y : m->band_lines;
        bool last = y + bl >= lines;
        o += hiscoa_encode(t->raster + (size_t)y * ls, (unsigned)ls, (unsigned)bl,
                           last && m->eop_marker ? HISCOA_EOB_PAGE : HISCOA_EOB_BAND, &hiscoa_params_default,
                           t->data + o, hiscoa_bound((size_t)ls * bl));
        t->band_end[nb++] = (uint32_t)o;
    }
    t->pg = (capt_page_t){.paper_code = 0x02, .margin_x = 120, .margin_y = 96, .line_size = (uint16_t)ls,
                          .lines = (uint16_t)lines, .paper_w = 4960, .paper_h = 7014, .density = 7,
                          .data = t->data, .len = o, .band_end = t->band_end, .nbands = nb};
}

static void free_page(test_page_t *t)
{
    free(t->raster);
    free(t->data);
}

// --- io wrapper: the scenario's events happen while the engine sleeps

typedef struct {
    fake_capt_t *f;
    capt_io_t inner;
    uint32_t paper_out_at;      // when the fake ran out of paper
    int refill_after_ms;        // reload + press the button this long after (0 = never)
    int refill_sheets;
    bool cancel;
    int cancel_when_paper_out;
    int unplug_at_command;      // pull the cable at the nth command
} scen_t;

static int s_write(void *ctx, const uint8_t *d, size_t n)
{
    scen_t *s = ctx;
    if (s->unplug_at_command && s->f->commands >= s->unplug_at_command)
        s->f->unplugged = true;
    return s->inner.write(s->f, d, n);
}

static int s_read(void *ctx, uint8_t *b, size_t m, int t)
{
    scen_t *s = ctx;
    return s->inner.read(s->f, b, m, t);
}

static void s_sleep(void *ctx, int ms)
{
    scen_t *s = ctx;
    s->inner.sleep_ms(s->f, ms);
    fake_capt_t *f = s->f;
    if (f->paper_out && !s->paper_out_at)
        s->paper_out_at = f->now;
    if (f->paper_out && s->cancel_when_paper_out)
        s->cancel = true;
    if (f->paper_out && s->refill_after_ms && f->now - s->paper_out_at >= (uint32_t)s->refill_after_ms) {
        f->paper = s->refill_sheets;
        fake_capt_press_button(f);
        s->refill_after_ms = 0;
    }
}

static uint32_t s_now(void *ctx) { return ((scen_t *)ctx)->f->now; }
static bool s_cancelled(void *ctx) { return ((scen_t *)ctx)->cancel; }

static void s_log(void *ctx, int level, const char *msg)
{
    if (verbose || level == 0)
        printf("    [capt %d] %s\n", level, msg);
}

static capt_io_t scen_io(scen_t *s, fake_capt_t *f)
{
    memset(s, 0, sizeof(*s));
    s->f = f;
    s->inner = fake_capt_io(f);
    return (capt_io_t){.write = s_write, .read = s_read, .sleep_ms = s_sleep, .now_ms = s_now,
                       .cancelled = s_cancelled, .log = s_log, .ctx = s};
}

// A job of `pages` copies of page t; returns the engine's result.
static int run_job(capt_t *c, test_page_t *t, int pages)
{
    int r = capt_job_begin(c);
    for (int i = 0; i < pages && !r; i++)
        r = capt_print_page(c, &t->pg);
    int e = capt_job_end(c, r == 0);
    return r ? r : e;
}

static void test_engine(const capt_model_t *m)
{
    printf("engine (%s)\n", m->name);
    test_page_t text, noise, frame;
    make_page(&text, m, 0);
    make_page(&noise, m, 1);
    make_page(&frame, m, 2);
    printf("  A4 pages: text %zu, noise %zu, frame %zu bytes compressed\n", text.pg.len, noise.pg.len, frame.pg.len);

    for (int pipeline = 0; pipeline < 2; pipeline++) {
        capt_settings.pipeline = pipeline;
        fake_capt_t f;
        fake_capt_init(&f, m);
        scen_t s;
        capt_io_t io = scen_io(&s, &f);
        capt_t c;
        capt_init(&c, &io, m);
        CHECK(capt_query(&c) == 0 && c.st.valid, "query");
        int r = run_job(&c, &text, 3);
        CHECK(r == 0, "3 pages, pipeline %d: %s", pipeline, capt_strerror(r));
        CHECK(f.pages_printed == 3, "printed %d", f.pages_printed);
        for (int i = 0; i < f.pages_printed && i < 3; i++)
            CHECK(f.page_crc[i] == text.crc, "page %d differs", i + 1);
        CHECK(!f.in_job, "unit still reserved");
        CHECK(!f.error[0], "%s", f.error);
        printf("  3 pages, pipeline %d: %.1f s (virtual), %u bytes\n", pipeline, f.now / 1000.0, (unsigned)c.bytes_sent);
        // a second job on the same link
        r = run_job(&c, &frame, 1);
        CHECK(r == 0 && f.pages_printed == 4 && f.page_crc[3] == frame.crc, "second job: %s", capt_strerror(r));
        CHECK(!f.error[0], "%s", f.error);
        fake_capt_free(&f);
    }
    capt_settings.pipeline = 0;

    {   // a page larger than the printer's memory: printed while it is sent
        fake_capt_t f;
        fake_capt_init(&f, m);
        scen_t s;
        capt_io_t io = scen_io(&s, &f);
        capt_t c;
        capt_init(&c, &io, m);
        int r = run_job(&c, &noise, 1);
        CHECK(r == 0 && f.pages_printed == 1 && f.page_crc[0] == noise.crc, "large page: %s", capt_strerror(r));
        CHECK(f.streamed, "large page not streamed");
        CHECK(f.max_ram_used <= (int)f.ram, "buffer overrun");
        CHECK(!f.error[0], "%s", f.error);
        printf("  %zu-byte page into 2 MB: streamed, peak %d KB\n", noise.pg.len, f.max_ram_used / 1024);
        fake_capt_free(&f);
    }
    {   // out of paper after 1 sheet of 3, refilled after 20 s: all 3 pages print, the 2nd once
        fake_capt_t f;
        fake_capt_init(&f, m);
        f.paper = 1;
        scen_t s;
        capt_io_t io = scen_io(&s, &f);
        s.refill_after_ms = 20000;
        s.refill_sheets = 10;
        capt_t c;
        capt_init(&c, &io, m);
        int r = run_job(&c, &text, 3);
        CHECK(r == 0 && f.pages_printed == 3, "paper out: %s, %d pages", capt_strerror(r), f.pages_printed);
        CHECK(!f.error[0], "%s", f.error);
        printf("  out of paper after page 1, refilled: %d pages\n", f.pages_printed);
        fake_capt_free(&f);
    }
    {   // out of paper, nobody refills: the job fails after the paper timeout, then a job works
        fake_capt_t f;
        fake_capt_init(&f, m);
        f.paper = 0;
        scen_t s;
        capt_io_t io = scen_io(&s, &f);
        capt_t c;
        capt_init(&c, &io, m);
        int saved = capt_settings.paper_timeout_ms;
        capt_settings.paper_timeout_ms = 30000;
        int r = run_job(&c, &text, 2);
        capt_settings.paper_timeout_ms = saved;
        CHECK(r == CAPT_ERR_PAPER && !f.in_job, "no paper: %s, unit %s", capt_strerror(r), f.in_job ? "held" : "free");
        f.paper = 5;
        fake_capt_press_button(&f);
        r = run_job(&c, &frame, 1);
        CHECK(r == 0 && f.pages_printed == 1, "after refill: %s", capt_strerror(r));
        CHECK(!f.error[0], "%s", f.error);
        fake_capt_free(&f);
    }
    {   // cancel while waiting for paper
        fake_capt_t f;
        fake_capt_init(&f, m);
        f.paper = 0;
        scen_t s;
        capt_io_t io = scen_io(&s, &f);
        s.cancel_when_paper_out = 1;
        capt_t c;
        capt_init(&c, &io, m);
        int r = run_job(&c, &text, 2);
        CHECK(r == CAPT_ERR_CANCELED && !f.in_job, "cancel: %s", capt_strerror(r));
        CHECK(!f.error[0], "%s", f.error);
        fake_capt_free(&f);
    }
    {   // cancel before the second page: one page out, the job released
        fake_capt_t f;
        fake_capt_init(&f, m);
        scen_t s;
        capt_io_t io = scen_io(&s, &f);
        capt_t c;
        capt_init(&c, &io, m);
        int r = capt_job_begin(&c);
        if (!r)
            r = capt_print_page(&c, &text.pg);
        s.cancel = true;
        int r2 = capt_print_page(&c, &text.pg);
        int e = capt_job_end(&c, false);
        CHECK(r == 0 && r2 == CAPT_ERR_CANCELED && e == 0 && f.pages_printed == 1 && !f.in_job,
              "cancel between pages: %s / %s / %s, %d pages", capt_strerror(r), capt_strerror(r2), capt_strerror(e),
              f.pages_printed);
        CHECK(!f.error[0], "%s", f.error);
        fake_capt_free(&f);
    }
    {   // cable pulled in the middle of a page, plugged again: the next job clears the stale unit
        fake_capt_t f;
        fake_capt_init(&f, m);
        scen_t s;
        capt_io_t io = scen_io(&s, &f);
        s.unplug_at_command = 30;
        capt_t c;
        capt_init(&c, &io, m);
        int r = run_job(&c, &text, 2);
        CHECK(r == CAPT_ERR_IO, "unplug: %s", capt_strerror(r));
        f.unplugged = false;
        s.unplug_at_command = 0;
        f.pg.open = f.pg.started = false;   // the printer threw the half page away
        CHECK(f.in_job, "the fake should still hold the stale job");
        r = run_job(&c, &frame, 1);
        CHECK(r == 0 && f.pages_printed >= 1 && !f.in_job, "after replug: %s", capt_strerror(r));
        CHECK(!f.error[0], "%s", f.error);
        fake_capt_free(&f);
    }
    {   // a reply lost: the job fails with a timeout, the next one prints
        fake_capt_t f;
        fake_capt_init(&f, m);
        f.drop_reply_of = 12;
        scen_t s;
        capt_io_t io = scen_io(&s, &f);
        capt_t c;
        capt_init(&c, &io, m);
        int r = run_job(&c, &text, 1);
        CHECK(r == CAPT_ERR_TIMEOUT, "lost reply: %s", capt_strerror(r));
        f.drop_reply_of = 0;
        f.error[0] = 0;             // the fake complains about the abandoned job; expected
        f.in_job = false;
        r = run_job(&c, &frame, 1);
        CHECK(r == 0 && f.pages_printed >= 1, "after a lost reply: %s", capt_strerror(r));
        fake_capt_free(&f);
    }
    {   // a printer that never finishes the page: an error after the page timeout, no hang
        fake_capt_t f;
        fake_capt_init(&f, m);
        f.stuck_busy = true;
        scen_t s;
        capt_io_t io = scen_io(&s, &f);
        capt_t c;
        capt_init(&c, &io, m);
        int r = run_job(&c, &text, 2);
        CHECK(r == CAPT_ERR_PRINTER, "stuck: %s", capt_strerror(r));
        printf("  stuck engine: gave up after %.0f s (virtual)\n", f.now / 1000.0);
        fake_capt_free(&f);
    }
    {   // reply sizes in BCD
        fake_capt_t f;
        fake_capt_init(&f, m);
        f.bcd_sizes = true;
        scen_t s;
        capt_io_t io = scen_io(&s, &f);
        capt_t c;
        capt_init(&c, &io, m);
        int r = run_job(&c, &frame, 2);
        CHECK(r == 0 && f.pages_printed == 2, "BCD sizes: %s", capt_strerror(r));
        CHECK(!f.error[0], "%s", f.error);
        fake_capt_free(&f);
    }
    free_page(&text);
    free_page(&noise);
    free_page(&frame);
}


// --- the printer drivers (components/printers/canon_capt.c) on the emulated printer

typedef struct {
    fake_capt_t *f;
    bool cancel;
    char state[16], detail[24];
} dio_t;

static int d_write(void *ctx, const uint8_t *d, size_t n) { dio_t *o = ctx; return fake_capt_io(o->f).write(o->f, d, n); }
static int d_read(void *ctx, uint8_t *b, size_t m, int t) { dio_t *o = ctx; return fake_capt_io(o->f).read(o->f, b, m, t); }
static void d_sleep(void *ctx, int ms) { dio_t *o = ctx; fake_capt_io(o->f).sleep_ms(o->f, ms); }
static uint32_t d_now(void *ctx) { return ((dio_t *)ctx)->f->now; }
static bool d_cancelled(void *ctx) { return ((dio_t *)ctx)->cancel; }
static void d_status(void *ctx, const char *state, const char *detail, const char *data)
{
    dio_t *o = ctx;
    snprintf(o->state, sizeof(o->state), "%s", state);
    snprintf(o->detail, sizeof(o->detail), "%s", detail);
}
static void d_log(void *ctx, int level, const char *msg) { s_log(ctx, level, msg); }

static printer_io_t driver_io(dio_t *o, fake_capt_t *f)
{
    memset(o, 0, sizeof(*o));
    o->f = f;
    return (printer_io_t){.write = d_write, .read = d_read, .cancelled = d_cancelled, .status = d_status,
                          .log = d_log, .sleep_ms = d_sleep, .now_ms = d_now, .ctx = o};
}

static int bit(const uint8_t *img, int ls, int x, int y)
{
    return (img[(size_t)y * ls + x / 8] >> (7 - x % 8)) & 1;
}

// A page at dpi: white, a black square at sheet (1000..1999, 2000..2999)
// (600 dpi coordinates) and a 50 % gray square at (3000..3599, 2000..2599).
// The printed image must have them where the image area puts them: at
// (x - 120, y - 96).
static void print_squares(const printer_driver_t *d, int dpi, int bpp)
{
    fake_capt_t f;
    fake_capt_init(&f, d->model);
    dio_t o;
    printer_io_t io = driver_io(&o, &f);
    int s = 600 / dpi, W = 4961 / s, H = 7016 / s;
    void *job = d->job_begin(d, &io, dpi, 0);
    CHECK(job, "%s: job", d->id);
    if (!job)
        return;
    uint8_t *line = malloc((size_t)W * bpp);
    int r = d->page_begin(job, W, H, bpp);
    for (int y = 0; y < H && !r; y++) {
        int sy = y * s;
        bool any = false;
        for (int x = 0; x < W; x++) {
            int sx = x * s;
            uint8_t g = 255;
            if (sx >= 1000 && sx < 2000 && sy >= 2000 && sy < 3000)
                g = 0;
            if (sx >= 3000 && sx < 3600 && sy >= 2000 && sy < 2600)
                g = 128;
            any |= g != 255;
            for (int k = 0; k < bpp; k++)
                line[x * bpp + k] = g;
        }
        r = d->page_line(job, y, any ? line : (y % 2 ? NULL : line));
    }
    if (!r)
        r = d->page_end(job);
    char stats[200];
    d->job_stats(job, stats, sizeof(stats));
    int e = d->job_end(job);
    free(line);
    CHECK(r == 0 && e == 0 && f.pages_printed == 1, "%s %d dpi: %d/%d, %d pages", d->id, dpi, r, e, f.pages_printed);
    CHECK(!f.error[0], "%s", f.error);
    if (f.pages_printed != 1 || !f.last_page)
        return;
    int ls = f.line_size[0], lines = f.lines[0];
    CHECK(ls == 592 && lines == 6776, "A4 image %d x %d", ls * 8, lines);
    long black = 0, gray = 0, stray = 0;
    for (int y = 0; y < lines; y++)
        for (int x = 0; x < ls * 8; x++) {
            int sx = x + 120, sy = y + 96, b = bit(f.last_page, ls, x, y);
            if (sx >= 1000 && sx < 2000 && sy >= 2000 && sy < 3000)
                black += b;
            else if (sx >= 3000 && sx < 3600 && sy >= 2000 && sy < 2600)
                gray += b;
            else
                stray += b;
        }
    CHECK(black == 1000 * 1000, "%s %d dpi: black square %ld dots", d->id, dpi, black);
    CHECK(gray > 600 * 600 * 45 / 100 && gray < 600 * 600 * 55 / 100, "%s: 50%% gray %ld dots", d->id, gray);
    CHECK(stray == 0, "%s %d dpi: %ld dots outside the squares", d->id, dpi, stray);
    printf("  %d dpi %s: squares in place (gray %.1f %%); %s\n", dpi, bpp == 3 ? "RGB" : "gray",
           gray * 100.0 / (600 * 600), stats);
    fake_capt_free(&f);
}

static void test_driver(const printer_driver_t *d)
{
    printf("driver %s\n", d->id);
    print_squares(d, 600, 1);
    print_squares(d, 300, 1);
    print_squares(d, 300, 3);
    {   // status poll, then the test page
        fake_capt_t f;
        fake_capt_init(&f, d->model);
        dio_t o;
        printer_io_t io = driver_io(&o, &f);
        CHECK(d->poll(d, &io) == 0 && !strcmp(o.state, "idle"), "%s: poll: %s", d->id, o.state);
        int r = d->test_page(d, &io, 600, "HOST TEST");
        CHECK(r == 0 && f.pages_printed == 1 && !f.error[0], "%s: test page %d, %d pages %s", d->id, r,
              f.pages_printed, f.error);
        if (f.last_page) {
            FILE *pbm = fopen("out/capt-testpage.pbm", "wb");
            if (pbm) {
                fprintf(pbm, "P4\n%d %d\n", f.line_size[0] * 8, f.lines[0]);
                fwrite(f.last_page, 1, (size_t)f.line_size[0] * f.lines[0], pbm);
                fclose(pbm);
            }
        }
        // out of paper while idle: the status says so
        f.paper = 0;
        r = d->test_page(d, &io, 600, "NO PAPER");
        airprint_status_t st = {0};
        d->status(o.state, o.detail, "", &st);
        printf("  test page with no paper: %s; IPP: %s \"%s\"\n", r ? "failed" : "printed",
               st.nreasons ? st.reasons[0] : "-", st.message);
        fake_capt_free(&f);
    }
    {   // a cancel between pages: the job ends cleanly, the unit is free
        fake_capt_t f;
        fake_capt_init(&f, d->model);
        dio_t o;
        printer_io_t io = driver_io(&o, &f);
        void *job = d->job_begin(d, &io, 300, 1);
        int r = 0;
        for (int p = 0; p < 2 && !r; p++) {
            r = d->page_begin(job, 2550, 3300, 1);
            for (int y = 0; y < 3300 && !r; y++)
                r = d->page_line(job, y, NULL);
            if (!r)
                r = d->page_end(job);
            o.cancel = true;
        }
        int e = d->job_end(job);
        CHECK(f.pages_printed == 1 && !f.in_job && e == 0 && !f.error[0], "%s: cancel: %d pages, %s", d->id,
              f.pages_printed, f.error);
        CHECK(f.line_size[0] == 608 && f.lines[0] == 6360, "Letter image %d x %d", f.line_size[0] * 8, f.lines[0]);
        fake_capt_free(&f);
    }
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    capt_settings.trace = verbose;
    test_hiscoa();
    test_engine(&capt_model_lbp2900);
    test_engine(&capt_model_lbp3010);
    test_driver(printer_driver_find("canon-lbp2900"));
    test_driver(printer_driver_find("canon-lbp3050"));
    printf(failures ? "capt_host: %d FAILURES\n" : "capt_host: all tests passed\n", failures);
    return failures != 0;
}
