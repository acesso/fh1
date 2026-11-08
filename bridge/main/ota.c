// Firmware updates over Wi-Fi: POST the image (fh1_bridge.bin) to http://<bridge>/ota with the header
// X-FH1-Bridge-Code set to the bridge's code. The code is six digits made once and kept in NVS ("bridge",
// "ota_code"); the bridge reports it to the FH1 (fh1b_status_t.ota_code: its About shows it, and the FH1's
// dev console lists it). The image goes to the other app slot; the bridge restarts into it, and the
// bootloader rolls back unless it runs for OTA_VALID_S (bridge_ota_init marks it good then).
#include <string.h>
#include <stdlib.h>
#include "bridge_ota.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bridge_ota";

#define OTA_VALID_S 10

static uint32_t s_code;
static httpd_handle_t s_server;

uint32_t bridge_ota_code(void) { return s_code; }

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));    // the answer gets out first
    esp_restart();
}

static esp_err_t ota_post(httpd_req_t *req)
{
    char code[12] = "";
    httpd_req_get_hdr_value_str(req, "X-FH1-Bridge-Code", code, sizeof(code));
    if (!s_code || (uint32_t)strtoul(code, NULL, 10) != s_code) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "code needed (X-FH1-Bridge-Code)");
        return ESP_FAIL;
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t h;
    if (!part || esp_ota_begin(part, OTA_SIZE_UNKNOWN, &h) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no slot for an update");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "update: %d bytes into %s", req->content_len, part->label);
    static char buf[1024];
    int left = req->content_len;
    esp_err_t err = ESP_OK;
    while (left > 0 && err == ESP_OK) {
        int n = httpd_req_recv(req, buf, left < (int)sizeof(buf) ? left : (int)sizeof(buf));
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) {
            err = ESP_FAIL;
            break;
        }
        err = esp_ota_write(h, buf, n);
        left -= n;
    }
    if (err == ESP_OK) err = esp_ota_end(h);
    else esp_ota_abort(h);
    if (err == ESP_OK) err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "update failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "update written; restarting into %s", part->label);
    httpd_resp_sendstr(req, "updated; restarting\n");
    xTaskCreate(restart_task, "ota_restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// What it runs (no code needed): to check a board before and after an update.
static esp_err_t info_get(httpd_req_t *req)
{
    const esp_app_desc_t *d = esp_app_get_description();
    const esp_partition_t *p = esp_ota_get_running_partition();
    char b[160];
    snprintf(b, sizeof(b), "FH1 bridge %s, built %s %s, running from %s\n", BRIDGE_BOARD_NAME, d->date, d->time, p ? p->label : "?");
    httpd_resp_sendstr(req, b);
    return ESP_OK;
}

static void mark_valid(void *arg)
{
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) ESP_LOGI(TAG, "this firmware is kept");
}

void bridge_ota_init(void)
{
    nvs_handle_t n;
    if (nvs_open("bridge", NVS_READWRITE, &n) == ESP_OK) {
        if (nvs_get_u32(n, "ota_code", &s_code) != ESP_OK || !s_code) {
            s_code = 100000 + esp_random() % 900000;
            nvs_set_u32(n, "ota_code", s_code);
            nvs_commit(n);
        }
        nvs_close(n);
    }
    // A new firmware is kept once it has run this long; a crash before sends the bootloader back.
    static esp_timer_handle_t t;
    const esp_timer_create_args_t a = { .callback = mark_valid, .name = "ota_valid" };
    if (esp_timer_create(&a, &t) == ESP_OK) esp_timer_start_once(t, OTA_VALID_S * 1000000ULL);
}

void bridge_ota_start(void)
{
    if (s_server) return;
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.stack_size = 4096;
    c.max_uri_handlers = 2;
    c.max_open_sockets = 2;
    if (httpd_start(&s_server, &c) != ESP_OK) {
        ESP_LOGE(TAG, "no web server");
        return;
    }
    const httpd_uri_t ota = { .uri = "/ota", .method = HTTP_POST, .handler = ota_post };
    const httpd_uri_t info = { .uri = "/", .method = HTTP_GET, .handler = info_get };
    httpd_register_uri_handler(s_server, &ota);
    httpd_register_uri_handler(s_server, &info);
    ESP_LOGI(TAG, "updates at POST /ota (code %06lu)", (unsigned long)s_code);
}
