// Firmware updates over Wi-Fi (ota.c): POST /ota with the bridge's code.
#pragma once
#include <stdint.h>

void bridge_ota_init(void);            // at start: the code, and keeping this firmware once it has run 10 s
void bridge_ota_start(void);           // Wi-Fi has an address: the web server starts
uint32_t bridge_ota_code(void);        // six digits
