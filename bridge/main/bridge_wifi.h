// The bridge's Wi-Fi (wifi.c): off unless switched on with a saved network.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "fh1_bridge_proto.h"

void bridge_wifi_init(void);                           // from the saved settings
void bridge_wifi_set(const char *ssid, const char *pass);   // saved; switches it on
void bridge_wifi_enable(bool on);                      // saved
void bridge_wifi_forget(void);
// Off while a new controller is paired: Wi-Fi's share of the radio got in the way of paging (a Joy-Con
// refused the connection, HCI 0x0F, with Wi-Fi on). Back when pairing ends.
void bridge_wifi_pause(bool pause);
void bridge_wifi_status(fh1b_status_t *st);            // fills the Wi-Fi fields
bool bridge_wifi_connected(void);                      // has an address
int8_t bridge_wifi_rssi(void);                         // dBm, 0 when not connected
