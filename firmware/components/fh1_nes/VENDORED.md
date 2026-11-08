# Vendored code

`nofrendo/`: the Nofrendo NES emulator by Matthew Conte and Neil Stevens, as maintained in retro-go by
Alex Duchesne (https://github.com/ducalex/retro-go, `retro-core/components/nofrendo`, commit
`4ced120669750ca7228fd0414211430c1d923166`). Licence: GNU GPL version 2 (`nofrendo/COPYING`; the original
file headers say GNU Library GPL version 2). The documentation folder and its test ROM archive are left out.

Changes, each marked "FH1 change":
- `nes/nes.h`, `nes/nes.c`: `apu_mix` flag; without a sound device the APU's samples are not mixed (its
  registers and interrupts still run).
- `nes/mem.c`, `nes/apu.c`, `nes/ppu.c`: the CPU memory map, APU and PPU state are allocated by their init
  functions and freed at shutdown instead of being static, so they take no internal RAM while no NES game
  is open.
- All allocations go through `nes_alloc.h` (forced include, see `CMakeLists.txt`): internal RAM when there
  is room above the reserve (`fh1_mem.h`), PSRAM otherwise.
