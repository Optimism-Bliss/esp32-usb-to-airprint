#include "print_job.h"

#include <stdio.h>
#include <string.h>

#include "canon_bjl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "shared_buf.h"
#include "usb_printer.h"

static const char *TAG = "print_job";

#if CONFIG_PRINTER_RES_300
#define TEST_PAGE_DPI 300
#else
#define TEST_PAGE_DPI 600
#endif

static const printer_driver_t *configured_driver(void)
{
    static const printer_driver_t *d;
    if (!d) {
        d = printer_driver_find(CONFIG_PRINTER_DRIVER);
        if (!d) {
            ESP_LOGE(TAG, "no printer driver \"%s\", using \"%s\"", CONFIG_PRINTER_DRIVER, printer_drivers[0]->id);
            d = printer_drivers[0];
        }
    }
    return d;
}

// the driver of the last printer plugged in, NULL if it had none
static const printer_driver_t *volatile s_attached;

const printer_driver_t *print_job_driver(void)
{
    const printer_driver_t *d = s_attached;
    return d ? d : configured_driver();
}

bool print_job_set_printer(const usb_printer_info_t *info)
{
    const printer_driver_t *before = print_job_driver();
    const printer_driver_t *d = printer_driver_match(info->vid, info->pid, info->device_id);
    s_attached = d;
    usb_printer_set_raw(print_job_driver()->raw_back_channel);
    if (d)
        ESP_LOGI(TAG, "printer %04x:%04x: driver %s (%s)", info->vid, info->pid, d->id, d->make_model);
    else
        ESP_LOGW(TAG, "printer %04x:%04x \"%s\" is not a model the firmware knows: using the \"%s\" driver anyway",
                 info->vid, info->pid, info->product, print_job_driver()->id);
    return print_job_driver() != before;
}

int print_job_default_dpi(void)
{
    return printer_driver_has_dpi(print_job_driver(), TEST_PAGE_DPI) ? TEST_PAGE_DPI
                                                                     : print_job_driver()->resolutions[0];
}

// Canon's driver waits for each IVEC response; Gutenprint does not wait at
// all. Wait a bit, but carry on either way. The end of the job is answered
// once the page is out (which keeps the job "processing" until then), by a
// printer that answered the start of the job: one that did not (a model whose
// back channel we do not know) is not waited for for minutes.
int print_job_sync(const char *what)
{
    static bool s_start_answered;
    if (usb_printer_flush() != ESP_OK)
        return -1;
    bool end = strcmp(what, "EndJob") == 0;
    uint32_t timeout = !end ? 5000 : s_start_answered ? 180000 : 5000;
    esp_err_t err = usb_printer_wait_response(what, timeout);
    if (strcmp(what, "StartJob") == 0)
        s_start_answered = err == ESP_OK;
    if (err == ESP_OK)
        ESP_LOGI(TAG, "%s acknowledged", what);
    else if (err == ESP_ERR_TIMEOUT)
        ESP_LOGW(TAG, "no %s reply within %lu ms, continuing", what, (unsigned long)timeout);
    else if (err != ESP_ERR_NOT_SUPPORTED)
        return -1;
    return 0;
}

typedef struct {
    bool dry_run;
    uint32_t crc, bytes;
} test_out_t;

static volatile bool s_test_cancel;

static int test_write_raw(void *ctx, const uint8_t *data, size_t len)
{
    return usb_printer_write(data, len) == ESP_OK ? 0 : -1;
}

static int test_write(void *ctx, const uint8_t *data, size_t len)
{
    test_out_t *o = ctx;
    o->bytes += len;
    if (o->dry_run) {
        o->crc = bjl_crc32(o->crc, data, len);
        return 0;
    }
    return usb_printer_write(data, len) == ESP_OK ? 0 : -1;
}

static int test_sync(void *ctx, const char *what)
{
    return print_job_sync(what);
}

// --- the hooks of request/reply printers (CAPT), for the test page, the
// status poll and (through airprint_esp.c) the IPP jobs

int print_job_read(void *ctx, uint8_t *buf, size_t max, int timeout_ms)
{
    return usb_printer_read(buf, max, (uint32_t)timeout_ms);
}

static uint8_t *s_page_buf;

uint8_t *print_job_page_buffer(void *ctx, size_t *size)
{
    if (!s_page_buf)
        s_page_buf = sbuf_acquire(SBUF_PRINT, size);
    else
        *size = sbuf_size();
    if (!s_page_buf)
        ESP_LOGW(TAG, "no page buffer (PSRAM): the driver allocates");
    return s_page_buf;
}

void print_job_release_page_buffer(void)
{
    if (s_page_buf) {
        sbuf_release(SBUF_PRINT, 0);
        s_page_buf = NULL;
    }
}

void print_job_status(void *ctx, const char *state, const char *detail, const char *data)
{
    usb_printer_set_status(state, detail, data);
}

void print_job_log(void *ctx, int level, const char *msg)
{
    switch (level) {
    case 0: ESP_LOGE("capt", "%s", msg); break;
    case 1: ESP_LOGW("capt", "%s", msg); break;
    case 2: ESP_LOGI("capt", "%s", msg); break;
    default: ESP_LOGI("capt", "%s", msg); break;    // command traces: only with capt_settings.trace
    }
}

static bool test_cancelled(void *ctx)
{
    return s_test_cancel;
}

void print_job_cancel_test_page(void)
{
    s_test_cancel = true;
}

static volatile bool s_polling;

bool print_job_polling(void)
{
    return s_polling;
}

// Status of a printer whose driver asks for it (CAPT), every 3 s while no
// job holds the printer.
static void poll_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        const printer_driver_t *d = print_job_driver();
        if (!d->poll || !usb_printer_connected() || usb_printer_job_active())
            continue;
        s_polling = true;
        if (usb_printer_job_begin() != ESP_OK) {
            s_polling = false;
            continue;
        }
        printer_io_t io = {.write = test_write_raw, .read = print_job_read, .status = print_job_status,
                           .log = print_job_log};
        int r = d->poll(d, &io);
        usb_printer_job_end();
        s_polling = false;
        if (r)
            ESP_LOGD(TAG, "status poll failed (%d)", r);
    }
}

void print_job_start_poll(void)
{
    xTaskCreate(poll_task, "printer_poll", 4096, NULL, 3, NULL);
}

static void test_progress(void *ctx, int line, int lines)
{
    vTaskDelay(1);      // let the idle task run (task watchdog)
    if (line % 1024 == 0)
        ESP_LOGI(TAG, "line %d/%d, %lu bytes", line, lines, (unsigned long)((test_out_t *)ctx)->bytes);
}

// "MDL:MG2500 series;" from the IEEE 1284 ID, else the USB product string
static void printer_label(char *out, size_t n)
{
    usb_printer_info_t info;
    if (!usb_printer_get_info(&info)) {
        snprintf(out, n, "PRINTER: ?");
        return;
    }
    const char *mdl = strstr(info.device_id, "MDL:");
    char model[sizeof(info.product)] = "";
    if (mdl) {
        mdl += 4;
        size_t i = 0;
        while (mdl[i] && mdl[i] != ';' && i < sizeof(model) - 1) {
            model[i] = mdl[i];
            i++;
        }
        model[i] = 0;
    } else {
        snprintf(model, sizeof(model), "%s", info.product);
    }
    snprintf(out, n, "PRINTER: %s %04X:%04X", model, info.vid, info.pid);
}

esp_err_t print_job_test_page(bool dry_run, int dpi, print_stats_t *stats)
{
    const printer_driver_t *d = print_job_driver();
    int64_t t0 = esp_timer_get_time();
    memset(stats, 0, sizeof(*stats));
    if (!d->test_page)
        return ESP_ERR_NOT_SUPPORTED;
    esp_err_t err;
    if (!dry_run && (err = usb_printer_job_begin()) != ESP_OK) {
        ESP_LOGE(TAG, "printer not ready: %s", esp_err_to_name(err));
        return err;
    }
    char label[64];
    if (dry_run)
        snprintf(label, sizeof(label), "DRY RUN");      // = TESTPAGE_DRYRUN_INFO, as the host harness
    else
        printer_label(label, sizeof(label));
    ESP_LOGI(TAG, "%s: %s test page at %d dpi, free heap %lu", dry_run ? "dry run" : "printing", d->id, dpi,
             (unsigned long)esp_get_free_heap_size());
    test_out_t out = {.dry_run = dry_run};
    printer_io_t io = {.write = test_write, .sync = dry_run ? NULL : test_sync, .progress = test_progress, .ctx = &out};
    if (!dry_run && d->raw_back_channel) {
        s_test_cancel = false;
        io.read = print_job_read;
        io.cancelled = test_cancelled;
        io.page_buffer = print_job_page_buffer;
        io.status = print_job_status;
        io.log = print_job_log;
    }
    err = d->test_page(d, &io, dpi, label) == 0 ? ESP_OK : ESP_FAIL;
    if (!dry_run) {
        print_job_release_page_buffer();
        if (err != ESP_OK && usb_printer_connected() && !d->raw_back_channel)
            ESP_LOGW(TAG, "job aborted, resetting printer");
        usb_printer_job_end();
        if (err != ESP_OK && usb_printer_connected() && !d->raw_back_channel)
            usb_printer_soft_reset();
    }
    stats->bytes = out.bytes;
    stats->crc32 = out.crc;
    stats->ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    return err;
}
