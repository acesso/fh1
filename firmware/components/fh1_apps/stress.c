// A stress test for the display path (dev tool, started from the console): PSRAM copies on both cores,
// flash writes, and the whole UI redrawn non-stop, for a few seconds. The panel's health numbers
// (fh1_board_display_stats, in the console stats) show whether it kept being fed in time.
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include "apps_priv.h"
#include "fh1_ui.h"
#include "fh1_board.h"
#include "fh1_storage.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "stress";
static volatile bool s_running;
static volatile int s_tasks;
static lv_timer_t *s_redraw;

static void psram_task(void *arg)
{
    const size_t n = 128 * 1024;
    uint8_t *a = heap_caps_malloc(n, MALLOC_CAP_SPIRAM), *b = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    uint32_t i = 0;
    while (s_running && a && b) {
        memcpy(b, a, n);
        memset(a, (int)i, n);
        if (++i % 4 == 0) vTaskDelay(1);
    }
    heap_caps_free(a);
    heap_caps_free(b);
    s_tasks--;
    vTaskDelete(NULL);
}

static void flash_task(void *arg)
{
    uint8_t *buf = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL);
    while (s_running && buf) {
        FILE *f = fopen(FH1_INT_PATH "/stress.tmp", "wb");
        for (int k = 0; f && k < 16 && s_running; k++) {
            memset(buf, k, 4096);
            fwrite(buf, 1, 4096, f);
        }
        if (f) fclose(f);
        unlink(FH1_INT_PATH "/stress.tmp");
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    heap_caps_free(buf);
    s_tasks--;
    vTaskDelete(NULL);
}

static void redraw(lv_timer_t *t) { lv_obj_invalidate(lv_screen_active()); }

static void stop_redraw(void *arg)
{
    if (s_redraw) lv_timer_delete(s_redraw);
    s_redraw = NULL;
}

static void stop_timer_cb(void *arg)
{
    s_running = false;
    fh1_ui_async(stop_redraw, NULL);
    while (s_tasks > 0) vTaskDelay(pdMS_TO_TICKS(20));
    fh1_display_stats_t st;
    fh1_board_display_stats(&st, false);
    ESP_LOGI(TAG, "done: frames %lu, late fills %lu, odd frames %lu, longest fill %lu us, frame period %lu-%lu us",
             (unsigned long)st.frames, (unsigned long)st.late_fills, (unsigned long)st.odd_frames,
             (unsigned long)st.max_fill_us, (unsigned long)st.vsync_min_us, (unsigned long)st.vsync_max_us);
    vTaskDelete(NULL);
}

static void stopper(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS((int)(intptr_t)arg * 1000));
    stop_timer_cb(NULL);
}

// Runs in the UI task.
esp_err_t stress_start(int seconds, bool psram, bool flash, bool ui)
{
    if (s_running) return ESP_ERR_INVALID_STATE;
    fh1_display_stats_t st;
    fh1_board_display_stats(&st, true);
    s_running = true;
    s_tasks = 0;
    if (psram) {
        s_tasks += 2;
        xTaskCreatePinnedToCore(psram_task, "stress_ps0", 3072, NULL, 2, NULL, 0);
        xTaskCreatePinnedToCore(psram_task, "stress_ps1", 3072, NULL, 2, NULL, 1);
    }
    if (flash) {
        s_tasks++;
        xTaskCreatePinnedToCore(flash_task, "stress_fl", 4096, NULL, 2, NULL, 0);
    }
    if (ui) s_redraw = lv_timer_create(redraw, 30, NULL);
    xTaskCreate(stopper, "stress_stop", 3072, (void *)(intptr_t)seconds, 3, NULL);
    ESP_LOGI(TAG, "start: %d s, psram %d, flash %d, ui %d", seconds, psram, flash, ui);
    return ESP_OK;
}
