#include <string.h>
#include "sdkconfig.h"
#include "fh1_mem.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_log.h"

static const char *TAG = "mem";

void *fh1_mem_fast(size_t size)
{
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    if (heap_caps_get_free_size(caps) >= size + FH1_MEM_RESERVE && heap_caps_get_largest_free_block(caps) >= size) {
        void *p = heap_caps_malloc(size, caps);
        if (p) return p;
    }
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
}

void *fh1_mem_fast_calloc(size_t n, size_t size)
{
    void *p = fh1_mem_fast(n * size);
    if (p) memset(p, 0, n * size);
    return p;
}

void fh1_mem_free(void *p) { heap_caps_free(p); }

bool fh1_mem_is_fast(const void *p) { return p && esp_ptr_internal(p); }

void fh1_mem_log(const char *what)
{
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    ESP_LOGI(TAG, "%s: internal %u KB free, largest block %u KB, lowest %u KB", what,
             (unsigned)(heap_caps_get_free_size(caps) / 1024), (unsigned)(heap_caps_get_largest_free_block(caps) / 1024),
             (unsigned)(heap_caps_get_minimum_free_size(caps) / 1024));
}

// ── Where internal RAM goes ──
// Every allocation that failed is counted and the last four kept (size, caps, task, when): Wi-Fi borrowing internal
// RAM in a burst shows here first. The console's "heap" op reports them with the free and lowest internal RAM.

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

static struct {
    uint32_t fails;
    struct { uint32_t size, caps, ms; char task[16]; } fail[4];
} s_diag;
static portMUX_TYPE s_diag_mux = portMUX_INITIALIZER_UNLOCKED;

static void failed_alloc(size_t size, uint32_t caps, const char *fn)
{
    // Any context: only copy into the ring.
    TaskHandle_t t = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL_SAFE(&s_diag_mux);
    const uint32_t k = s_diag.fails++ % 4;
    s_diag.fail[k].size = size;
    s_diag.fail[k].caps = caps;
    s_diag.fail[k].ms = (uint32_t)(esp_timer_get_time() / 1000);
    strlcpy(s_diag.fail[k].task, t ? pcTaskGetName(t) : "?", sizeof(s_diag.fail[k].task));
    portEXIT_CRITICAL_SAFE(&s_diag_mux);
}

void fh1_mem_diag_start(void) { heap_caps_register_failed_alloc_callback(failed_alloc); }

void fh1_mem_diag_report(char *out, size_t n, bool reset)
{
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    int k = snprintf(out, n, "internal now %u B (largest %u B), lowest ever %u B; %u allocations failed",
                     (unsigned)heap_caps_get_free_size(caps), (unsigned)heap_caps_get_largest_free_block(caps),
                     (unsigned)heap_caps_get_minimum_free_size(caps), (unsigned)s_diag.fails);
    for (uint32_t i = 0; i < 4 && i < s_diag.fails && k < (int)n; i++) {
        const uint32_t j = (s_diag.fails - 1 - i) % 4;
        k += snprintf(out + k, n - k, " [%u B caps %x %s @%u ms]", (unsigned)s_diag.fail[j].size, (unsigned)s_diag.fail[j].caps,
                      s_diag.fail[j].task, (unsigned)s_diag.fail[j].ms);
    }
    if (reset) s_diag.fails = 0;
}
