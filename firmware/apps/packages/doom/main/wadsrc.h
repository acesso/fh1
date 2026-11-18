// Where Doom reads its WADs from: a file (the SD card, internal storage, the package's assets) or a computer on the
// network serving it over HTTP (http://192.168.0.2:8077/freedoom1.wad: firmware/tools/fh1wadserve.py), read a block at a
// time when the game first needs it and kept in a cache in PSRAM. A WAD of 28 MB plays on a unit with no SD card.
#pragma once
#include <stddef.h>
#include <stdbool.h>

void *wadsrc_open(const char *name, size_t *size);     // NULL if it cannot be read
bool wadsrc_read(void *src, size_t offset, void *dest, size_t size);
void wadsrc_close(void *src);                          // its connection or file closed, its cached blocks freed
void wadsrc_close_all(void);                           // when the game closes: every source and the cache
// Network reads since the start: requests, bytes, milliseconds spent waiting (for the log).
void wadsrc_stats(unsigned *requests, unsigned *kbytes, unsigned *ms);
size_t wadsrc_cache_bytes(void);                       // its blocks in memory now
