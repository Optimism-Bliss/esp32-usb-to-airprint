// WiFi station: the network saved from the setup page (NVS), else the one
// from .env. Without either, or while the station cannot connect, a setup
// access point "<hostname>-setup" is open (wifi_setup.c serves the page).
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

typedef enum {
    WIFI_CRED_NONE,
    WIFI_CRED_BUILD,            // .env, compiled in
    WIFI_CRED_SAVED,            // saved from the setup page (NVS)
} wifi_cred_source_t;

typedef struct {
    char ssid[33];              // the network the station uses ("" = none)
    wifi_cred_source_t source;
    bool connected;
    char ip[16];
    int rssi;
    bool ap_on;                 // the setup access point is open
    char ap_ssid[33];
} wifi_status_t;

typedef struct {
    char ssid[33];
    int rssi;
    bool secure;
} wifi_network_t;

// Starts the station (and the setup access point when needed); reconnects forever.
esp_err_t wifi_start(const char *hostname);
bool wifi_connected(void);
bool wifi_setup_ap_on(void);
// The station's IPv4 address as text; false (and "") while not connected.
bool wifi_get_ip(char *buf, size_t n);
// Signal of the access point in dBm, 0 when unknown.
int wifi_rssi(void);
void wifi_get_status(wifi_status_t *st);
// Saves a network (password "" = open, else 8..64 characters) and joins it.
esp_err_t wifi_set_network(const char *ssid, const char *password);
// Only saves it (NVS), for the next start (the COM port setup restarts).
esp_err_t wifi_store_network(const char *ssid, const char *password);
// Forgets the saved network: back to .env's, else setup mode.
esp_err_t wifi_forget_network(void);
// Networks in range, strongest first (blocks ~2 s). Count, or -1.
int wifi_scan(wifi_network_t *out, int max);
