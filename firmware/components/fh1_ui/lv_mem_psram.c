// LVGL's heap, in PSRAM. Everything the window system allocates (objects, styles, texts, image caches)
// lives there, so internal RAM stays free for Wi-Fi and for a full-screen app's hot data: the interface is
// not on screen while such an app runs, and drawing it from PSRAM is fast enough otherwise. Only the draw
// buffer is internal, and it is given back while the interface is paused (fh1_ui_pause).
#include "lvgl.h"
#include "esp_heap_caps.h"

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}
lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) { return NULL; }
void lv_mem_remove_pool(lv_mem_pool_t pool) {}
void *lv_malloc_core(size_t size) { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM); }
void *lv_realloc_core(void *p, size_t new_size) { return heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM); }
void lv_free_core(void *p) { heap_caps_free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t *mon)
{
    multi_heap_info_t i;
    heap_caps_get_info(&i, MALLOC_CAP_SPIRAM);
    mon->total_size = i.total_free_bytes + i.total_allocated_bytes;
    mon->free_size = i.total_free_bytes;
    mon->free_biggest_size = i.largest_free_block;
    mon->used_cnt = i.allocated_blocks;
    mon->free_cnt = i.free_blocks;
    mon->max_used = mon->total_size - i.minimum_free_bytes;
    mon->used_pct = mon->total_size ? (uint8_t)(100 - 100ULL * mon->free_size / mon->total_size) : 0;
    mon->frag_pct = mon->free_size ? (uint8_t)(100 - 100ULL * mon->free_biggest_size / mon->free_size) : 0;
}

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }
