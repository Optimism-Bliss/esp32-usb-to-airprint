// The WiFi setup page (/wifi) and the setup network's captive redirects.
#pragma once

#include <stdbool.h>

#include "airprint.h"

// An http_handler of the print server: true once it answered.
bool wifi_setup_http(airprint_http_t *h, const char *method, const char *path);
