#include <string.h>
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
