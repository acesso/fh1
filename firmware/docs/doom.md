# Doom

PrBoom (retro-go's prboom-go 2.5.0, GPL-2.0+) as an FH1 package: [`apps/packages/doom`](../apps/packages/doom).
The game is PrBoom's own code in `main/prboom/` (changes marked `FH1_DOOM`). The FH1 side is three files:
- [`fh1_doom.c`](../apps/packages/doom/main/fh1_doom.c): video, sound, input, the WAD's place, dev commands;
- [`wadsrc.c`](../apps/packages/doom/main/wadsrc.c): WADs from a file or over HTTP;
- [`doom_mem.c`](../apps/packages/doom/main/doom_mem.c): memory.

It needs SDK 0.10.

## Playing

**The WAD:**
- **Where Doom looks:** the first IWAD found in `/sd/wads`, then `/int/wads`. The web console's Upload puts a
  `.wad` file there (up to 64 MB). Freedoom's `freedoom1.wad` (28 MB) or any Doom, Doom II, TNT or Plutonia IWAD
  works.
- **Choosing one:** `wad.txt` in the app's data folder, or the dev command `wad <file or URL>`, names another WAD.
- **Without a WAD,** the screen says where to put one.

**Controls:**

| Key | Action |
|---|---|
| D-pad, or the left stick | Walk and turn |
| A | Fire, and choose in menus |
| B | Use and open, and go back in menus |
| X | Run |
| Y | Next weapon |
| L1 / R1 | Strafe |
| Start | Doom's menu |
| Select | The map |

**A unit with no SD card** reads its WAD over the network from a computer:
- On the computer: `tools/fh1wadserve.py DIR`. It serves the WADs in DIR on port 8077 and prints their addresses.
- On the FH1: `wad http://<computer's IP>:8077/freedoom1.wad`.
- How it reads: HTTP Range requests on one kept-alive connection, in 16 KB blocks, with 8 blocks cached. The game
  holds what it reads in its own cache too.
- Each graphic the cache dropped is fetched again, so the network's speed sets the frame rate (8 to 20 fps on a
  unit with a weak signal).

## How it runs

- **Frames:** one game step a frame, on the SDK's full screen in 256 colours (`fh1_screen_open8`).
  - PrBoom draws straight into the firmware's picture, which is shown through the game's palette on core 0 while
    the next frame is drawn.
  - Only the status bar's rows are copied between the two pictures (the 3D view is drawn whole every frame).
  - Wipes melt a step a frame.
- **Exits and fatal errors:** they return to `frame()` through a long jump; an error is shown on the screen and
  logged.
- **Memory:** every block is in PSRAM, tracked, and given back when the game closes (PrBoom frees little itself).
  - 384 KB of PSRAM stays free for the firmware: past that an allocation fails, and PrBoom's zone frees its
    least recently used graphics.
  - **The tables** (blocks with no owner, which nothing purges: the lump list, the renderer's visplanes, wall and
    sprite lists, strings) live in an arena of 1 MB taken at the start ([`fh1_arena.c`](../apps/packages/doom/main/fh1_arena.c)).
    The renderer's lists are made there at the start, at their usual sizes (visplanes in 16 bits a row).
  - **Levels:** sounds are stopped and their lumps given back as a level starts; freeing the last level and the
    cache then leaves the rest of the heap in large runs for the new level's tables (a big map's lines take
    274 KB in one block). Each level's start logs PSRAM free, its largest block, and the arena's use.
  - Graphics are loaded when first drawn (no level precache).
  - Sound effects play from their lumps (8-bit, `fh1_sfx_play8`), held while they play.
- **Stack:** the app's 8 KB stack fits.
  - The BSP walk is a loop with a list of nodes, not recursion.
  - The sprite clipping arrays and the state table of `P_SetMobjState` are off the stack.
- **Music:** none.

**Speed:** Freedoom's demos timed (`timedemo DEMO 2000`: the first 2000 tics as fast as they draw;
`tools/fh1doombench.py`). Normal play is capped by the game's 35 tics a second.

| Demo | Guition (WAD on the SD card) | Radio unit (WAD over Wi-Fi) |
|---|---|---|
| demo1 | 31 fps | 28 fps |
| demo2 | 34 fps | 21–22 fps |
| demo3 | 14 fps | under 1 fps after demo2; 12 fps from a fresh start |
| demo4 | 32 fps | — |

demo3's map is the largest: its level takes 1.4 MB, which leaves the cache too little for its walls, so textures
are built again every frame; over Wi-Fi each one is fetched again too (35 MB in 2000 tics). A frame of demo1 on
the Guition:

| Part | ms a frame |
|---|---|
| Walls | 12–17 (the columns themselves 5–6) |
| Sprites and see-through walls | 4–10 |
| The game's tic | 5–6 |
| Floors and ceilings | 3.5–5 |

What weighs the most is memory. The code and data are in PSRAM, behind a cache shared by both cores, and graphics
dropped from the game's cache come back from the SD card (fast seek keeps that short).

## Development

The dev console's `op dev "<command>"`:

| Command | What |
|---|---|
| `wad <file or URL>` | The WAD (while the screen asks for one); kept in `wad.txt` |
| `fps` | Every 5 s: frames a second, and where a frame's time goes (game, walls, planes, sprites, WAD reads...) |
| `mem` | Memory: the zone by tag, by block size, its biggest blocks |
| `net` | Network reads so far (requests, KB, ms waiting) |
| `timedemo [demo] [tics]` | A demo a tic a frame, as fast as it draws; with tics, the time of its first tics in the log |

**Benchmarks:** `tools/fh1doombench.py [--wad URL] [--install doom_X.fpk] DEMO:TICS ...` runs the timed demos on a
unit and watches them. It stops as soon as the game stalls, falls under 1 fps, logs an error, or the unit restarts,
and it then saves the log, a screenshot and the core dump if there is one.

Build with `tools/pkgbuild.sh apps/packages/doom`. For crash addresses, configure once with
`idf.py -DELF_LOADER_KEEP_DEBUG_ELF=ON reconfigure` ([`sdk.md`](sdk.md), "Debugging").
