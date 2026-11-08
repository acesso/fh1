#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "fh1_ota.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "ota";

struct fh1_ota {
    esp_ota_handle_t h;
    const esp_partition_t *part;
    size_t written;
    bool header_checked;
    uint8_t head[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)];
    size_t head_len;
};

// The first bytes hold the image header and the app description: check it is an FH1 firmware
// for this chip before anything is written.
static esp_err_t check_header(const uint8_t *buf, size_t len, char *version, size_t vlen, char *err, size_t n)
{
    const esp_image_header_t *ih = (const void *)buf;
    const esp_app_desc_t *d = (const void *)(buf + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
    if (len < sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t) ||
        ih->magic != ESP_IMAGE_HEADER_MAGIC || d->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        snprintf(err, n, "not an ESP firmware image");
        return ESP_ERR_INVALID_ARG;
    }
    if (ih->chip_id != ESP_CHIP_ID_ESP32S3) {
        snprintf(err, n, "firmware for another chip");
        return ESP_ERR_INVALID_ARG;
    }
    if (strncmp(d->project_name, esp_app_get_description()->project_name, sizeof(d->project_name)) != 0) {
        snprintf(err, n, "not FH1 firmware (project \"%.20s\")", d->project_name);
        return ESP_ERR_INVALID_ARG;
    }
    if (version) strlcpy(version, d->version, vlen);
    return ESP_OK;
}

esp_err_t fh1_ota_begin(size_t size, fh1_ota_t **out, char *err, size_t n)
{
    fh1_ota_t *o = calloc(1, sizeof(*o));
    if (!o) return ESP_ERR_NO_MEM;
    o->part = esp_ota_get_next_update_partition(NULL);
    if (!o->part) {
        snprintf(err, n, "no spare firmware slot");
        free(o);
        return ESP_FAIL;
    }
    if (size && size > o->part->size) {
        snprintf(err, n, "image too big for the slot");
        free(o);
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t e = esp_ota_begin(o->part, size ? size : OTA_SIZE_UNKNOWN, &o->h);
    if (e != ESP_OK) {
        snprintf(err, n, "esp_ota_begin: %s", esp_err_to_name(e));
        free(o);
        return e;
    }
    ESP_LOGI(TAG, "writing to %s", o->part->label);
    *out = o;
    return ESP_OK;
}

esp_err_t fh1_ota_write(fh1_ota_t *o, const void *data, size_t len, char *err, size_t n)
{
    if (!o->header_checked) {
        size_t take = sizeof(o->head) - o->head_len;
        if (take > len) take = len;
        memcpy(o->head + o->head_len, data, take);
        o->head_len += take;
        if (o->head_len == sizeof(o->head)) {
            char version[32];
            esp_err_t e = check_header(o->head, o->head_len, version, sizeof(version), err, n);
            if (e != ESP_OK) return e;
            ESP_LOGI(TAG, "image version %s", version);
            o->header_checked = true;
        }
    }
    esp_err_t e = esp_ota_write(o->h, data, len);
    if (e != ESP_OK) snprintf(err, n, "write: %s", esp_err_to_name(e));
    o->written += len;
    return e;
}

esp_err_t fh1_ota_finish(fh1_ota_t *o, char *err, size_t n)
{
    esp_err_t e = o->header_checked ? esp_ota_end(o->h) : ESP_ERR_INVALID_SIZE;
    if (e == ESP_OK) e = esp_ota_set_boot_partition(o->part);
    else if (!o->header_checked) {
        esp_ota_abort(o->h);
        snprintf(err, n, "image too short");
    } else {
        snprintf(err, n, e == ESP_ERR_OTA_VALIDATE_FAILED ? "image is corrupt" : "esp_ota_end: %s", esp_err_to_name(e));
    }
    if (e == ESP_OK) ESP_LOGI(TAG, "%u bytes written; %s boots next", (unsigned)o->written, o->part->label);
    free(o);
    return e;
}

void fh1_ota_abort(fh1_ota_t *o)
{
    if (!o) return;
    esp_ota_abort(o->h);
    free(o);
}

esp_err_t fh1_ota_peek_file(const char *path, char *version, size_t vlen, char *err, size_t n)
{
    uint8_t head[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)];
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, n, "cannot open %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    size_t len = fread(head, 1, sizeof(head), f);
    fclose(f);
    return check_header(head, len, version, vlen, err, n);
}

esp_err_t fh1_ota_from_file(const char *path, void (*progress)(int pct, void *user), void *user, char *err, size_t n)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, n, "cannot open %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    fh1_ota_t *o;
    esp_err_t e = fh1_ota_begin(size > 0 ? size : 0, &o, err, n);
    // Internal, DMA-capable: the SD driver reads by DMA straight into the buffer, and a PSRAM buffer
    // came back with stale cache contents (wrong data, identical on every read).
    uint8_t *buf = e == ESP_OK ? heap_caps_malloc(8192, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) : NULL;
    long done = 0;
    int last = -1;
    while (e == ESP_OK && buf) {
        size_t r = fread(buf, 1, 8192, f);
        if (r == 0) break;
        e = fh1_ota_write(o, buf, r, err, n);
        done += r;
        int pct = size > 0 ? (int)(done * 100 / size) : 0;
        if (progress && pct != last) progress(pct, user);
        last = pct;
    }
    fclose(f);
    heap_caps_free(buf);
    if (e == ESP_OK) return fh1_ota_finish(o, err, n);
    fh1_ota_abort(o);
    return e;
}

void fh1_ota_mark_valid(void)
{
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new firmware marked valid");
    }
}

const char *fh1_ota_running_version(void) { return esp_app_get_description()->version; }
