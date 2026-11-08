#include <string.h>
#include "fh1_events.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_log.h"
#include "freertos/task.h"

#define MAX_SUBS 16
#define MAX_RECURRING 8
#define DATA_MAX 64

typedef struct {
    uint8_t id;
    uint8_t len;
    uint8_t data[DATA_MAX];
} ev_t;

static QueueHandle_t s_q;
static struct { fh1_event_id_t id; fh1_event_cb cb; void *user; } s_subs[MAX_SUBS];
static int s_nsubs;
static struct { fh1_event_id_t id; uint32_t min_ms, max_ms; esp_timer_handle_t t; } s_rec[MAX_RECURRING];
static int s_nrec;

void fh1_events_init(void)
{
    if (!s_q) s_q = xQueueCreate(24, sizeof(ev_t));
}

esp_err_t fh1_event_post(fh1_event_id_t id, const void *data, size_t len)
{
    if (!s_q || id >= FH1_EV_COUNT || len > DATA_MAX) return ESP_ERR_INVALID_ARG;
    ev_t e = { .id = id, .len = len };
    if (len) memcpy(e.data, data, len);
    return xQueueSend(s_q, &e, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t fh1_event_subscribe(fh1_event_id_t id, fh1_event_cb cb, void *user)
{
    if (s_nsubs >= MAX_SUBS) return ESP_ERR_NO_MEM;
    s_subs[s_nsubs].id = id;
    s_subs[s_nsubs].cb = cb;
    s_subs[s_nsubs].user = user;
    s_nsubs++;
    return ESP_OK;
}

static uint32_t delay_for(int i)
{
    uint32_t span = s_rec[i].max_ms - s_rec[i].min_ms;
    return s_rec[i].min_ms + (span ? esp_random() % (span + 1) : 0);
}

static void rec_fire(void *arg)
{
    int i = (int)(intptr_t)arg;
    fh1_event_post(s_rec[i].id, NULL, 0);
    esp_timer_start_once(s_rec[i].t, (uint64_t)delay_for(i) * 1000);
}

esp_err_t fh1_event_every(fh1_event_id_t id, uint32_t min_ms, uint32_t max_ms)
{
    if (s_nrec >= MAX_RECURRING || max_ms < min_ms) return ESP_ERR_INVALID_ARG;
    int i = s_nrec++;
    s_rec[i].id = id;
    s_rec[i].min_ms = min_ms;
    s_rec[i].max_ms = max_ms;
    esp_timer_create_args_t a = { .callback = rec_fire, .arg = (void *)(intptr_t)i, .name = "ev_every" };
    esp_err_t err = esp_timer_create(&a, &s_rec[i].t);
    if (err == ESP_OK) err = esp_timer_start_once(s_rec[i].t, (uint64_t)delay_for(i) * 1000);
    return err;
}

void fh1_events_dispatch(void)
{
    ev_t e;
    while (s_q && xQueueReceive(s_q, &e, 0) == pdTRUE) {
        for (int i = 0; i < s_nsubs; i++) {
            if (s_subs[i].id == e.id) s_subs[i].cb(e.id, e.data, e.len, s_subs[i].user);
        }
    }
}

void fh1_transfer_report(fh1_xfer_kind_t kind, fh1_xfer_state_t state, const char *name, uint64_t done, uint64_t total)
{
    static int last_pct = -1;
    static int64_t last_us;
    fh1_transfer_t t = { .kind = kind, .state = state };
    t.pct = total ? (uint8_t)(done * 100 / total) : 0;
    if (t.pct > 100) t.pct = 100;
    if (state == FH1_XFER_DONE) t.pct = 100;
    if (name) strlcpy(t.name, name, sizeof(t.name));
    int64_t now = esp_timer_get_time();
    if (state == FH1_XFER_RUNNING) {
        // Steps of 2 %, or any change after 250 ms; the first report always.
        bool first = done == 0 || last_pct < 0;
        if (!first && (t.pct == last_pct || (t.pct < last_pct + 2 && now - last_us < 250000))) return;
        last_pct = t.pct;
        last_us = now;
        fh1_event_post(FH1_EV_TRANSFER, &t, sizeof(t));
        return;
    }
    last_pct = -1;
    for (int i = 0; i < 50 && fh1_event_post(FH1_EV_TRANSFER, &t, sizeof(t)) == ESP_ERR_NO_MEM; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
