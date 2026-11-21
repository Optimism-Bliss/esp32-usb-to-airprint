// WiFi: a station on the network saved from the setup page (NVS), else the
// one compiled in from .env. With neither, or while the station cannot
// connect for a while, the board also opens its own access point
// "<hostname>-setup" (192.168.4.1) with a captive DNS, so a phone that joins
// it lands on the setup page (wifi_setup.c) and can pick a network. The
// station keeps retrying meanwhile; the access point closes a minute after
// the station is connected.
#include "wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "local_config.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_sta";

#define NVS_NS "wifi"
#define AP_AFTER_FAIL_MS 60000      // station not connected this long: open the setup AP
#define AP_CLOSE_AFTER_MS 60000     // station connected this long: close it
#define AP_MAX_CLIENTS 4

static volatile bool s_connected;
static volatile bool s_ap_on;
static int s_retries;
static esp_netif_t *s_netif, *s_ap_netif;
static char s_ssid[33], s_pass[65];
static wifi_cred_source_t s_source;
static char s_ap_ssid[33];
static int64_t s_down_since;        // when the station lost (or never had) the link, us
static int64_t s_up_since;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_dns_task;

static esp_err_t nvs_ready(void);

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_ssid[0])
            esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        if (s_connected)
            s_down_since = esp_timer_get_time();
        s_connected = false;
        if (s_retries++ % 10 == 0)
            ESP_LOGW(TAG, "disconnected (reason %d), retrying", d->reason);
        if (s_ssid[0])
            esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "connected, IP " IPSTR, IP2STR(&e->ip_info.ip));
        s_retries = 0;
        s_connected = true;
        s_up_since = esp_timer_get_time();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "a device joined the setup network");
    }
}

// --- captive DNS: every name resolves to the access point (192.168.4.1), so
// phones show the setup page as a "sign in to network" page

static void dns_task(void *arg)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        ESP_LOGW(TAG, "captive DNS: cannot bind port 53");
        if (s >= 0)
            close(s);
        s_dns_task = NULL;
        vTaskDelete(NULL);
    }
    struct timeval tv = {.tv_sec = 1};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint8_t q[512];
    while (s_ap_on) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(s, q, sizeof(q) - 16, 0, (struct sockaddr *)&from, &fl);
        if (n < 12 || (q[2] & 0x80))        // too short, or a response
            continue;
        // find the end of the (first) question: name, type, class
        int p = 12;
        while (p < n && q[p])
            p += q[p] + 1;
        p += 5;
        if (p > n)
            continue;
        uint16_t qtype = (uint16_t)(q[p - 4] << 8 | q[p - 3]);
        q[2] = 0x84 | (q[2] & 0x01);        // response, authoritative, RD copied
        q[3] = 0x00;
        q[4] = 0;
        q[5] = 1;                           // one question
        q[6] = 0;
        q[7] = qtype == 1 ? 1 : 0;          // one answer for A, none for AAAA & co.
        q[8] = q[9] = q[10] = q[11] = 0;
        int len = p;
        if (qtype == 1) {
            static const uint8_t ans[] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 192, 168, 4, 1};
            memcpy(q + len, ans, sizeof(ans));
            len += sizeof(ans);
        }
        sendto(s, q, len, 0, (struct sockaddr *)&from, fl);
    }
    close(s);
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

static void ap_set(bool on)
{
    if (on == s_ap_on)
        return;
    if (on) {
        wifi_config_t ap = {0};
        strncpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
        ap.ap.ssid_len = (uint8_t)strlen(s_ap_ssid);
        ap.ap.max_connection = AP_MAX_CLIENTS;
        size_t pl = strlen(SETUP_PASSWORD);
        if (pl >= 8) {
            strncpy((char *)ap.ap.password, SETUP_PASSWORD, sizeof(ap.ap.password));
            ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
        } else {
            ap.ap.authmode = WIFI_AUTH_OPEN;
        }
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        esp_wifi_set_config(WIFI_IF_AP, &ap);
        s_ap_on = true;
        if (!s_dns_task)
            xTaskCreate(dns_task, "captive_dns", 3072, NULL, 3, &s_dns_task);
        ESP_LOGW(TAG, "setup network \"%s\" is open%s: join it and open http://192.168.4.1/wifi", s_ap_ssid,
                 pl >= 8 ? " (password from .env SETUP_PASSWORD)" : "");
    } else {
        s_ap_on = false;            // the DNS task ends within a second
        esp_wifi_set_mode(WIFI_MODE_STA);
        ESP_LOGI(TAG, "setup network closed");
    }
}

// opens / closes the setup access point as the station's state requires
static void supervisor_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        int64_t now = esp_timer_get_time();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (!s_ssid[0] || (!s_connected && now - s_down_since > AP_AFTER_FAIL_MS * 1000LL))
            ap_set(true);
        else if (s_connected && s_ap_on && now - s_up_since > AP_CLOSE_AFTER_MS * 1000LL)
            ap_set(false);
        xSemaphoreGive(s_lock);
    }
}

static void load_credentials(void)
{
    nvs_handle_t h;
    s_ssid[0] = s_pass[0] = 0;
    s_source = WIFI_CRED_NONE;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof(s_ssid);
        if (nvs_get_str(h, "ssid", s_ssid, &n) == ESP_OK && s_ssid[0]) {
            n = sizeof(s_pass);
            if (nvs_get_str(h, "pass", s_pass, &n) != ESP_OK)
                s_pass[0] = 0;
            s_source = WIFI_CRED_SAVED;
        }
        nvs_close(h);
    }
    if (s_source == WIFI_CRED_NONE && strlen(WIFI_SSID)) {
        snprintf(s_ssid, sizeof(s_ssid), "%s", WIFI_SSID);
        snprintf(s_pass, sizeof(s_pass), "%s", WIFI_PASSWORD);
        s_source = WIFI_CRED_BUILD;
    }
}

// the station's configuration from s_ssid / s_pass, and (re)connect
static void apply_station(void)
{
    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, s_ssid, sizeof(cfg.sta.ssid));
    strncpy((char *)cfg.sta.password, s_pass, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = s_pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    s_connected = false;
    s_down_since = esp_timer_get_time();
    s_retries = 0;
    if (s_ssid[0])
        esp_wifi_connect();
}

esp_err_t wifi_start(const char *hostname)
{
    ESP_RETURN_ON_ERROR(nvs_ready(), TAG, "nvs");
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    s_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_netif, hostname);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%.25s-setup", hostname);

    // the driver's own info logs include the SSID; keep warnings only
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("wifi_init", ESP_LOG_WARN);
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "wifi init");
    esp_wifi_set_storage(WIFI_STORAGE_RAM);     // our own NVS keys hold the network
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);

    load_credentials();
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, s_ssid, sizeof(cfg.sta.ssid));
    strncpy((char *)cfg.sta.password, s_pass, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = s_pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "config");
    // no modem sleep: a sleeping station answers mDNS / IPP slowly
    esp_wifi_set_ps(WIFI_PS_NONE);
    s_down_since = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
    if (!s_ssid[0]) {
        ESP_LOGW(TAG, "no WiFi network configured");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        ap_set(true);
        xSemaphoreGive(s_lock);
    } else {
        ESP_LOGI(TAG, "connecting as %s (network from %s)", hostname,
                 s_source == WIFI_CRED_SAVED ? "the setup page" : ".env");
    }
    xTaskCreate(supervisor_task, "wifi_sup", 3072, NULL, 3, NULL);

    // real wall clock for IPP date-time attributes
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp);
    return ESP_OK;
}

static esp_err_t nvs_ready(void)
{
    esp_err_t err = nvs_flash_init();      // ESP_OK again if already done
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t wifi_store_network(const char *ssid, const char *password)
{
    size_t sl = strlen(ssid), pl = strlen(password);
    if (sl == 0 || sl > 32 || pl > 64 || (pl && pl < 8))
        return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(nvs_ready(), TAG, "nvs");
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "nvs");
    esp_err_t err = nvs_set_str(h, "ssid", ssid);
    if (err == ESP_OK)
        err = nvs_set_str(h, "pass", password);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t wifi_set_network(const char *ssid, const char *password)
{
    if (!s_lock)
        return ESP_ERR_INVALID_STATE;
    size_t sl = strlen(ssid), pl = strlen(password);
    if (sl == 0 || sl > 32 || pl > 64 || (pl && pl < 8))
        return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(wifi_store_network(ssid, password), TAG, "save");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    snprintf(s_pass, sizeof(s_pass), "%s", password);
    s_source = WIFI_CRED_SAVED;
    apply_station();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "new network saved, connecting");
    return ESP_OK;
}

esp_err_t wifi_forget_network(void)
{
    if (!s_lock)
        return ESP_ERR_INVALID_STATE;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    load_credentials();             // back to .env's network, if any
    apply_station();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "saved network forgotten (%s)", s_ssid[0] ? "back to the one from .env" : "none left");
    return ESP_OK;
}

void wifi_get_status(wifi_status_t *st)
{
    memset(st, 0, sizeof(*st));
    if (!s_lock)        // WiFi not started (the QEMU self-test runs on Ethernet)
        return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(st->ssid, sizeof(st->ssid), "%s", s_ssid);
    st->source = s_source;
    xSemaphoreGive(s_lock);
    st->connected = s_connected;
    st->ap_on = s_ap_on;
    snprintf(st->ap_ssid, sizeof(st->ap_ssid), "%s", s_ap_ssid);
    wifi_get_ip(st->ip, sizeof(st->ip));
    st->rssi = wifi_rssi();
}

int wifi_scan(wifi_network_t *out, int max)
{
    if (!s_lock)
        return -1;
    wifi_scan_config_t sc = {.show_hidden = false};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK)
        return -1;
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0)
        return 0;
    wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
    if (!recs) {
        esp_wifi_clear_ap_list();
        return -1;
    }
    esp_wifi_scan_get_ap_records(&n, recs);
    int k = 0;
    for (int i = 0; i < n && k < max; i++) {       // strongest first; one entry per name
        if (!recs[i].ssid[0])
            continue;
        bool dup = false;
        for (int j = 0; j < k && !dup; j++)
            dup = !strcmp(out[j].ssid, (const char *)recs[i].ssid);
        if (dup)
            continue;
        snprintf(out[k].ssid, sizeof(out[k].ssid), "%s", (const char *)recs[i].ssid);
        out[k].rssi = recs[i].rssi;
        out[k].secure = recs[i].authmode != WIFI_AUTH_OPEN;
        k++;
    }
    free(recs);
    return k;
}

bool wifi_connected(void)
{
    return s_connected;
}

bool wifi_setup_ap_on(void)
{
    return s_ap_on;
}

bool wifi_get_ip(char *buf, size_t n)
{
    esp_netif_ip_info_t ip;
    if (!s_connected || !s_netif || esp_netif_get_ip_info(s_netif, &ip) != ESP_OK) {
        if (n)
            buf[0] = 0;
        return false;
    }
    snprintf(buf, n, IPSTR, IP2STR(&ip.ip));
    return true;
}

int wifi_rssi(void)
{
    int rssi = 0;
    if (!s_connected || esp_wifi_sta_get_rssi(&rssi) != ESP_OK)
        return 0;
    return rssi;
}
