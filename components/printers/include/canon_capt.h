// Settings of the Canon CAPT laser drivers (canon_capt.c), for the debug server.
#pragma once

#include <stdbool.h>

// Toner density 0..15 (7 = Canon's default) and toner saving, for the next pages.
void canon_capt_set(int density, bool toner_save);
void canon_capt_get(int *density, bool *toner_save);
