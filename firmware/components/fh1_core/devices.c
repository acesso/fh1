// FH1 device manager (firmware/PLAN.md, "Devices and capabilities").
#include <string.h>
#include "fh1_devices.h"
#include "fh1_settings.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "devices";

ESP_EVENT_DEFINE_BASE(FH1_EVENT);

#define MAX_DEVICES 24

static fh1_device_t *s_devs[MAX_DEVICES];
static size_t s_count;
static SemaphoreHandle_t s_lock;
static volatile uint32_t s_caps;
static volatile uint32_t s_gen;

static const char *const s_cap_names[FH1_CAP_COUNT] = {
    "DISPLAY", "BACKLIGHT", "BUTTONS", "STICK_ANALOG", "TOUCH", "AUDIO_OUT", "AUDIO_IN", "SD",
    "WIFI", "BLE", "LORA", "IMU", "RTC", "BATTERY", "BUTTON_LIGHTS",
};

const char *fh1_cap_name(uint32_t cap_bit)
{
    for (int i = 0; i < FH1_CAP_COUNT; i++) {
        if (cap_bit == (1u << i)) return s_cap_names[i];
    }
    return "?";
}

uint32_t fh1_cap_from_name(const char *name)
{
    for (int i = 0; i < FH1_CAP_COUNT; i++) {
        if (strcmp(name, s_cap_names[i]) == 0) return 1u << i;
    }
    return 0;
}

const char *fh1_dev_state_name(fh1_dev_state_t s)
{
    switch (s) {
    case FH1_DEV_ABSENT: return "absent";
    case FH1_DEV_READY: return "ready";
    case FH1_DEV_DISABLED: return "disabled";
    case FH1_DEV_FAILED: return "failed";
    }
    return "?";
}

static void lock(void) { xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGiveRecursive(s_lock); }

static void recompute_caps(void)
{
    uint32_t caps = 0;
    for (size_t i = 0; i < s_count; i++) {
        if (s_devs[i]->state == FH1_DEV_READY) caps |= s_devs[i]->caps;
    }
    if (caps != s_caps) {
        s_caps = caps;
        esp_event_post(FH1_EVENT, FH1_EVT_CAPS_CHANGED, &caps, sizeof(caps), 0);
    }
}

static void set_state(fh1_device_t *d, fh1_dev_state_t st, const char *reason)
{
    if (d->state == st && strcmp(d->reason, reason ? reason : "") == 0) return;
    if (d->state != st) {
        ESP_LOGI(TAG, "%s (%s): %s -> %s%s%s", d->id, d->drv->name, fh1_dev_state_name(d->state),
                 fh1_dev_state_name(st), reason && *reason ? ", " : "", reason ? reason : "");
    }
    d->state = st;
    strlcpy(d->reason, reason ? reason : "", sizeof(d->reason));
    s_gen++;
    for (int i = 0; i < (int)s_count; i++) {
        if (s_devs[i] == d) {
            esp_event_post(FH1_EVENT, FH1_EVT_DEVICE_CHANGED, &i, sizeof(i), 0);
            break;
        }
    }
    recompute_caps();
}

void fh1_dev_register(fh1_device_t *d)
{
    if (!s_lock) s_lock = xSemaphoreCreateRecursiveMutex();
    lock();
    if (s_count < MAX_DEVICES) {
        d->state = FH1_DEV_ABSENT;
        s_devs[s_count++] = d;
    } else {
        ESP_LOGE(TAG, "registry full, %s dropped", d->id);
    }
    unlock();
}

bool fh1_dev_enabled_setting(const fh1_device_t *d)
{
    if (d->essential) return true;
    return fh1_settings_get_u8("devices", d->id, d->default_on ? 1 : 0) != 0;
}

// Probe, then start. Leaves the device ready, absent or failed.
static void bring_up(fh1_device_t *d)
{
    d->info[0] = 0;
    esp_err_t err = d->drv->probe ? d->drv->probe(d) : ESP_OK;
    if (err != ESP_OK) {
        set_state(d, FH1_DEV_ABSENT, err == ESP_ERR_NOT_FOUND ? "not found" : esp_err_to_name(err));
        return;
    }
    err = d->drv->start ? d->drv->start(d) : ESP_OK;
    if (err == ESP_ERR_NOT_SUPPORTED) {
        set_state(d, FH1_DEV_FAILED, "no driver yet");
    } else if (err != ESP_OK) {
        set_state(d, FH1_DEV_FAILED, esp_err_to_name(err));
    } else {
        set_state(d, FH1_DEV_READY, "");
    }
}

static void poll_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        lock();
        for (size_t i = 0; i < s_count; i++) {
            fh1_device_t *d = s_devs[i];
            if (d->state != FH1_DEV_DISABLED && d->drv->poll) d->drv->poll(d);
        }
        unlock();
    }
}

void fh1_dev_start_all(void)
{
    lock();
    for (size_t i = 0; i < s_count; i++) {
        fh1_device_t *d = s_devs[i];
        if (!fh1_dev_enabled_setting(d)) {
            if (d->drv->stop) d->drv->stop(d);
            set_state(d, FH1_DEV_DISABLED, "switched off");
        } else {
            bring_up(d);
        }
    }
    unlock();
    xTaskCreatePinnedToCore(poll_task, "dev_poll", 4096, NULL, 2, NULL, 0);
}

esp_err_t fh1_dev_set_enabled(const char *id, bool enabled)
{
    fh1_device_t *d = fh1_dev_find(id);
    if (!d) return ESP_ERR_NOT_FOUND;
    if (d->essential && !enabled) return ESP_ERR_NOT_ALLOWED;
    lock();
    fh1_settings_set_u8("devices", d->id, enabled ? 1 : 0);
    if (!enabled && d->state != FH1_DEV_DISABLED) {
        if (d->state == FH1_DEV_READY && d->drv->stop) d->drv->stop(d);
        set_state(d, FH1_DEV_DISABLED, "switched off");
    } else if (enabled && d->state == FH1_DEV_DISABLED) {
        bring_up(d);
    }
    unlock();
    return ESP_OK;
}

esp_err_t fh1_dev_reprobe(const char *id)
{
    fh1_device_t *d = fh1_dev_find(id);
    if (!d) return ESP_ERR_NOT_FOUND;
    if (d->state == FH1_DEV_DISABLED) return ESP_ERR_INVALID_STATE;
    lock();
    if (d->state == FH1_DEV_READY && d->drv->stop) d->drv->stop(d);
    bring_up(d);
    unlock();
    return ESP_OK;
}

void fh1_dev_report(fh1_device_t *d, fh1_dev_state_t state, const char *reason)
{
    lock();
    if (d->state != FH1_DEV_DISABLED) set_state(d, state, reason);
    unlock();
}

size_t fh1_dev_count(void) { return s_count; }
fh1_device_t *fh1_dev_at(size_t i) { return i < s_count ? s_devs[i] : NULL; }
uint32_t fh1_caps(void) { return s_caps; }
uint32_t fh1_dev_generation(void) { return s_gen; }

fh1_device_t *fh1_dev_find(const char *id)
{
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_devs[i]->id, id) == 0) return s_devs[i];
    }
    return NULL;
}

fh1_device_t *fh1_dev_ready_of_class(const char *cls)
{
    for (size_t i = 0; i < s_count; i++) {
        if (s_devs[i]->state == FH1_DEV_READY && strcmp(s_devs[i]->cls, cls) == 0) return s_devs[i];
    }
    return NULL;
}
