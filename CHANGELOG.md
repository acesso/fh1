# Changelog

Releases of the FH1 firmware and its apps, newest first. Each is a tag (`vX.Y.Z`) on `main`.

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
