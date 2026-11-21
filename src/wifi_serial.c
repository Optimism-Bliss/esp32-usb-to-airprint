// WiFi setup over the COM port (UART0, the console, 115200 baud), for
// tools/set_wifi.py: a line "WIFI <ssid in hex> <password in hex>" saves the
// network (the same NVS keys as the setup page) and restarts the board.
// Answers "WIFI setup saved", "WIFI setup rejected" (bad line or lengths:
// SSID 1-32 bytes, password empty or 8-64) or "WIFI setup failed" (NVS).
// Neither the SSID nor the password is logged.
#include "wifi_serial.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi.h"

static const char *TAG = "wifi_serial";

#define UART UART_NUM_0
#define SETUP_LINE_MAX 320    // "WIFI " + 64 hex + " " + 128 hex

static int unhex(const char *s, size_t n, uint8_t *out, size_t max)
{
    if (n % 2 || n / 2 > max)
        return -1;
    for (size_t i = 0; i < n; i += 2) {
        if (!isxdigit((unsigned char)s[i]) || !isxdigit((unsigned char)s[i + 1]))
            return -1;
        char b[3] = {s[i], s[i + 1], 0};
        out[i / 2] = (uint8_t)strtol(b, NULL, 16);
    }
    return (int)(n / 2);
}

static void answer(const char *what)
{
    // straight to the console (not through the log, whose level may hide it)
    printf("\n%s\n", what);
    fflush(stdout);
}

static void handle_line(char *line)
{
    // WIFI <hex> [<hex>]   (an empty password leaves the second field empty)
    char *p = line + 4;
    while (*p == ' ')
        p++;
    char *ssid_hex = p;
    while (*p && *p != ' ')
        p++;
    size_t ssid_n = (size_t)(p - ssid_hex);
    while (*p == ' ')
        p++;
    char *pw_hex = p;
    while (*p && *p != ' ')
        p++;
    size_t pw_n = (size_t)(p - pw_hex);
    uint8_t ssid[33] = {0}, pw[65] = {0};
    int sl = unhex(ssid_hex, ssid_n, ssid, 32), pl = unhex(pw_hex, pw_n, pw, 64);
    if (sl < 1 || pl < 0 || (pl > 0 && pl < 8) || memchr(ssid, 0, (size_t)sl) || memchr(pw, 0, (size_t)pl)) {
        ESP_LOGW(TAG, "WiFi setup line rejected");
        answer("WIFI setup rejected");
        return;
    }
    if (wifi_store_network((const char *)ssid, (const char *)pw) != ESP_OK) {
        answer("WIFI setup failed");
        return;
    }
    ESP_LOGI(TAG, "WiFi network saved from the COM port, restarting");
    answer("WIFI setup saved");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void serial_task(void *arg)
{
    static char line[SETUP_LINE_MAX + 1];
    size_t n = 0;
    bool overflow = false;
    for (;;) {
        uint8_t c;
        if (uart_read_bytes(UART, &c, 1, portMAX_DELAY) != 1)
            continue;
        if (c == '\r' || c == '\n') {
            line[n] = 0;
            if (!overflow && n >= 4 && !strncmp(line, "WIFI", 4) && (line[4] == ' ' || !line[4]))
                handle_line(line);
            else if (overflow && !strncmp(line, "WIFI", 4))
                answer("WIFI setup rejected");
            n = 0;
            overflow = false;
        } else if (n < SETUP_LINE_MAX) {
            line[n++] = (char)c;
        } else {
            overflow = true;
        }
    }
}

void wifi_serial_start(void)
{
    // the console's UART: install the driver for reading (output is unchanged)
    if (!uart_is_driver_installed(UART) && uart_driver_install(UART, 1024, 0, 0, NULL, 0) != ESP_OK) {
        ESP_LOGW(TAG, "cannot read the COM port: WiFi setup over serial disabled");
        return;
    }
    xTaskCreate(serial_task, "wifi_serial", 3072, NULL, 4, NULL);
}
