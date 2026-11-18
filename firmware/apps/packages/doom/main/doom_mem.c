// Doom's memory (doom_mem.h). The firmware keeps blocks under 4 KB in internal RAM, which Wi-Fi needs: PrBoom makes
// thousands of small ones (things, strings, the zone's pieces) and filled it, and the network stopped. It also never
// frees what it holds when it quits, and an app's leftovers stay taken until the unit restarts. So each block goes
// to PSRAM, with a header that links it into a list, and the whole list is freed when the game closes.
#include "doom_mem.h"
#undef malloc                                        // (the header comes first in every source, this one too)
#undef calloc
#undef realloc
#undef strdup
#undef free
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "esp_heap_caps.h"
#include "fh1_sdk.h"

#define MAGIC 0xD00Du
#define CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define RESERVE (384 * 1024)        // PSRAM left to the firmware: past that a block fails and the zone frees its cache

typedef struct hdr {
    struct hdr *prev, *next;
    uint32_t size;
    uint32_t magic;                                  // (16 bytes: the block after it stays 8-aligned)
} hdr_t;

static hdr_t *s_list;
static size_t s_in_use;

static void link_in(hdr_t *h)
{
    h->prev = NULL;
    h->next = s_list;
    if (s_list) s_list->prev = h;
    s_list = h;
    s_in_use += h->size;
}

static void unlink_out(hdr_t *h)
{
    if (h->prev) h->prev->next = h->next;
    else s_list = h->next;
    if (h->next) h->next->prev = h->prev;
    s_in_use -= h->size;
}

static bool room_for(size_t size)
{
    return heap_caps_get_free_size(CAPS) >= sizeof(hdr_t) + size + RESERVE;
}

// The largest block of PSRAM that can be had now (by trying)
size_t doom_mem_largest(void)
{
    size_t lo = 0, hi = heap_caps_get_free_size(CAPS);
    while (hi - lo > 4096) {
        const size_t mid = (lo + hi) / 2;
        void *p = heap_caps_malloc(mid, CAPS);
        if (p) heap_caps_free(p), lo = mid;
        else hi = mid;
    }
    return lo;
}

// A big block refused: why (the log). The largest block found by trying (only then: a few allocations).
static void why_not(size_t size, bool reserve)
{
    static int logged;
    if (size < 64 * 1024 || reserve || logged >= 3) return;   // (the reserve reached: the zone frees its cache, as meant)
    logged++;
    size_t lo = 0, hi = heap_caps_get_free_size(CAPS);
    while (hi - lo > 4096) {
        const size_t mid = (lo + hi) / 2;
        void *p = heap_caps_malloc(mid, CAPS);
        if (p) heap_caps_free(p), lo = mid;
        else hi = mid;
    }
    fh1_log("doom: %u KB refused (%s): PSRAM %u KB free, the largest block %u KB; Doom holds %u KB", (unsigned)(size / 1024),
            reserve ? "the firmware's reserve" : "no block that big", (unsigned)(heap_caps_get_free_size(CAPS) / 1024),
            (unsigned)(lo / 1024), (unsigned)(s_in_use / 1024));
    static int dumped;
    if (!reserve && !dumped++) {
        extern void Z_DumpPlaces(void);
        fh1_log("doom: PSRAM blocks of Doom from %p; the zone by place:", (void *)s_list);
        Z_DumpPlaces();
    }
}

void *doom_malloc(size_t size)
{
    if (!room_for(size)) {
        why_not(size, true);
        return NULL;
    }
    hdr_t *h = heap_caps_malloc(sizeof(hdr_t) + size, CAPS);
    if (!h) {
        why_not(size, false);
        return NULL;
    }
    h->size = (uint32_t)size;
    h->magic = MAGIC;
    link_in(h);
    return h + 1;
}

void *doom_calloc(size_t n, size_t size)
{
    void *p = doom_malloc(n * size);
    if (p) memset(p, 0, n * size);
    return p;
}

void *doom_realloc(void *p, size_t size)
{
    if (!p) return doom_malloc(size);
    if (!size) {
        doom_free(p);
        return NULL;
    }
    hdr_t *h = (hdr_t *)p - 1;
    if (size > h->size && !room_for(size - h->size)) return NULL;
    unlink_out(h);
    hdr_t *n = heap_caps_realloc(h, sizeof(hdr_t) + size, CAPS);
    if (!n) {
        link_in(h);                                  // (the old block is still there)
        return NULL;
    }
    n->size = (uint32_t)size;
    link_in(n);
    return n + 1;
}

char *doom_strdup(const char *s)
{
    const size_t n = strlen(s) + 1;
    char *d = doom_malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

void doom_free(void *p)
{
    if (!p) return;
    hdr_t *h = (hdr_t *)p - 1;
    if (h->magic != MAGIC) {                         // the firmware's (none expected): its own free
        free(p);
        return;
    }
    h->magic = 0;
    unlink_out(h);
    heap_caps_free(h);
}

void doom_mem_free_all(void)
{
    while (s_list) {
        hdr_t *h = s_list;
        s_list = h->next;
        h->magic = 0;
        heap_caps_free(h);
    }
    s_in_use = 0;
}

size_t doom_mem_in_use(void) { return s_in_use; }
