// Doom's memory: every block in PSRAM and every one freed when the game closes (doom_mem.c). Included ahead of every
// source of the package (CMakeLists.txt: -include), so PrBoom's malloc, calloc, realloc, strdup and free are these.
#pragma once
#include <stddef.h>

void *doom_malloc(size_t size);
void *doom_calloc(size_t n, size_t size);
void *doom_realloc(void *p, size_t size);
char *doom_strdup(const char *s);
void doom_free(void *p);
void doom_mem_free_all(void);                       // at the end: whatever the game still holds
size_t doom_mem_in_use(void);                       // bytes held now (for the log)
size_t doom_mem_largest(void);                      // the largest block of PSRAM to be had now

#define malloc doom_malloc
#define calloc doom_calloc
#define realloc doom_realloc
#define strdup doom_strdup
#define free doom_free
