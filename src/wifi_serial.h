// WiFi setup over the COM port: tools/set_wifi.py (see wifi_serial.c).
#pragma once

// Starts listening on the console UART. Call first thing in app_main: opening
// the port usually resets the board, and the tool writes 2 s later.
void wifi_serial_start(void);
