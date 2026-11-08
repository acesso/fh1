// Included first in every nofrendo source (CMakeLists.txt): its malloc/calloc/free take the fastest memory
// with room (fh1_mem.h). They run in the order the emulator starts up: CPU and PPU tables, then the
// cartridge's RAM; big blocks (the ROM image) end up in PSRAM.
#pragma once
#include <stdlib.h>
#include <string.h>
#include "fh1_mem.h"
#define malloc(n) fh1_mem_fast(n)
#define calloc(n, s) fh1_mem_fast_calloc(n, s)
#define free(p) fh1_mem_free(p)
