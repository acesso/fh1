# CLAUDE.md: FH1 session handoff

For a Claude instance picking up this repository. Read this first, then the documents it points to. They
are the source of truth; this file is the map, the working rules and the hard-won lessons.

## The project

FH1 is a pocket handheld being built by hand: an ESP32-S3 (N16R8) on two 12 x 8 cm perfboards, a 320 x 240
ST7789 screen, SNES-style lit buttons, ES8311 audio, micro-SD, a 2.4 GHz LoRa radio (E28/SX1281), one 18650.
It runs "apps": emulators, a music player, packaged games. Parts for two units are bought; nothing is
soldered yet. The firmware runs on a **borrowed Guition ESP32-4848S040** dev board (4" 480 x 480
ST7701S RGB panel, GT911 touch, SD over SPI, CH340 USB serial).

| Read | For |
|---|---|
| `README.md` | Goals, status, repo layout |
| `hardware/*.md` | Requirements, architecture, parts, layout, wiring; `hardware/open-questions.md` holds decisions and open questions |
| `firmware/PLAN.md` | Firmware design and measured results (emulators, memory, display path, events, audio, music, bridge) |
| `firmware/README.md` | Building, flashing, using the firmware: controls, apps, console, screenshots, layout |
| `firmware/components/*/VENDORED.md` | What was vendored, from where, licence, and every local change ("FH1 change") |

## Working rules from the user (keep them)

- **Commit, never push.** `git push` is the user's job. Branch only if asked. Never add `Co-Authored-By`
  or any Claude attribution to commits.
- **Stay in `/home/acesso/ham`** for this project's sessions. The sibling `../Signal-Decoder` is a reference
  (the bridge board's style and the PCM5102A module notes), not a place to work.
- **No Chromium/Playwright** or other browser automation without asking first.
- **Docs reference repo files, never claude.ai links.**
- **Secrets stay out of the repo:** the Wi-Fi credentials live only in the device's NVS (set with `!wifi`);
  the dev console code is printed in the boot log and shown in Settings: do not write it into files; the
  friend's original firmware backup (contains their Wi-Fi passwords) is outside the repo, keep it there.
- **The user decides** open questions and design choices; recommend, don't settle. They are hands-on: they
  test on the device, send photos and terse feedback (often in Portuguese/English mix). Act on feedback
  precisely; ask nothing that can be checked.
- **Style of docs and UI text:** plain, short, concrete sentences; measured numbers over adjectives. The UI is
  in Portuguese by default (`T()` strings in English, table in `fh1_core/i18n.c`). Footer action words are
  at most 9 characters (`FH1_HINT_CHARS`).
- **Licences:** prefer permissive. Two GPL v2 emulators are in (nofrendo for NES, SMS Plus for Master System)
  because the permissive ones are too slow on 240 MHz. Flag any new licence before adding code.

## Environment

```sh
. ~/esp/esp-idf-v6.1/export.sh      # ESP-IDF v6.1; LVGL 9.6.0 (managed component)
cd ~/ham/FH1/firmware
idf.py build                        # -O2 for the whole firmware; a full rebuild takes ~10 min
```

- **Device:** a CH340 (USB id 1a86); its USB port (1-1, 1-3) and `/dev/ttyUSB*` number move when other boards
  (the bridges) are plugged or off: map ports by chip first (the bridges are the other CH340 and the A1S's CP2102). Opening the
  serial port resets the board. Flash at 460800 baud at most (reads above 230400 were unreliable).
- **Network:** DHCP, and the address changes. Use names: `fh1.local`
  for the FH1, `fh1-bridge-<4 hex>.local` for bridges (the A1S: `fh1-bridge-cb5a.local`). An old FH1 build
  elsewhere on the network once also answered `fh1.local`: after an update, check the dev console's stats
  (`fw`, `up`) to be sure it reached this board.
- **Update over Wi-Fi:** `curl -X POST -H "X-FH1-Code: <code>" --data-binary @build/fh1.bin http://<ip>/ota`.
  The device shows an overlay, then restarts gracefully into the other slot (ota_0/ota_1), rollback after 10 s.
- **Update over USB** when Wi-Fi is poor: `idf.py -p /dev/ttyUSB0 -b 460800 flash`. It writes the bootloader, partition table, the OTA
  selector (pointing at ota_0) and the app at ota_0; NVS (settings, Wi-Fi) and the `apps` LittleFS are kept.
  **Do not use `app-flash` alone**: it writes ota_0 while the selector may still boot ota_1.
- **Crash analysis:** `idf.py -p /dev/ttyUSB0 coredump-info` (core dumps go to flash).
- **Dev console:** `http://<ip>/` (WebSocket `/ws`). Drive it from the PC with
  `FH1_HOST=<ip> FH1_CODE=<code> firmware/tools/fh1dev.py` (keys, screenshots, stats, console ops; see its
  header). Screenshots (`/shot.bmp?code=`) are of the logical screen (320 x 320 in native mode, 320 x 240 in fh1 mode).
- **Serial commands** (via a terminal or `tools/keys.py`, type `!`): `!wifi <ssid> <pass>`, `!bl <pct>`,
  `!mode native|fh1`, `!theme gen1|gen2`.

## Firmware map (`firmware/components`)

| Component | What |
|---|---|
| `fh1_core` | Device registry and capabilities, settings (NVS), input queue, audio class, i18n, events (`fh1_events.h`, incl. `FH1_EV_TRANSFER`, `FH1_EV_SHUTDOWN`), fast-memory helper (`fh1_mem.h`), app signals and graceful restart (`fh1_app.h`), `fh1_text.h` |
| `fh1_board` | Guition 4848S040: ST7701S init, RGB panel fed from a logical-size PSRAM screen through bounce buffers (2x / 1.5x), backlight, I2C, GT911, SD, display health stats; `pcm5102a.c` audio driver (I2S GPIO1/2/40, registered off) |
| `fh1_storage` | LittleFS `/int`, SD card as a hot-plug device |
| `fh1_ui` | LVGL port and the window system (`ui.c`, large): themes, sprites/icons, title pill, windows with open/close animation, footers, paged lists, views (sideways pages), meters, rich text, transfer overlay, screen lending, shutdown routine; `post.c` boot screen; `lv_mem_psram.c` LVGL heap in PSRAM; `fh1_font8.c` generated font |
| `fh1_apps` | Launcher; System menu: Installer (with firmware from SD), Storage, Devices by kind (one window per kind, a view per device, each device's settings), About (read only); Wi-Fi window; console hooks (`apps.c`); game libraries for all consoles (`apps_games.c`); the Music app (`apps_music.c`, `music_deck.c`); packages and the ELF app runner |
| `fh1_gb` / `fh1_nes` / `fh1_sms` | Emulators with the same front-end shape: open / play (benchmark or play) / options / states / close; emulation on core 1, blitter on core 0 |
| `fh1_devconsole` | Wi-Fi, mDNS, SNTP, HTTP + WebSocket console, uploads, OTA, screenshots |
| `fh1_player` | Audio player engine: MP3/FLAC/Ogg/WAV/MOD decoders (vendored, permissive; `VENDORED.md`), a decoding task with a PSRAM stack and a file-I/O helper task, a tap for visualisers. The Music app (`fh1_apps/apps_music.c` library, `music_deck.c` full-screen deck) uses it; PLAN.md "Music" |
| `fh1_avatar` | Avatar renderer, style A (C port of `firmware/docs/avatar-showcase.html`, kept pixel-identical: compare with the page's JS before changing it), the avatar code, editor lists |
| `fh1_users` | Users (`firmware/docs/users.md`; contacts, trust statements and shared-content headers in `users_social.c`; between units in `fh1_apps/apps_net.c`, test peer `firmware/tools/fh1peer.py`): Ed25519 keys (Monocypher, vendored), signed CBOR profiles, owner / local / visitors, PINs, controller links, players' users, per-user data root for `fh1_data_dir`, SD backups. Identities in NVS `users`. The UI is the launcher's Users menu (`fh1_apps/apps_users.c`). Dev console ops `users`, `user_*`, `users_reset` (README "Users") |
| `fh1_ota`, `fh1_sdk` | OTA helpers; the app API for packaged `.fpk` apps (`apps/packages/hello` is the template) |
| `tools/` | `fpk.py` packages, `hexfont.py` font generator, `gbc_palettes.py` GBC palette tables, `keys.py` serial keys, `fh1dev.py` console driver |

## Hard-won lessons (read before changing these areas)

- **Internal RAM is the scarce resource** (~78 KB free at the desktop). Wi-Fi aborts when it runs out. Keep UI
  and tooling data in PSRAM (LVGL heap, sprites, JSON, logs). Use `fh1_mem_fast()` for an app's hot data: it
  keeps a 32 KB reserve. Internal RAM is fragmented (largest block ~31 KB): split big hot blocks. Avoid
  static buffers in `.bss`; allocate per session. Keep `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` off (it forces
  static TX buffers, -26 KB internal for good). After changing Kconfig options, check the
  generated `sdkconfig` against one regenerated from `sdkconfig.defaults` (dependent options do not revert).
- **Full-screen apps:** `fh1_ui_pause(true)` frees the UI's internal draw buffer; only the UI task touches
  LVGL. A play loop must check `fh1_app_quit_requested()` and (when playing) `fh1_app_should_yield()` once per
  frame, and block now and then (`vTaskDelay`): a loop that never blocks starves core 1's idle task, the
  watchdog then prints from an interrupt and the panel glitches.
- **Display:** the RGB panel is fed by CPU-copied bounce buffers from the logical screen; continuous flash
  writes still cause brief glitches (shared bus). Watch the console's `disp` counters (`late`, `odd`) during
  any heavy test; the user asked for glitch monitoring on every benchmark.
  The driver also logs each glitch as a `disp:` warning (late refill or short frame, with the task running
  on each core). Don't scan PSRAM memory while measuring: scanning tasks' PSRAM stacks causes glitches (the console reads
  the task list every 10 s only).
- **LVGL:** objects inside a hidden parent cannot take focus: animate visibility with opacity, not the hidden
  flag . Lending the screen uses `lv_inv_area(disp, NULL)` (private
  header `src/core/lv_refr_private.h`).
- **Build:** `-O2` turns an mDNS `strncpy` warning into an error; the top-level `CMakeLists.txt` keeps
  `stringop-truncation` a warning. Vendored emulators get their allocations redirected by a forced include
  (`nes_alloc.h`, `sms_alloc.h`).
- **Downloads are untrusted data:** parse them, never execute them (the GBC palette generator parses the
  SameBoy source with a strict number parser and runs with `python3 -I`).
