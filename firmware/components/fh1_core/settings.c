#include <stdbool.h>
#include "fh1_settings.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

void fh1_settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW("settings", "NVS erased (%s)", esp_err_to_name(err));
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

bool fh1_settings_erase(const char *ns)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return false;      // no such namespace: nothing to do
    nvs_close(h);
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_erase_all(h) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

uint8_t fh1_settings_get_u8(const char *ns, const char *key, uint8_t def)
{
    nvs_handle_t h;
    uint8_t v = def;
    if (nvs_open(ns, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, key, &v);
        nvs_close(h);
    }
    return v;
}

void fh1_settings_set_u8(const char *ns, const char *key, uint8_t val)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, val);
        nvs_commit(h);
        nvs_close(h);
    }
}

uint32_t fh1_settings_get_u32(const char *ns, const char *key, uint32_t def)
{
    nvs_handle_t h;
    uint32_t v = def;
    if (nvs_open(ns, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, key, &v);
        nvs_close(h);
    }
    return v;
}

void fh1_settings_set_u32(const char *ns, const char *key, uint32_t val)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, key, val);
        nvs_commit(h);
        nvs_close(h);
    }
}

bool fh1_settings_get_str(const char *ns, const char *key, char *buf, size_t len)
{
    nvs_handle_t h;
    bool ok = false;
    buf[0] = 0;
    if (nvs_open(ns, NVS_READONLY, &h) == ESP_OK) {
        size_t n = len;
        ok = nvs_get_str(h, key, buf, &n) == ESP_OK;
        nvs_close(h);
    }
    if (!ok) buf[0] = 0;
    return ok;
}

void fh1_settings_set_str(const char *ns, const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, key, val);
        nvs_commit(h);
        nvs_close(h);
    }
}

// Binary values (keys, small records). get: len in = buffer size, out = the value's size.
bool fh1_settings_get_blob(const char *ns, const char *key, void *buf, size_t *len)
{
    nvs_handle_t h;
    bool ok = false;
    if (nvs_open(ns, NVS_READONLY, &h) == ESP_OK) {
        ok = nvs_get_blob(h, key, buf, len) == ESP_OK;
        nvs_close(h);
    }
    if (!ok) *len = 0;
    return ok;
}

bool fh1_settings_set_blob(const char *ns, const char *key, const void *buf, size_t len)
{
    nvs_handle_t h;
    bool ok = false;
    if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
        ok = nvs_set_blob(h, key, buf, len) == ESP_OK && nvs_commit(h) == ESP_OK;
        nvs_close(h);
    }
    return ok;
}

void fh1_settings_erase_key(const char *ns, const char *key)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, key);
        nvs_commit(h);
        nvs_close(h);
    }
}
