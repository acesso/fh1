# Vendored code

`smsplus/`: SMS Plus by Charles MacDonald, as maintained in retro-go by Alex Duchesne
(https://github.com/ducalex/retro-go, `retro-core/components/smsplus`, commit
`4ced120669750ca7228fd0414211430c1d923166`). Licence: GNU GPL version 2 (`smsplus/COPYING`).

Changes, each marked "FH1 change":
- `render.c`, `tms.c`, `tms.h`, `sms.c`: the render lookup tables, the TMS tables and the Z80 flag tables
  are freed at shutdown (they leaked ~576 KB of PSRAM per game opened).
- `loadrom.c`: the cartridge RAM (32 KB, rarely touched) is allocated with plain `calloc` (PSRAM), so it
  does not take internal RAM before the hot work and video RAM.
- `sound/sound.c`: a sample rate of 0 leaves the sound chips off (no sound device) instead of aborting.
- `render.c`: colours are plain RGB565; retro-go swapped their bytes for its display, which made every
  colour wrong on the FH1.
- All allocations go through `sms_alloc.h` (forced include, see `CMakeLists.txt`): internal RAM when there
  is room above the reserve (`fh1_mem.h`), PSRAM otherwise.
