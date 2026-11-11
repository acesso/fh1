# Changelog

Releases of the FH1 firmware and its apps, newest first. Each is a tag (`vX.Y.Z`) on `main`.

## 0.3.3 (2026-11-11)

Táticas da Ilha does less than half the work for the same picture, and the simulator shows the FH1 larger.

### Táticas da Ilha 0.8.2

- **The menus' background** (night blue with a diamond grid) is painted once and copied, instead of computed for
  every pixel of every frame: it was 82% of the game's own work on the title screen. **Menu boxes** draw by rows,
  without tests per pixel. The title screen runs 58% fewer instructions, pixel for pixel the same.

### Firmware

- **Apps' code address in the log** (`text at ..., linked at ...`): with the app's unstripped ELF, a crash or a
  profile inside an app reads as its functions and lines (`firmware/README.md`, "Reading a crash or a profile").

### Simulator

- **The FH1 at 1.5 to 2 times its real size**, as large as the window allows, with every screen pixel the same size
  (a whole-number nearest-neighbour blow-up, then a smooth scale to the screen's own pixels).

## 0.3.2 (2026-11-11)

The simulator loads on GitHub Pages again, and games and boot cost less.

### Simulator

- **Fixed:** the simulator stopped at "source array is too long" on GitHub Pages: the host compresses the
  downloads, and the page sized its buffer from the compressed length.
- **Faster boot:** ESP-IDF's busy-waits at boot (clock and radio calibration, the second core's start) pass their
  time without running their loops (esp32sim's new fast delays).

### SDK

- **Frames send only what changed:** an app's frame copies and reports only the rows that differ from the last one,
  compared a word at a time, so a still game screen costs nothing on the FH1's display.
- **Text:** `fh1_px_text` draws glyphs that lie wholly on screen without testing each pixel.

On the simulator's Táticas da Ilha title screen the firmware runs 24% fewer instructions (891 M against 1,169 M over
boot and 13 s of the game) and sends a tenth of the pixels.

## 0.3.1 (2026-11-11)

Less work for the same picture: on the FH1 board, and three times faster in the simulator's idle menus.

### FH1 board

- **The screen gets only what changed:** while the UI owns it, each 20-line band carries just the rectangle
  drawn since the last frame (a blinking cursor costs its own pixels, not 320 × 240 thirty times a second);
  full frames while a full-screen program draws directly (`fh1_board_direct`, switched by `fh1_ui_pause`).
  `fh1_board_changed` reports rows drawn directly, as the POST screen does.
- **Status bar:** each icon, the battery and the clock's colon are restyled only when they change.
- **Boot:** no 8 MB PSRAM memory test.

On the simulator's idle launcher the firmware runs 62% fewer instructions (167 M against 437 M over boot and
9 s idle) and sends ten thousand pixels where it sent about 27 million.

## 0.3.0 (2026-11-11)

The simulator as a showcase: the FH1 drawn to scale, played through its own buttons.

### Simulator and site

- **[The simulator](https://acesso.github.io/fh1/play.html)** draws the FH1 to scale (80 × 120 mm, the layout's
  positions) with the real screen inside. Its buttons reach the firmware as on the hardware: A, B, X, Y, Start and
  Select through the MCP23017, the thumbstick through the ADC, Menu through GPIO0. The button lights follow the
  firmware's LEDs, and the sound plays through Web Audio. 🛠 opens the details: speed, restart, your own firmware
  image, the UART0 console.
- **Landing page:** the live 3D view of the V1 build, with its own copy of three.js and the static render as a
  fallback without WebGL; the toy FH1 to scale; Connectivity (Wi-Fi, Bluetooth LE, 2.4 GHz LoRa).
- **esp32sim** (`simulator/esp32sim.patch`): board keys and indicator lights in the board interface, the page
  messages `press`, `adc` and `lights`, and `press NAME` for board keys in scripts.

### Firmware

- **UI:** LVGL 9.6's per-flag setters and a 10 s limit on scrolling text; the console no longer fills with LVGL
  warnings, which cost time on the device and much more in the simulator.

### Builds and releases

- **CI runs on release tags only:** a tag builds the firmware for both boards, the packages and the site, creates
  the release and publishes the site.

## 0.2.0 (2026-11-10)

The firmware for the FH1 board itself, and the FH1 in the browser.

### FH1 board

- **Chosen at build time** (`sdkconfig.defaults.fh1`, `tools/build-board.sh fh1`): the ST7789 screen at 320 × 240
  over SPI2; A, B, X, Y, Start and Select with their lights on MCP23017 #1; the amplifier on #2; the thumbstick on
  ADC1; Menu on GPIO0; the PCF8563 clock.
- **Console:** an empty command is ignored, so scripts can end a line of keys with `!`.

### Web simulator and site

- **[acesso.github.io/fh1](https://acesso.github.io/fh1/):** the FH1's page in Portuguese and English, in dark,
  light and auto themes, with feedback through GitHub issues (Bug, Sugestão, Outro).
- **The real firmware in the browser:** the FH1-board build with Táticas da Ilha and Duelo installed, on
  [esp32sim](https://github.com/mikroverk/esp32sim) compiled to WebAssembly with an `fh1` board model (`simulator/`).

### Builds and releases

- **CI** (`.github/workflows/pages.yml`): every push builds the firmware for both boards, the packages and the
  site. `main` publishes the site. Each release carries `fh1-firmware-fh1.bin`, `fh1-firmware-guition.bin` and
  the packages.

## 0.1.0 (2026-11-10)

The first tagged release: the firmware on the Guition dev board, with packaged games on an isometric engine.

### Platform (SDK 0.8)

- **Full screen for apps** (SDK 0.5): `fh1_screen_open`, a back buffer, `fh1_app_dir`, `fh1_px_text`.
- **Development commands** from the console to the running app (SDK 0.6): `fh1_dev_command`.
- **Asset packages and sound effects** (SDK 0.7): packages shared by apps (`"uses"`, `fh1_asset_dir`, on the SD
  card); an 8-voice mixer at 44.1 kHz (`fh1_sfx_*`); `tools/fh1sfx.py` builds sound banks; the RPG sound pack
  as `org.fh1.assets.rpg-sfx`.
- **The controller in pictures** (SDK 0.8): footers' button chips and `fh1_px_key` follow player 1's controller
  (the FH1's colours, Xbox and Nintendo letters, PlayStation shapes) and change with it.
- **Per-user data follows player 1** (SDK 0.8): a controller taken over by another user mid-game moves saves and
  settings to that user's folder.
- **Controller takeover:** any connected controller holding Menu for 3 s takes player 1, then asks whose it is.
- **Packages:** `tools/pkgbuild.sh` builds cleanly (no stale objects) and `tools/elfcheck.py` fails a build that
  needs a symbol the firmware does not export; uploads up to 64 MB.
- **Dev console:** valid UTF-8 always; ops `heap`, `data_list`, `data_copy`; stats pushes carry the device and app
  lists only when they change.

### Táticas da Ilha 0.8.1

- **Engine:** isometric maps with heights, a static layer, units with gear (weapons in hand, armour by part),
  styled drawing (shades, twice the size, falling over, fading), effects (projectiles, trails, sparks, lightning,
  rock spikes, whirlwinds); art packs built by `tools/fh1art.py`.
- **Game:** four elemental realms of 3, 6 and 9 battles with bosses and Elementals; heroes to level 15; gear in
  four levels per class; elemental weapons and armour; casters from fase 2; spells scaled by the staff; per-user
  progress; the shop on the party screen. Rules: `firmware/docs/taticas-da-ilha.md`.

### Fixed

- **Internal RAM ran out** (0 bytes at its lowest): console clients that went away kept their unsent data until
  TCP gave up, and Wi-Fi could borrow up to 32 + 32 buffers in a burst. Such clients are now closed (on a failed
  push, and by TCP keep-alive), Wi-Fi's dynamic buffers are capped (8 TX, 16 RX), and uploads write from one DMA
  buffer. Idle internal RAM went from ~21 KB to ~23–35 KB free; uploads no longer fail allocations.
- **Sounds over 1.5 s looped** (the mixer's position wrapped at 65 536 frames).
- **Crashes of packaged games** built from stale objects; a stopped app now frees its memory.
