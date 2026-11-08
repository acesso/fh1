// Wi-Fi as a device. With saved credentials it joins that network; without, it opens a setup
// access point "FH1-setup". Credentials are set over the serial console (!wifi <ssid> <pass>).
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "fh1_devconsole.h"
#include "fh1_settings.h"
#include "console_priv.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "mdns.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"

static const char *TAG = "wifi";
static esp_netif_t *s_sta, *s_ap;
static bool s_inited, s_started, s_connected;
static char s_ip[16];
static fh1_device_t *s_dev;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        s_ip[0] = 0;
        if (s_started) esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        static bool sntp_started;
        if (!sntp_started) {
            esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2, ESP_SNTP_SERVER_LIST("a.ntp.br", "pool.ntp.org"));
            sc.wait_for_sync = false;
            sntp_started = esp_netif_sntp_init(&sc) == ESP_OK;
        }
        ESP_LOGI(TAG, "connected, http://%s/ or http://fh1.local/ (code %s)", s_ip, fh1_console_token());
        if (s_dev) snprintf(s_dev->info, sizeof(s_dev->info), "station, %s", s_ip);
    }
}

static esp_err_t wifi_probe(fh1_device_t *d)
{
    s_dev = d;
    strlcpy(d->info, "ESP32-S3 radio, 2.4 GHz", sizeof(d->info));
    return ESP_OK;
}

static esp_err_t wifi_start(fh1_device_t *d)
{
    if (!s_inited) {
        ESP_ERROR_CHECK(esp_netif_init());
        s_sta = esp_netif_create_default_wifi_sta();
        s_ap = esp_netif_create_default_wifi_ap();
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
        if (mdns_init() == ESP_OK) {
            mdns_hostname_set("fh1");
            mdns_instance_name_set("FH1 dev console");
            mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        }
        s_inited = true;
    }
    char ssid[33], pass[65];
    fh1_settings_get_str("wifi", "ssid", ssid, sizeof(ssid));
    fh1_settings_get_str("wifi", "pass", pass, sizeof(pass));
    if (ssid[0]) {
        wifi_config_t sc = { 0 };
        strlcpy((char *)sc.sta.ssid, ssid, sizeof(sc.sta.ssid));
        strlcpy((char *)sc.sta.password, pass, sizeof(sc.sta.password));
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &sc);
        snprintf(d->info, sizeof(d->info), "station, joining %s", ssid);
    } else {
        wifi_config_t ac = { .ap = { .ssid = "FH1-setup", .ssid_len = 9, .password = "fh1-setup",
                                     .max_connection = 2, .authmode = WIFI_AUTH_WPA2_PSK, .channel = 6 } };
        esp_wifi_set_mode(WIFI_MODE_AP);
        esp_wifi_set_config(WIFI_IF_AP, &ac);
        strlcpy(d->info, "access point FH1-setup, 192.168.4.1", sizeof(d->info));
        ESP_LOGW(TAG, "no credentials: access point FH1-setup (password fh1-setup); set them with !wifi <ssid> <pass>");
    }
    s_started = true;
    esp_err_t err = esp_wifi_start();
    // No modem power save: with it, every radio wake-up allocates a timer from internal RAM, and Wi-Fi
    // aborts the system when that fails under load (seen with a game plus the stress test). It also cuts
    // input delay from the console. Battery devices revisit this with proper sleep modes.
    if (err == ESP_OK) esp_wifi_set_ps(WIFI_PS_NONE);
    if (err == ESP_OK) console_server_start();
    return err;
}

static void wifi_stop(fh1_device_t *d)
{
    s_started = false;
    s_connected = false;
    s_ip[0] = 0;
    if (s_inited) esp_wifi_stop();
}

const fh1_driver_t fh1_wifi_driver = { .name = "esp_wifi", .probe = wifi_probe, .start = wifi_start, .stop = wifi_stop };

void fh1_wifi_set_credentials(const char *ssid, const char *pass)
{
    fh1_settings_set_str("wifi", "ssid", ssid);
    fh1_settings_set_str("wifi", "pass", pass);
    ESP_LOGI(TAG, "credentials saved for %s", ssid);
    if (s_dev && s_dev->state == FH1_DEV_READY) {
        wifi_stop(s_dev);
        wifi_start(s_dev);
    }
}

bool fh1_wifi_connected(void) { return s_connected; }

bool fh1_wifi_status(char *ssid, size_t ssid_len, char *ip, size_t ip_len, int *rssi)
{
    fh1_settings_get_str("wifi", "ssid", ssid, ssid_len);
    strlcpy(ip, s_ip, ip_len);
    *rssi = 0;
    if (s_connected) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) *rssi = ap.rssi;
    }
    return s_connected;
}

// The console's controller and the serial keys, as one input device.
static esp_err_t devinput_probe(fh1_device_t *d)
{
    strlcpy(d->info, "web console controller + serial keys", sizeof(d->info));
    return ESP_OK;
}
const fh1_driver_t fh1_devinput_driver = { .name = "devinput", .probe = devinput_probe };

int fh1_wifi_scan(fh1_wifi_ap_t *out, int max)
{
    if (!s_started) return -1;
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_AP) esp_wifi_set_mode(WIFI_MODE_APSTA);    // scanning needs the station side
    int n = -1;
    if (esp_wifi_scan_start(NULL, true) == ESP_OK) {
        uint16_t count = max;
        wifi_ap_record_t *recs = calloc(max, sizeof(*recs));
        if (recs && esp_wifi_scan_get_ap_records(&count, recs) == ESP_OK) {
            n = 0;
            for (int i = 0; i < count; i++) {
                if (!recs[i].ssid[0]) continue;                  // hidden networks
                bool dup = false;
                for (int j = 0; j < n && !dup; j++) dup = strcmp(out[j].ssid, (char *)recs[i].ssid) == 0;
                if (dup) continue;
                strlcpy(out[n].ssid, (char *)recs[i].ssid, sizeof(out[n].ssid));
                out[n].rssi = recs[i].rssi;
                out[n].secure = recs[i].authmode != WIFI_AUTH_OPEN;
                n++;
            }
        }
        free(recs);
    }
    if (mode == WIFI_MODE_AP) esp_wifi_set_mode(WIFI_MODE_AP);
    return n;
}

void fh1_wifi_forget(void)
{
    fh1_settings_set_str("wifi", "ssid", "");
    fh1_settings_set_str("wifi", "pass", "");
    ESP_LOGI(TAG, "saved network forgotten");
    if (s_dev && s_dev->state == FH1_DEV_READY) {
        wifi_stop(s_dev);
        wifi_start(s_dev);
    }
}

// ── Other devices by mDNS (fh1_devconsole.h) ──

void fh1_mdns_announce(const char *service, const char *key, const char *value)
{
    if (!s_inited) return;
    if (mdns_service_exists(service, "_tcp", NULL)) {
        mdns_service_txt_item_set(service, "_tcp", key, value);
        return;
    }
    mdns_txt_item_t txt = { .key = key, .value = value };
    mdns_service_add(NULL, service, "_tcp", 80, &txt, 1);
}

int fh1_mdns_find(const char *service, const char *key, fh1_mdns_peer_t *out, int max, int timeout_ms)
{
    if (!s_inited || !s_connected) return 0;
    mdns_result_t *res = NULL;
    if (mdns_query_ptr(service, "_tcp", timeout_ms, max, &res) != ESP_OK) return 0;
    int k = 0;
    // A device on several networks (a PC with a container bridge) gives several addresses: the one on this
    // device's own network first.
    esp_netif_ip_info_t me = { 0 };
    if (s_sta) esp_netif_get_ip_info(s_sta, &me);
    for (mdns_result_t *r = res; r && k < max; r = r->next) {
        uint32_t ip = 0;
        for (mdns_ip_addr_t *a = r->addr; a; a = a->next) {
            if (a->addr.type != ESP_IPADDR_TYPE_V4) continue;
            const uint32_t x = a->addr.u_addr.ip4.addr;
            if (!ip || (me.netmask.addr && (x & me.netmask.addr) == (me.ip.addr & me.netmask.addr))) ip = x;
        }
        if (!ip) continue;
        bool dup = false;
        for (int j = 0; j < k; j++) dup |= out[j].ip == ip;
        if (dup) continue;
        out[k].ip = ip;
        out[k].port = r->port ? r->port : 80;
        out[k].txt[0] = 0;
        for (size_t i = 0; i < r->txt_count; i++)
            if (r->txt[i].key && strcmp(r->txt[i].key, key) == 0 && r->txt[i].value)
                snprintf(out[k].txt, sizeof(out[k].txt), "%.*s", r->txt_value_len ? r->txt_value_len[i] : 23, r->txt[i].value);
        k++;
    }
    mdns_query_results_free(res);
    return k;
}
