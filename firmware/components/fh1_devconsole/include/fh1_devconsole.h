// The web dev console (firmware/PLAN.md, "Dev console"): live system stats, devices, apps, log,
// and a remote controller, at http://fh1.local/. Wi-Fi is a device (net.wifi) like any other.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "fh1_devices.h"
#include "cJSON.h"
#include "esp_http_server.h"

extern const fh1_driver_t fh1_wifi_driver;     // net.wifi; starts the console's web server once up
extern const fh1_driver_t fh1_devinput_driver; // input.buttons: the console's controller and serial keys

void fh1_wifi_set_credentials(const char *ssid, const char *pass);   // saved, then reconnects
bool fh1_wifi_status(char *ssid, size_t ssid_len, char *ip, size_t ip_len, int *rssi);
bool fh1_wifi_connected(void);                         // no flash access: safe from any task

typedef struct {
    char ssid[33];
    int rssi;
    bool secure;
} fh1_wifi_ap_t;
int fh1_wifi_scan(fh1_wifi_ap_t *out, int max);       // blocking, a few seconds; count, or -1
void fh1_wifi_forget(void);                           // drop the saved network; back to the setup access point

void fh1_console_log_init(void);                 // capture the ESP log for the console (call early)
const char *fh1_console_token(void);             // the 6-digit access code
// Another page on the console's server (no access code: the handler checks what it gets). Up to 4.
void fh1_console_add_uri(const httpd_uri_t *u);

// Other devices on the network by mDNS (service like "_fh1"; protocol _tcp, port 80). announce adds this
// device's service with one TXT item (again: updates it). find asks the network (blocks up to timeout_ms):
// each one's IPv4 address (network order) and the value of its TXT item key.
typedef struct {
    uint32_t ip;
    uint16_t port;
    char txt[24];
} fh1_mdns_peer_t;
void fh1_mdns_announce(const char *service, const char *key, const char *value);
int fh1_mdns_find(const char *service, const char *key, fh1_mdns_peer_t *out, int max, int timeout_ms);

// Hooks for the apps layer: add its state to the stats, and handle "app" commands.
typedef void (*fh1_console_apps_json_fn)(cJSON *root);
typedef void (*fh1_console_apps_cmd_fn)(const cJSON *msg, char *reply, size_t reply_len);
void fh1_console_set_apps_hooks(fh1_console_apps_json_fn json_fn, fh1_console_apps_cmd_fn cmd_fn);
