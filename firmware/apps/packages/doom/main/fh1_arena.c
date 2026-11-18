// Doom's arena for its tables (fh1_arena.h): the static blocks with no owner (tables, visplanes, the renderer's lists,
// strings), which nothing ever purges. PrBoom's zone (z_zone.c) gives every block to malloc, and a few MB of PSRAM
// ended up in pieces: tables made during play sat between the levels' and the cached graphics' blocks, and a big
// level's line table (274 KB) found no block that big with 1.5 MB free. With the tables here, a level's start (which
// frees the last level and the cache) leaves the rest of the heap in large runs. The allocator is id Software's
// Doom 1.10 zone (GPL-2.0+) without purging: first fit from a rover, freed blocks joined to their free neighbours.
#include "fh1_arena.h"
#include "lprintf.h"
#include "fh1_sdk.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdint.h>

#define ALIGN 8
#define MINFRAGMENT 64
#define CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

typedef struct memblock {
  size_t size;                 // with this header
  int used;
  unsigned id;
  struct memblock *next, *prev;
} memblock_t;

#define ARENAID 0x4a7e1d
#define HEADER ((sizeof(memblock_t) + ALIGN - 1) & ~(size_t)(ALIGN - 1))

static char *s_arena;
static size_t s_size, s_used;
static memblock_t s_list;          // the list's head: never free
static memblock_t *s_rover;

bool arena_init(size_t size)
{
  if (s_arena) return true;
  size &= ~(size_t)(ALIGN - 1);
  s_arena = heap_caps_malloc(size, CAPS);
  if (!s_arena) return false;
  s_size = size, s_used = 0;
  memblock_t *b = (memblock_t *)s_arena;
  s_list.next = s_list.prev = b;
  s_list.used = 1;
  s_list.size = 0;
  b->prev = b->next = &s_list;
  b->size = size;
  b->used = 0;
  b->id = 0;
  s_rover = b;
  return true;
}

void arena_close(void)
{
  if (s_arena) heap_caps_free(s_arena);
  s_arena = NULL;
  s_size = s_used = 0;
}

bool arena_has(const void *p) { return s_arena && (const char *)p >= s_arena && (const char *)p < s_arena + s_size; }
size_t arena_size(void) { return s_size; }
size_t arena_used(void) { return s_used; }

void arena_free(void *p)
{
  memblock_t *block = (memblock_t *)((char *)p - HEADER), *other;
  if (block->id != ARENAID)
    I_Error("arena_free: not a block of the arena");
  s_used -= block->size;
  block->used = 0;
  block->id = 0;
  other = block->prev;
  if (!other->used) {                              // joined to the free block before it
    other->size += block->size;
    other->next = block->next;
    other->next->prev = other;
    if (block == s_rover) s_rover = other;
    block = other;
  }
  other = block->next;
  if (!other->used) {                              // and to the one after
    block->size += other->size;
    block->next = other->next;
    block->next->prev = block;
    if (other == s_rover) s_rover = block;
  }
}

void *arena_alloc(size_t size)
{
  if (!s_arena || !size) return NULL;
  size = ((size + ALIGN - 1) & ~(size_t)(ALIGN - 1)) + HEADER;
  memblock_t *b = s_rover;
  do {                                             // first fit, from the rover round
    if (!b->used && b->size >= size) break;
    b = b->next;
  } while (b != s_rover);
  if (b->used || b->size < size) return NULL;      // full: the caller uses the heap
  const size_t extra = b->size - size;
  if (extra > MINFRAGMENT) {
    memblock_t *rest = (memblock_t *)((char *)b + size);
    rest->size = extra;
    rest->used = 0;
    rest->id = 0;
    rest->prev = b;
    rest->next = b->next;
    rest->next->prev = rest;
    b->next = rest;
    b->size = size;
  }
  b->used = 1;
  b->id = ARENAID;
  s_used += b->size;
  s_rover = b->next;
  return (char *)b + HEADER;
}
