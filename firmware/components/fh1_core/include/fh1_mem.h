// Memory for the running full-screen app. Internal RAM is several times faster than PSRAM for the data an
// app touches all the time (an emulated machine's RAM, its CPU state). While such an app runs, the window
// system keeps nothing in internal RAM (fh1_ui_pause), so the app gets what is left above a reserve that
// Wi-Fi and the system need. One full-screen app at a time.
#pragma once
#include <stddef.h>
#include <stdbool.h>

#define FH1_MEM_RESERVE (32 * 1024)     // internal RAM always left free (Wi-Fi bursts, the app's own tasks)

// Internal RAM when that leaves the reserve free, else PSRAM; NULL only when both are out. Free with
// fh1_mem_free (or heap_caps_free).
void *fh1_mem_fast(size_t size);
void *fh1_mem_fast_calloc(size_t n, size_t size);
void fh1_mem_free(void *p);
bool fh1_mem_is_fast(const void *p);
// Logs internal RAM: free, largest block, lowest ever.
void fh1_mem_log(const char *what);
