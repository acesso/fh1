// Doom's arena for its tables (fh1_arena.c says why): one block of PSRAM taken at the start, managed here.
#pragma once
#include <stddef.h>
#include <stdbool.h>

bool arena_init(size_t size);
void arena_close(void);
void *arena_alloc(size_t size);        // NULL when full (the caller falls back to the heap)
void arena_free(void *p);
bool arena_has(const void *p);
size_t arena_size(void);
size_t arena_used(void);
