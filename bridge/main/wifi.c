// The bridge's Wi-Fi (station). Off unless switched on with a saved network: Wi-Fi shares the radio with
// the controllers (Classic) and the FH1's link (LE), so it is only started when wanted, and initialised only
// then (it takes tens of KB of RAM). Settings in NVS "bridge": wifi_on, ssid, pass. The FH1 sets them over
// its encrypted link (bridge.c). It reconnects by itself after a drop.
#include <string.h>
#include "bridge_wifi.h"
#include "bridge_udp.h"
#include "bridge_ota.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mdns.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "freertos/task.h"

static const char *TAG = "bridge_wifi";

static bool s_inited, s_started, s_on;
static volatile bool s_paused;         // while pairing a controller (bridge_wifi_pause)
static char s_ssid[33], s_pass[65];
static volatile uint8_t s_state = FH1B_WIFI_OFF;
static volatile uint32_t s_ip;
static TimerHandle_t s_retry;

static void load(void)
{
    nvs_handle_t h;
    if (nvs_open("bridge", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t on = 0;
    nvs_get_u8(h, "wifi_on", &on);
    s_on = on;
    size_t n = sizeof(s_ssid);
    if (nvs_get_str(h, "ssid", s_ssid, &n) != ESP_OK) s_ssid[0] = 0;
    n = sizeof(s_pass);
    if (nvs_get_str(h, "pass", s_pass, &n) != ESP_OK) s_pass[0] = 0;
    nvs_close(h);
}

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open("bridge", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "wifi_on", s_on);
    nvs_set_str(h, "ssid", s_ssid);
    nvs_set_str(h, "pass", s_pass);
    nvs_commit(h);
    nvs_close(h);
}

static void retry(TimerHandle_t t)
{
    if (s_started && s_state != FH1B_WIFI_CONNECTED) esp_wifi_connect();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_state = FH1B_WIFI_CONNECTING;
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_ip = 0;
        if (s_started) {
            s_state = FH1B_WIFI_CONNECTING;
            xTimerStart(s_retry, 0);                 // again in 5 s
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        s_ip = e->ip_info.ip.addr;
        s_state = FH1B_WIFI_CONNECTED;
        ESP_LOGI(TAG, "connected to %s, " IPSTR, s_ssid, IP2STR(&e->ip_info.ip));
        bridge_udp_start();                          // the link to the FH1 over this network
        bridge_ota_start();                          // firmware updates over it
    }
}

static void stop(void)
{
    if (!s_started) return;
    s_started = false;
    esp_wifi_stop();
    s_ip = 0;
    s_state = FH1B_WIFI_OFF;
    ESP_LOGI(TAG, "off");
}

// Start or stop as the settings say.
static void apply(void)
{
    if (s_paused && s_started) {
        stop();
        s_state = FH1B_WIFI_CONNECTING;               // back after the pairing
        ESP_LOGI(TAG, "paused while pairing a controller");
        return;
    }
    if (s_paused) return;
    if (!s_on) {
        stop();
        s_state = FH1B_WIFI_OFF;
        return;
    }
    if (!s_ssid[0]) {
        stop();
        s_state = FH1B_WIFI_NO_NETWORK;
        return;
    }
    if (!s_inited) {
        ESP_ERROR_CHECK(esp_netif_init());
        esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(err);
        // A name on the network (DHCP): fh1-bridge-<the last four hex digits of its Bluetooth address>.
        esp_netif_t *sta = esp_netif_create_default_wifi_sta();
        uint8_t mac[6];
        char host[24];
        esp_read_mac(mac, ESP_MAC_BT);
        snprintf(host, sizeof(host), "fh1-bridge-%02x%02x", mac[4], mac[5]);
        esp_netif_set_hostname(sta, host);
        // And by mDNS: <host>.local (the router did not publish the DHCP name), with its update page.
        if (mdns_init() == ESP_OK) {
            mdns_hostname_set(host);
            mdns_instance_name_set("FH1 bridge");
            mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        }
        ESP_LOGI(TAG, "network name %s (%s.local)", host, host);
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        if (esp_wifi_init(&cfg) != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi did not start (memory?)");
            return;
        }
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
        s_retry = xTimerCreate("wifi_retry", pdMS_TO_TICKS(5000), pdFALSE, NULL, retry);
        s_inited = true;
    }
    stop();
    wifi_config_t wc = { 0 };
    memcpy(wc.sta.ssid, s_ssid, strlen(s_ssid));
    memcpy(wc.sta.password, s_pass, strlen(s_pass));
    wc.sta.threshold.authmode = s_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    s_started = true;
    s_state = FH1B_WIFI_CONNECTING;
    esp_wifi_start();
    // No modem sleep: with Bluetooth on, the driver still keeps the station awake only when coexistence
    // gives it the radio, but it no longer waits for beacons to take commands (rumble).
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "connecting to %s", s_ssid);
}

void bridge_wifi_init(void)
{
    load();
    apply();
}

// Changes come from the FH1's commands, on Bluetooth's run loop: they are saved there, and Wi-Fi is
// (re)started from a short task of its own, so the command is answered at once (starting Wi-Fi the first
// time takes a moment).
static void apply_task(void *arg)
{
    apply();
    vTaskDelete(NULL);
}

static void apply_later(void)
{
    if (xTaskCreatePinnedToCore(apply_task, "wifi_apply", 4096, NULL, 3, NULL, 0) != pdPASS) apply();
}

void bridge_wifi_pause(bool pause)
{
    if (pause == s_paused) return;
    s_paused = pause;
    if (s_on && s_ssid[0]) apply_later();
}

void bridge_wifi_set(const char *ssid, const char *pass)
{
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    snprintf(s_pass, sizeof(s_pass), "%s", pass);
    s_on = true;
    save();
    s_state = FH1B_WIFI_CONNECTING;
    apply_later();
}

void bridge_wifi_enable(bool on)
{
    s_on = on;
    save();
    apply_later();
}

void bridge_wifi_forget(void)
{
    s_ssid[0] = s_pass[0] = 0;
    save();
    apply_later();
}

bool bridge_wifi_connected(void) { return s_state == FH1B_WIFI_CONNECTED; }

int8_t bridge_wifi_rssi(void)
{
    wifi_ap_record_t ap;
    return s_state == FH1B_WIFI_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

void bridge_wifi_status(fh1b_status_t *st)
{
    st->wifi = s_state;
    st->wifi_rssi = 0;
    memset(st->wifi_ip, 0, 4);
    snprintf(st->wifi_ssid, sizeof(st->wifi_ssid), "%s", s_ssid);
    if (s_state == FH1B_WIFI_CONNECTED) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) st->wifi_rssi = ap.rssi;
        uint32_t ip = s_ip;
        memcpy(st->wifi_ip, &ip, 4);
    }
}
