// Force-included into the vendored decoders (CMakeLists.txt): their allocations go to PSRAM first, so a
// playing track takes no internal RAM beyond the player's task and its read buffer.
#pragma once
#include <stdlib.h>
#include <string.h>
void *pl_malloc(size_t n);
void *pl_calloc(size_t n, size_t size);
void *pl_realloc(void *p, size_t n);
void pl_free(void *p);
#define malloc(n) pl_malloc(n)
#define calloc(n, s) pl_calloc(n, s)
#define realloc(p, n) pl_realloc(p, n)
#define free(p) pl_free(p)
