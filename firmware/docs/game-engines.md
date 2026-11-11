# Game engines for installable games

The goal is an engine that several FH1 games can share. It must fit the FH1's hardware (ESP32-S3, 8 MB PSRAM,
~70 KB of free internal RAM, 320 x 240 or 240 x 240 screen, buttons and gamepads, `fh1_audio`) and its
software: ESP-IDF v6.1 (no Arduino), apps as `.fpk` packages loaded into PSRAM by `elf_loader`, and the SDK as
the only API.

## How packages can share an engine

`elf_loader` 2.1.0 resolves a package's symbols only against the firmware's export table. It can `dlopen` a
shared object and look up names with `dlsym`, but it does not link one loaded module against another (there
is no `DT_NEEDED`). Only one game runs at a time, in PSRAM. That leaves two shapes:

- **Native engine (C or C++), monolithic packages.** The engine is linked into each game's ELF; `--gc-sections`
  keeps only the parts that game uses. Sharing it as its own package would only save storage (tens to a
  hundred KB per game, on a 7.8 MB partition). It would need a stable binary interface between separately built
  modules, which compile-time feature flags break, because they change class layouts.
- **Engine as a runtime, games as data.** The engine is built once (into the firmware as a built-in app, like
  the emulators, or as one package), and a game is a folder of scripts or bytecode plus assets. Nothing is
  duplicated, and games carry no native code. This is the shape that fits "games share the engine's
  modules, and installing a game never reinstalls the engine".

## PixelRoot32 (evaluated, set aside)

[PixelRoot32](https://github.com/PixelRoot32-Game-Engine/PixelRoot32-Game-Engine): C++17, MIT, v1.15.0,
one maintainer, very active. Its NES-style audio core is a separate MIT repository (PixelRoot32-APU).

- **Build world:** PlatformIO and the Arduino core. Its declared dependencies are TFT_eSPI (a fork), U8g2 and
  OneButton, and its ESP32 code paths are selected by `ARDUINO_ARCH_ESP32`.
- **The engine core itself** uses little of Arduino: `millis`, `micros`, `delay`, `yield`, `Serial`,
  `ESP.getFreeHeap`, and `pinMode` / `digitalRead` in its input manager. The display, touch and audio drivers
  are where Arduino and TFT_eSPI live; a `DisplayType::CUSTOM` `DrawSurface` replaces the display driver.
- **Compile test:** with a 30-line shim (an `Arduino.h` on the FH1 SDK, an empty `SPI.h`, a force-included
  header), the engine core and its sprites demo built with ESP-IDF v6.1 into an FH1 package ELF (`-O2`, no
  exceptions, no RTTI). Without audio, physics, UI and particles the demo needed these symbols beyond what the
  SDK exports: `operator new`, `operator delete`, `std::__throw_length_error`, `__assert_func`, `ceilf` and
  `__dso_handle`. The full-module build was not finished.
- **To run it, still needed:**
  - the C++ runtime exports in the firmware;
  - an SDK call to show a full-screen frame;
  - an input adapter (its input is GPIO pins, read with `digitalRead`);
  - an audio backend on `fh1_audio` (its ESP32 backend drives I2S itself, from a task);
  - a game loop driven by the FH1 (its `run()` never returns).
- **Its "modules"** (audio, physics, UI, particles, networking, persistence) are compile-time flags, so the
  engine fits the monolithic shape only.
- **Verdict:** it would run, but it is Arduino- and PlatformIO-first, so the FH1 would carry a permanent port
  layer against an upstream that does not build with ESP-IDF. Set aside for engines made for ESP-IDF.

## Engines made for ESP-IDF

All checked on 2026-11-08. All are young projects with one author.

| Engine | Language, licence | Platform | Shape | Notes |
|---|---|---|---|---|
| [pinyol](https://github.com/ipaud/pinyol) | C11 engine, Lua 5.4 games; MIT (cJSON, Lua, pocketmod MIT; puff zlib; fonts OFL) | ESP-IDF 5.4 on an ESP32-S3 handheld (Cirera: 480 x 320, buttons, speaker, microSD); desktop simulator, browser | Runtime + games as folders (`games/<id>/`: Lua and assets) | The engine (`src/`) has no ESP-IDF or SDL headers: `hal.h` is the whole platform contract. A Game Boy Advance-like virtual console (4 background layers, Mode 7, 128 sprites, palettes, per-line effects), 60 fps claimed on the S3. Data-only adventures too. Tools: asset converters, a block editor. Started 2026-10-14 |
| [DGX-GE](https://github.com/jef-sure/esp32-component-dgx-ge) | C, MIT | ESP-IDF component (5.3+), on the author's DGX display library | Native engine; games mostly data (a pack in flash: pictures, animation scripts, tiles, sounds) plus some C | Indexed frame sent by its changes and scaled; actors run by text animation scripts; mask collisions; sound mixer with chip, NES and OPL2-style synths. Checked on a Prince of Persia remake. Interfaces "will still change". Started 2026-10-18 |
| [WASM-4](https://github.com/aduros/wasm4) on [WAMR](https://github.com/bytecodealliance/wasm-micro-runtime) | Runtime in C; games in any language compiled to WebAssembly; ISC (WASM-4), Apache 2.0 (WAMR) | WAMR supports ESP-IDF; a WASM-4 port to an ESP32-C6 with WAMR exists (MoonBit blog) | Runtime + games as `.wasm` files, sandboxed | A fixed fantasy console: 160 x 160, 4 colours, 64 KB of memory, 4 sound channels. Many existing games. Mature, but the look is limited |
| [32blit SDK](https://github.com/32blit/32blit-sdk) | C++, MIT | Its own handheld, RP2040 (PicoSystem), SDL; no ESP32 port | Native engine, monolithic games | Mature (2019), portable through a HAL, not Arduino. Same C++ runtime needs as PixelRoot32 |

Other projects found, not considered: Deki (an engine with an editor, licence not checked), RGBGameEngine and
Embedded-Game-Engine-for-ESP32 (no licence).

## The FH1 game engine (chosen)

None of the engines above has what the FH1's games need: isometric maps with heights, rich mostly-static
scenery, and colourful characters with 2–3-frame animations, built from existing art packs. The reference is a
tactics game in the style of Final Fantasy Tactics, with simpler rules (to be specified). So the engine is the
FH1's own: C, ESP-IDF, in the firmware, offered to packages through the SDK. It is installed once with the
firmware; a game is a small package (its rules) plus its art pack. It is first built inside a test package, so
it can change without flashing firmware, then moved into the firmware once its API settles.

### Engine

- **Full screen:** a package takes the screen, draws into a back buffer in PSRAM and presents it (SDK 0.5), as
  the emulators and the music deck do inside the firmware.
- **Isometric map:** cells with a height; 2:1 diamonds (32 x 16 top face for 32 x 32 tiles), a height step in
  pixels per map; ground, decoration and object layers. The cell under the cursor is found from the screen,
  heights included.
- **Static scenery drawn once:** the map is drawn into a PSRAM layer, again only when tiles change, the view
  turns or an animated tile's frame changes (only that tile's box); camera moves copy from it. Each frame redraws only what changed: a sprite's old and new rectangles are restored from the
  layer, sprites are drawn in depth order, then the tile parts in front of them are drawn again.
- **Sprites:** RGB565 with 1-bit transparency, trimmed to their pixels, an anchor at the feet; animations of a
  few frames at their own rate (4–8 frames a second), so a mostly static scene costs little CPU.
- **Depth:** by row + column, then height, then layer.
- **Characters:** 4 facings (the map's diagonals on screen); the two facing west are the eastern ones mirrored,
  so art is needed for 2 facings. A unit is a base body plus equipment layers (armour, shield, weapon), each with
  its own frames and a draw order per facing (a shield goes behind the body when the unit faces away). The layers
  are combined into a cached sheet per unit when its equipment changes, not every frame.
- **Around it:** the FH1's gamepads and keys, `fh1_audio`, saves with `fh1_state_save`, windows in the FH1's
  style for menus and unit details.

### Art pipeline (on the PC, when the package is built)

`tools/fh1art.py` turns external art into the game's art pack, following a recipe the game commits (JSON):

- **Inputs:** PNG sprite sheets, separate PNGs and Aseprite files (`.aseprite`, read directly: layers, frame
  tags as animations, cel positions).
- **Steps:** slice, trim, scale down (integer factors pixel-exact; other factors by area, then snapped to the
  source's colours, so pixel art stays crisp), optional palette reduction, anchors, mirrored facings, and the
  equipment layers kept apart.
- **Output:** one binary pack in the package's `assets/`, and preview sheets (PNG) to check by eye.
- **Sources** are in the repository's `assets/packs/<pack>/` (the repository is private), each pack as its
  author ships it. A build writes the SHA-256 of every input next to the pack (`*.sources.json`).

### Art sources

| Pack | What | Price | Licence |
|---|---|---|---|
| [32 x 32 Pixel Isometric Tiles](https://scrabling.itch.io/pixel-isometric-tiles) (scrabling) | Terrain (grass, dirt, flowers, rocks, stumps, water and its edges), separate PNGs and a sheet; animals (boar, stag, badger, wolf) walking in 4 directions, with Aseprite files | Free (pay what you want) | Labelled CC BY 4.0; its text allows use and modification in any project, no credit required, and forbids distributing the files on their own |
| [8 Directional Walk Isometric Pixel animation template](https://banado.itch.io/isometic) (banado) | A human base body walking in 8 directions, 64 x 64 frames (the figure about 32 x 32), its Aseprite file split into part layers (back leg, front leg, back arm, body, front arm, head); four dressed characters (Adventurer, Black Mage, Knight, Warrior) as Aseprite files and sheets. Walk only, plus one standing frame per direction (empty in the Knight and Adventurer files); still in development | $1 ($3 for the characters) | Same text as above |

- Both licences allow the art inside our games, modified, but not the files on their own: they are kept in
  this private repository only, and the converted art travels inside a game's package.
- The base body has only a walk. Idle, attack, hurt and knocked-out frames, and the equipment layers, are drawn
  on top of it in Aseprite. Its 32-pixel figure matches the 32 x 32 tiles, so neither needs scaling.

### Steps

1. **SDK 0.5 full-screen mode** for packages.
2. **`tools/fh1art.py`:** slicing, scaling, Aseprite reading, packs and previews.
3. **Engine test package:** a map with heights from the Scrabling tiles, camera, cursor, an animated animal;
   measured on the FH1 (frame time, PSRAM, internal RAM).
4. **Characters:** the banado base with one armour and one weapon layer, 4 facings by mirroring; memory per unit.
5. **Into the firmware:** the engine as an SDK module, then the game's rules.

### Built so far

- **SDK 0.5:** `fh1_screen_open` / `fh1_screen_close` (full screen: the app's `frame()` on a task of its own on
  core 1, a back buffer in PSRAM, overlays and quitting handled by the firmware), `fh1_app_dir` (the installed
  folder, with the package's `assets/`), `fh1_px_text` (the interface's 8 x 8 font into a full-screen buffer).
- **`tools/fh1art.py`:** `info` (an Aseprite file's layers and tags), `build` (a recipe into a `.fart` pack and a
  preview sheet), `show`. It reads Aseprite files directly (layers in groups, tags, linked cels), skips empty
  frames, scales down, trims and anchors.
- **`apps/packages/isotest`** (Iso test), a small tactics battle with placeholder rules: the engine (`fart.c`:
  packs and drawing, with dimmed and white-flash tints; `iso.c`: the map, the static layer, units with walking
  paths and one-shot animations, depth, occlusion, camera, cursor, tinted cells, the cells a unit can reach
  within a range and a climb limit; `ui.c`: translucent in-game windows, menus, bars, banners) on a 14 x 14
  island, with four heroes (the banado characters) against three wolves (Scrabling's critters: idle, run,
  bite, death).
  - Choosing: the D-pad moves the cursor along the map's axes; X / Y jump to the next / previous hero that can
    still act; A on a hero opens its menu; Start or Menu opens the battle menu (End turn, timings, Leave).
  - The hero's menu: Move (the reachable cells in blue, then A where to go), Attack (the cells in range in red),
    Magic and Items (a list with costs and counts, then a target), Equip (weapon and armour lists), Wait (the
    D-pad turns the hero, A ends its turn). A hero that moved and acted goes straight to Wait. The panel at the
    bottom shows the unit under the cursor: name, HP and MP bars, weapon and armour.
  - Enemy turn: each wolf walks toward the nearest hero and bites one next to it.
- **Measured on the FH1** (320 x 240): the static layer (448 x 296) is drawn in 17 ms; a frame takes 7–8 ms
  with the whole screen redrawn (layer copy, cursor, four units and the scenery in front of them), at 30 frames
  a second; no display faults. Full screen costs ~14 KB of internal RAM (the screen task's stack, the image
  table). The pack is 260 KB (115 tiles, 4 characters, 250 images); the package's ELF is 9.6 KB.
- **Táticas da Ilha** (the same package, 0.3): the engine as a small game, every text in Portuguese
  (`main/game.h`; `data.c` the catalogue, classes, enemies, levels and the save; `screens.c` title, party,
  battle choice, summary, shop; `battle.c` the battle; `isotest.c` the loop).
  - Title: Começar, Sair. Progress is saved (`fh1_state_save("progresso")`): the roster, the bag, the coins.
  - Party: a card per hero (sprite, name, class, level and XP, PV, PM; "equipe" when chosen), up to three in
    the team. A on a card opens its menu, cascading: Pôr na / Tirar da equipe, Nome (a letter keyboard with
    accents), Arma and Armadura (what the bag holds), Atributos. Pronto leads to the battle choice (one battle
    for now: Clareira dos Lobos, its enemies and the most it pays).
  - Classes: Paladino (Cura), Ninja (moves 5, climbs 3), Arqueiro (bow, range 4), Mago (Fogo, Trovão, Cura).
  - Enemies: each one, on its turn, bites the weakest hero it can reach this turn (walking to a cell next to
    them), otherwise walks toward the nearest. Each enemy is worth coins ("moedas") and XP.
  - XP goes to the hero who knocks an enemy out (its full value) and 4 per action. Levels raise PV, PM and
    force. After a battle the summary lists the enemies knocked out with their coins and XP, the total (half on
    a defeat), and each hero's XP, level and "Subiu!" when they levelled up.
  - Shop: Comprar / Vender (Left/Right), weapons, armour and items with prices (selling pays half), what each
    does, how many the bag holds; placeholder icons (a coloured square and a letter) until the icon set. Then
    back to the party.
- **0.4:** no friendly fire (attacks and harmful spells only on the other side; healing and items only on
  one's own). Maps are made from a seed (`mapgen.c`: hills on an island, tiles and objects by theme: meadow,
  forest, rocks; heroes in the near corner, enemies in the far one, a walkable way checked, another try when
  not) or drawn by hand (a row of height digits and a row of objects per map row). A chain of three battles
  (Orla da Ilha, Bosque Sombrio, then the Alpha's den, drawn by hand), shown as a path each time the party sets
  out: won battles can be fought again, the next one is open, the rest are locked. Screen changes use a
  transition: a grid of growing diamonds sweeping across, bursting from the middle into a battle. The summary
  is an overlay on the battlefield: the box opens, each enemy knocked out pops in with its coins and XP, the
  coins count up, each hero's XP bar fills level by level, with "Subiu de nível!" and the stats gained. The
  shop uses icons: the bag icons (`assets/packs/icons-adventures-bagback`), big swords for the detail
  (`assets/packs/swords-32`), a coin pile for "moedas".
- **0.5:** heroes hold their weapon and wear their armour. The art pack carries, for each frame of the banado
  characters, the hand (the end of the base body's front arm) and a part map (torso, arms, legs, from the base
  body's hidden layers); weapons are the bag icons turned to 16 angles around their grip (`"rotated"` in the
  recipe). Armour recolours the dressed sprite's pixels over the parts it covers, keeping their shading (leather
  the torso, chainmail torso and arms, the robe torso and legs, plate all three). Actions play out: a melee swing
  (raise, swing through, a step in), a bow shot (aim, an arrow flying in an arc that turns along its path), a cast
  (the weapon held high, sparks gathering, then fire, lightning or healing sparkles on the target), an item thrown
  into the sky and down onto the target; the effect lands at the impact (`fx.c`: projectiles, particles,
  lightning, for any later use). Esperar ends the turn at once (no facing step). While choosing a target, its
  card shows what the action would do on its bars (the PV lost blinking red, or the PV or PM gained in green, the
  number over the bar). Reagir: ready, once until the hero's next turn, to strike back at whoever attacks it in
  close combat. Items reach 4 cells.
- **Checking on the PC:** `host/` builds the game for the PC with the FH1's font (`make -C host`), driven by a
  script of keys and staging commands, writing screenshots (`host/host host/actions.script host/out`).
- **0.5.1:** choosing a target: Left/Right (and Up/Down) cycle through the units the action can go to, nearest
  first, with a pointer over the chosen one and its card; no tile cursor (nothing aims at a tile yet); "Sem alvo
  ao alcance" when there is none. The battle menu has "Sair do jogo" (leaves without saving the battle).
- **0.5.2:** fixed a crash: the Reagir sign wrote a pixel past the frame buffer near the screen's bottom edge,
  corrupting the memory next to it (the game then crashed somewhere else, on the title screen). Archers with
  Reagir shoot back at any attacker their bow reaches. The PC host guards the frame buffer (a write outside it
  stops the run) and has development calls to reach any screen without playing: `win`, `lose`, `xp`, `ranged`,
  `cam` (`host/flow.script` goes from the title through a won battle, its summary with a level up, the shop and
  the path with the next battle open; `host/edge.script` sweeps a unit across the screen's edges).
- **0.5.5:** the crashes since 0.5 were a build problem: elf_loader's `project_elf` compiles with make's `/fast`
  targets, which skip the dependency check, so a changed header (`fart.h`, its image entry grown from 16 to 24
  bytes) did not rebuild `screens.c`; part of the game read the table with the old size. `tools/pkgbuild.sh`
  deletes the objects before every package build. SDK 0.6 (firmware): `fh1_dev_command` takes lines sent with
  `fh1dev.py op dev "TEXT"`; the game runs them through the same `dev_command` as the PC host (place, act,
  counter, target, gear, ranged, xp, hp, win, lose, log; `nosave` keeps the saved game as it was).
  `host/device.py` runs a script on the FH1 (keys, dev commands, screenshots); `host/dev-start.script`,
  `dev-scenes.script` and `dev-knockout.script` went through every scene on the FH1.
- **0.7:** the realms (`docs/taticas-da-ilha.md`: four elemental realms of 3, 6 and 9 battles, bosses, levels to
  15, gear levels, elemental weapons). Engine: `fart_draw_style` draws an image with a tint, armour, a colour for
  all of it (32 shades by brightness: a realm's creatures, an elemental weapon, an Elemental's glow), twice the
  size (bosses) and turned about its anchor (falling over); units carry a shade, a weapon shade, a zoom and a fall
  (`iso_fall`, `iso_rise`); `map_grade` recolours the built scenery per realm (lava, sea, forest, pale air) and
  its sky. `fx.c`: decoration particles the battle does not wait for (`fx_aura`: rising, dripping, dust,
  swirling) and projectile trails (`fx_trail`; with no image, a ball of light). `tools/fh1art.py`: a PNG per
  facing (`"files"`), some frames of a tag (`"pick"`), hues turned (`"recolor"`: the fire slimes from the purple
  ones, the crown kept), whole-number upscaling. New art: Scrabling's boar, badger and stag, and the slime pack
  (`assets/packs/free-slimes`: slimes and king slimes in four colours).
- **0.9:** four views and animated scenery.
  - **Views:** `iso_turn` turns the map a quarter round the cursor (Select or R1 one way, L1 the other; "girar"
    in the corner while choosing), the cursor's cell kept where it was on screen; the layer is rebuilt once, half
    way. It spins (480 ms): screen offsets from the cursor on the ground turn by `[[cos a, -2 sin a],
    [sin a / 2, cos a]]` (the isometric projection of the plane turned by a), so each screen pixel reads the
    cached layer where it comes from (two additions a pixel); units stand upright, carried with the ground under
    them; marks, the cursor and the scenery over units wait for the end. Heights lean a little half way, which
    reads as perspective. The other style (`ISO_TURN_NARROW`) narrows the picture to a line and widens it. The engine maps cells to
    the view's cells for drawing, depth order and the scenery over units; cells, facings and paths stay in map
    coordinates, and `iso_map_dir` turns a D-pad press into a map facing, so Up stays up. Units face
    `(facing + view) % 4` on screen; objects mirror on the odd views; blocks keep their light from one side.
  - **Scenery animations:** `iso_tile_anim` gives a tile frames, timed together, scattered or as a wave rolling
    across the map. The layer keeps the frame each animated cell shows; a frame change draws that cell's box
    again (the sky, then every column reaching into it, back to front), only for cells on screen. The water
    ripples: `fh1art.py`'s `"tile_anims"` moves a wave through the tile's brightness levels (every frame a step
    of the wave).
  - **Grading the images:** `iso_grade` recolours the scenery's images in place (`fart_recolor`, from a kept
    copy of their pixels) instead of the built layer, so redraws, the new views and the scenery drawn over units
    all have the realm's colours (the scenery over units used to show the ungraded art); lava ripples too.
    `iso_free` puts the shared art back.
  - **Memory:** the copies share 16 KB blocks and the engine's small tables ask for a little more than 4 KB, so
    all of it is in PSRAM (ESP-IDF serves blocks of up to 4 KB from internal RAM); a battle leaves ~27 KB of
    internal RAM free on the Guition.
  - **Changing the scenery:** `iso_set_obj`, `iso_set_height`, `iso_set_top` with a transition (`ISO_FX_BREAK`,
    `_DROP`, `_RISE`, `_NONE`). The cell takes its new state at once; while the transition plays, the layer shows
    what stays put (a column's lower blocks, a cell without the object coming) and the moving part is drawn in
    depth order with the units: a column rising or sinking whole, clipped by a V at its base (a block's lower
    edges) so it comes out of and goes into the ground; an object rising out of its cell's top; things falling from
    64 pixels up, landing with a bounce and dust the colour of the ground. What breaks is cut into pieces of 8 to
    16 pixels by drawing the image clipped to each piece, flying out from its middle and falling (each piece's
    place is a function of time: no state), dimmed at the end. When it ends, the layer is drawn again round the
    column. `on_change` plays the sounds (a crack, a rumble, a thud on landing).
  - **More transitions:** `ISO_FX_FADE` (an ordered 4 × 4 dither, `fart_draw_dither`) and `ISO_FX_TELEPORT` (the
    image unpacked, `fart_unpack`, and drawn as blocks of colour from 8 pixels down to 1, dithered in, with sparks
    rising; going, the reverse). What fades or teleports is a list of parts (a column's blocks and its object) the
    change draws in depth order while the layer shows what stays.
  - **Floating and bobbing:** a cell's column starts at `base` (nothing under it) and may bob with a group
    (`bob`, ±3 pixels over 1.8 s, each group in its own phase). Bobbing columns are left out of the layer and drawn
    each frame in depth order (carried round during a spin like units); units standing on them bob with them.
  - **Looks only:** `iso_crack` (the top face's pieces fly while the tile stays) and `iso_drop_on` (an image falls
    onto a cell, after units on it, and shatters), each with a delay, telling `on_change` when they hit.
  - **A layer check:** `iso_check_layer` builds the layer afresh into a spare buffer and counts the pixels that
    differ under the camera; every test runs it after its changes.
  - **The whole screen** (`fx.c`): `fx_screen` starts a negative flash (fully inverted, then easing back), a wave
    (each row slid by a sine of its height) or a shake (an offset the game adds to the camera while drawing), at a
    strength 1–3; `fx_screen_apply` draws the first two over the frame, before the game's panels.
  - **The water's loop:** 6 frames of 170 ms; the still tile is not one of them, so the loop never rests on a
    flat frame.
  - **Measured on the Guition:** a frame 6–8 ms; spinning 10–12 ms; the layer rebuilt in 11 ms; all 52 water cells
    redrawn at once in 13–14 ms; four scenery changes running 7 ms (the `bench` command). On the 14 × 14 test field:
    a frame 7 ms, spinning 12–14 ms, four changes 9 ms, the layer rebuilt in 18–19 ms, 68 water cells in 20–21 ms.
  - **Tests:** the test field (`test REALM`) and `host/suite.sh`, groups of end-to-end scripts on the PC or the FH1
    (`docs/sdk.md`, "Testing"; `docs/taticas-da-ilha.md`, "Development commands").

