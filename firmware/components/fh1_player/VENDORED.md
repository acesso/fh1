# Vendored code in fh1_player

Everything under `vendor/` comes from other projects; our own code is `src/`, `include/` and `shim/`. The
player as a whole is described in `firmware/PLAN.md`, section "Music".

| Directory | From | Version | Licence |
|---|---|---|---|
| `vendor/minimp3` | minimp3 (https://github.com/lieff/minimp3), `minimp3.h` | `ea99364` (2022-06-25) | CC0 (`LICENSE`) |
| `vendor/dr_libs` | dr_libs (https://github.com/mackron/dr_libs), `dr_wav.h`, `dr_flac.h` | `35b5c39` (2026-10-30) | public domain or MIT-0 (end of each file) |
| `vendor/pocketmod` | pocketmod (https://github.com/rombankzero/pocketmod), `pocketmod.h` | `33ac2ba` (2018-06-14) | MIT (end of the file) |
| `vendor/tremor` | cspot's bell, `external/tremor` (https://github.com/feelfreelinux/cspot) | cspot `1b07a9c`, bell `e837373` | BSD (`COPYING`) |

All permissive: the firmware's licence is not affected.

## FH1 changes to vendored files

Each change is marked `FH1` in the file.

- **minimp3:** its ~16 KB scratch structure is where `MINIMP3_SCRATCH` points (`src/dec_mp3.c` allocates it
  in PSRAM) instead of on the decoding task's stack.
- **dr_wav:** its stack buffers are smaller (4096 bytes became 1024 for sample conversion, 512 for reading
  ahead and measuring an unsized data chunk); they are loops over the data, so only the step changes.
- **Tremor:** unchanged; built with `shim/pl_alloc.h` force-included, so its allocations go to PSRAM first.
- Removed from Tremor: `ivorbisfile_example.c`.
