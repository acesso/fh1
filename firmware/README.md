# FH1 firmware

The base system for the FH1: device manager, button-driven window interface, web dev console, and apps installed from packages without reflashing. The design is in [PLAN.md](PLAN.md). This page covers building and using it.

It runs on the **Guition ESP32-4848S040** dev board; the FH1 hardware is not built yet.

## What it has

- **Interface:** retro windows driven by buttons, five themes (Poke Gen1, Poke Gen2, Hex, Garden, Command) switched live in the Temas app, with animated frames and system sounds, menus side by side (APPS, USERS, SYSTEM), Portuguese and English.
- **Devices:** a registry with probing, enable/disable saved, capabilities and events; Devices windows by kind.
- **Storage:** internal LittleFS (`/int`); the SD card as a hot-plug device with mount, eject, format.
- **Apps:** GameBoy Color, NES and Master System emulators at full speed, a music player, and packaged apps (`.fpk`) loaded into PSRAM.
- **Network:** Wi-Fi with an on-screen keyboard, network time, the web dev console at `http://fh1.local/`, firmware updates over Wi-Fi and from the SD card with rollback.
- **Controllers:** Bluetooth controllers through an ESP32 bridge (`../bridge`), up to two players.
- **Users:** identities with keys, avatars, PINs, backups, people nearby and friends.

## Build and flash

```sh
. ~/esp/esp-idf-v6.1/export.sh
cd firmware
idf.py build
idf.py -p /dev/ttyUSB0 -b 460800 flash      # the dev board's CH340 is unreliable above 460800
```

`flash` keeps the settings (NVS) and the installed apps (the `apps` partition). Do not use `app-flash` alone: it writes ota_0 while the device may boot ota_1.

The board is chosen at build time (`components/fh1_board`, Kconfig "FH1 board"). `idf.py build` builds for the
Guition dev board. For the FH1 itself (ST7789 320x240 over SPI, buttons and their lights on MCP23017 #1, the stick
on the ADC, Menu on GPIO0; `components/fh1_board/fh1.c`), in its own build folder:

```sh
idf.py -B build-fh1 -D SDKCONFIG=build-fh1/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.fh1" build
```

The FH1 board does not yet set up the ES8311's registers or read the micro-SD card, the battery or the IMU.

`tools/build-board.sh guition|fh1 OUT_DIR` does either build and writes one image to flash at 0x0,
`fh1-firmware-<board>.bin`, plus its parts. A third argument, `psram120`, builds the Guition with PSRAM and flash at
120 MHz (`sdkconfig.defaults.psram120`): about 20-30% faster on memory-bound work, experimental (its timing is set
once at power-on, so a large temperature change can make it fail), for the dev unit only; releases stay at 80 MHz. Each release builds both in CI
(`.github/workflows/pages.yml`) and carries both images on GitHub. To write a release image:

```sh
python -m esptool --chip esp32s3 -p /dev/ttyUSB0 -b 460800 write-flash 0x0 fh1-firmware-guition.bin
```

That image holds the bootloader, the partition table, the OTA data and the app, and it **erases the settings**
(NVS at 0x9000: the Wi-Fi network, users, theme, the console code); installed apps stay. Use it for a first install.
To update a board that is set up, keep its settings:

- **Over Wi-Fi** (the web page's Firmware section): writes the other app slot and leaves NVS alone.
- **Over USB, from a build** (`fh1.bin` and `ota_data_initial.bin` in OUT_DIR): write the app and reset the OTA data,
  so the board starts from the slot just written:
  ```sh
  python -m esptool --chip esp32s3 -p /dev/ttyUSB0 -b 460800 write-flash 0xf000 ota_data_initial.bin 0x20000 fh1.bin
  ```
- **Over USB, with the release image:** back up NVS first and write it back after. The backup holds the Wi-Fi
  password: keep it out of the repo.
  ```sh
  python -m esptool --chip esp32s3 -p /dev/ttyUSB0 read-flash 0x9000 0x6000 nvs-backup.bin
  python -m esptool --chip esp32s3 -p /dev/ttyUSB0 -b 460800 write-flash 0x0 fh1-firmware-guition.bin
  python -m esptool --chip esp32s3 -p /dev/ttyUSB0 write-flash 0x9000 nvs-backup.bin
  ```

## First start

1. Set the Wi-Fi network over the serial console. Either type in a serial terminal at 115200 baud, or run `tools/keys.py` and type `!`:
   ```
   !wifi <ssid> <password>
   ```
   Other serial commands: `!bl <percent>`, `!mode <native|fh1>` (screen mode, restarts: native is 320 × 320 filling the Guition's panel, fh1 the FH1's
   320 × 240, both at 1.5 times), `!theme <gen1|gen2|hex|garden|scifi>` (at once), `!sounds <on|off>` (system sounds).
   Without credentials the device opens an access point, `FH1-setup` (password `fh1-setup`), at 192.168.4.1.
2. Open `http://fh1.local/`. It asks for the 6-digit code, which is printed in the boot log and shown in the System window.

## Controls on the dev board

The board has no buttons. Use either of these:

- **The console's Controller panel:**
  - **Keyboard:** arrows or WASD; J = A, K = B, U = X, I = Y; Enter = Start; Space = Select; M = Menu.
  - **Gamepad:** a pad connected to the PC works through the browser's standard mapping, with SNES positions. On a DualSense: Cross = B, Circle = A, Square = Y, Triangle = X, Create = Select, Options = Start, PS = Menu.
- **`tools/keys.py`:** the same keys over the serial console.

| Button | Does |
|---|---|
| D-pad | Move focus in the focused window |
| A | Activate |
| B | Close the window |
| Select | Cycle through the windows |
| Menu | Open the launcher |
| Left / Right (the D-pad chip) | In the launcher: switch menus (Apps, Users, System); on a slider: move it; in a window with views: the next or previous view; in a game list: pages. On a DualSense: the D-pad (or the left stick) |
| X | In a game list: speed test; in About: restart |

The screen has two bars. The top one shows the top-level menus as tabs; the bottom one shows live status: Wi-Fi signal and SD card icons, a battery icon on devices with a battery, and the clock (network time, UTC-3 by default; the dev board has no RTC). Clock and battery sit on the left, SD and Wi-Fi on the right. The launcher has one menu per page, each a tab with its icon: **Apps** first (each app with its icon), **Users**, then
**System**: Installer (packages, and firmware from the SD card), Storage, Devices, Inputs and outputs, Bridges and
About.

**Devices** lists the device kinds (Video, Sound, Network, Clock, Controls) with how many are on, e.g. "Som (1/1)". A kind
opens one window with a view per device, side by side (Left/Right; the line on top names the view, e.g.
"Tela 1/3"). Each view shows its device's state, driver, details and power draw, switches it on or off,
probes it again, and holds its own settings:

| Kind | Views and settings |
|---|---|
| Video | Screen (screen mode, theme, FPS overlay, language), Backlight (brightness slider), Touch |
| Sound | Sound out (volume slider; channels: stereo, mono, left only, right only; a test sound: a tone on the left, then on the right) |
| Network | Wi-Fi (network, IP, signal; the networks window; forget the network) |
| Clock | System time (time, zone, source), RTC chip |
| Controls | Buttons (the dev console's controller and the serial keys) |

**About** is read only: board, CPU, memory, RAM and PSRAM meters, version, ESP-IDF and LVGL, uptime, Wi-Fi
and the console address and code. X restarts.

## GameBoy Color

APPS > GameBoy Color lists the games in `/int/roms` and `/sd/roms` by the title in their cartridge header, with a small icon for where each is stored; A plays, X runs a 20-second speed test.
**MENU in a game** pauses it and opens its options over the picture: resume, picture size (fit / 1x),
palette for original Game Boy games (green, grey, sepia, ocean), drawing (auto / every frame / half),
speed (normal / turbo), FPS (emulated / shown, above the picture), save and load state, reset, quit. B or MENU
resumes. Options are saved; battery saves and save states go to `/int/data/gb/`. Upload `.gb`/`.gbc` (and `.nes`) files from
the console. The test ROM was Tobu Tobu Girl Deluxe (Tangram Games; MIT code, CC BY 4.0 assets), from Homebrew Hub.

## Games: GameBoy Color, NES, Master System

Three apps share one game library (`components/fh1_apps/apps_games.c`): each lists its games from
`/int/roms` and `/sd/roms` in pages (Left/Right turns pages; only the visible rows exist, so thousands of
games are fine), A plays, X runs a 20-second speed test, MENU in a game opens its options over the frozen
picture (drawing, speed, FPS, sprite limit, palette where the console has one; save and load state, reset,
quit). Battery saves and states go to `/int/data/<console>/`.

| App | Files | Emulator | Licence |
|---|---|---|---|
| GameBoy Color | `.gb`, `.gbc` | Peanut-GB | MIT |
| NES | `.nes` | nofrendo (from retro-go) | GPL v2 |
| Master System | `.sms`, `.gg` (Game Gear), `.sg` (SG-1000) | SMS Plus (from retro-go) | GPL v2 |

Original Game Boy games are coloured as a Game Boy Color colours them: "GBC (auto)" looks the game up by
its title and colours the background and each sprite group apart (Pokémon Red in red, Blue in blue); the
GBC's 12 power-on palettes and the one-colour classics (green, grey, sepia, ocean) are the other choices.
The tables come from SameBoy's open boot ROM (`tools/gbc_palettes.py`).
Titles come from the cartridge header (Game Boy) or the file name without tags such as "(Europe)". PAL games
run at 50 frames/s. Delete a game from the console: `{"t":"app","op":"delete_rom","path":"/sd/roms/<file>"}`.
The Master System test game was Rassak's Retribution (Haroldo O. Pinheiro, MIT).

**Events during a game:** a file or firmware arriving pauses the game; its overlay shows over the frozen
picture, and the game carries on when it closes. A restart closes the game first (saving).

## Sound

A PCM5102A on the board's `H1` header (wiring in PLAN.md, "Dev-board audio") is the `audio0` device. The chip cannot be
detected, so it is off until enabled in Devices once wired. The GameBoy Color, NES and Master System apps
and the Music app then play sound. Without it, the sound chips are not even emulated.

## Bluetooth controllers

A DualSense (or another Bluetooth Classic pad Bluepad32 knows) connects through an extension board, the
ESP32 in `bridge/` (see its README). In System > Bridges: switch Bluetooth on (it takes ~27 KB of internal
RAM while on); the first bridge is added by itself, more with "Search for bridges" (two at once). A bridge's
window has its Devices (pair a controller there), Storage and About.

Holding Menu (PS, Home) for 3 s on any connected controller that is not player 1's makes it player 1 at once
(for a controller whose battery ran out): it rumbles, a notification says so, and a window asks whose
controller it is (B leaves that as it was).

System > Inputs and outputs chooses who plays with what (automatic, the FH1's own keys, or a given
controller) and where the sound goes. Player 1's controller also works the whole interface: circle is A,
cross B, triangle X, square Y, Options Start, Create Select, PS Menu; the left stick moves like the D-pad.
Player 2 plays in NES and Master System games.

## Users

The **Users** menu holds the people on this FH1 (design and formats: `docs/users.md`). The first
user made is the **owner**: the saves made before users existed become theirs. Then more local users, and
**visitors** (never the owner; removed with their saves). Each user is `name#TAG`: a short name unique on this
FH1 (3 to 16 of a-z, 0-9, `_`, `-`) and the first 4 characters of their id. Behind it is an Ed25519 key made on
the FH1 and a signed profile (name, rev), so users can travel between units without clashing.

In the menu each user is their animated mini face and name, with badges over the face: a crown for the owner,
a tag for a visitor, and their controller connected now in its colour. A user's window shows their face, Edit
avatar (the editor also opens by itself for a new user: body, hair, colours, top, bottom, accessories, an
expression to preview; Save signs the profile again, with the PIN), then Play as player 1, Link a controller (a controller connected now) or Free one, Rename, a PIN
(4 to 8 digits; it locks the user's key, and acting as them asks for it), Back up to SD, Make owner and Remove
(both ask for the owner's PIN). A controller's own window (Inputs and outputs) also has **Belongs to**.

- **Players have users.** A controller linked to a user makes that user its player. Player 1 is the
  interface user: games and apps keep their saves in that user's folder, `/int/users/<id>/data/<app>/`
  (`/int/data/<app>/` while there are no users). Player 1 without a controller of their own is the user picked
  with Play as player 1 (the owner after a restart), unless that user plays as another player with their own
  controller.
- **Backups**: `/sd/users/<name>-<TAG>.fh1u`, locked with a passphrase (6+ characters): the key and the profile.
  Restore from SD (in the Users menu) brings the user to this or another FH1 as a local user, without the PIN.
- **Storage**: identities (keys, profiles, links) live in NVS (namespace `users`), so formatting the internal
  storage keeps them; their saves are in `/int/users/`.
- **Notifications** slide in at the top right with a face: a user's controller connecting, player 1
  changing hands, who plays when an app starts, someone nearby, friend requests and answers.
- **People nearby** (in the Users menu): users of other FH1s on the same Wi-Fi, found every minute by mDNS
  (`_fh1._tcp`) and checked by their signatures; Look now asks at once; a person can be forgotten.
- **Friends** (a user's window): requests to answer (A accepts), friends, requests sent; Add a friend asks
  someone nearby (or another user here); A on a friend takes it back. Requests travel to the other FH1
  (`POST /fh1/trust`), retried until delivered.
- To try it without a second FH1: `tools/fh1peer.py serve --name ana --port 8765 --accept` on the PC (needs
  python3-cryptography and avahi): a pretend unit with one user, announced by mDNS, that accepts friend
  requests; `tools/fh1peer.py users` lists the FH1's users with every signature checked on the PC.
- Measured: a PIN check takes ~130 ms (Argon2id, 256 KB), a backup or restore ~0.7 s (1 MB).
- Console ops (dev; `fh1dev.py op OP [PATH] [ID]`, a user by name or id): `users`, `user_new` (ID the name,
  PATH `visitor` for a visitor), `user_use`, `user_link` (ID the controller, PATH the user or empty),
  `user_pin` (PATH `old,new`), `user_rename` (PATH `new,pin`), `user_backup` (PATH `pin,passphrase`),
  `user_restore` (ID the file, PATH the passphrase), `user_delbackup`, `user_remove`, `users_data` (player 1's
  data folder), `user_avbench` (avatar drawing times), `user_look` (look for other units now), `user_rel`
  (a user's relations and the outbox), `user_friend` (ID the user, PATH `other id,level[,pin]`), `user_share`
  (ID the author, PATH `who,viewer[,pin]`: signs and checks a test header), `user_forget` (a contact), `users_reset` (back to no users; the owner's saves
  become the unit's again).

## Music

The Music app (APPS menu) plays MP3, FLAC, Ogg Vorbis, WAV and tracker modules (`.mod`) from `/sd/music`
and `/int/music`, folders included; upload them on the dev console like games. In the list: A plays,
X opens the deck, Y stops, B closes (the music plays on until stopped or a game starts).

The deck is a full-screen retro hi-fi panel: time, track, levels, and a visualiser of what is heard.

| Key | Deck |
|---|---|
| A | Play / pause |
| Left / Right | Previous / next track; held: seek back / forward |
| Up / Down | Volume |
| X | Visualiser: spectrum, waterfall (spectrogram), scope, vector |
| Y | Colours: amber, phosphor, synthwave, LCD |
| Select | Repeat all, repeat one, shuffle |
| B or Menu | Back to the list |

## Memory while a game runs

A full-screen app gets the fast internal RAM: the window system keeps its heap in PSRAM and gives back its
draw buffer while paused, and the emulators put their hottest data (CPU state, work and video RAM, the
first ROM bank) in internal RAM first, always leaving 32 KB free for Wi-Fi. See PLAN.md, "Memory".

## Look

Windows are text boxes with a double border, ball corners, and the title in a small pill the border runs
into. They open by growing from the top-left corner and close by shrinking back into it.
Each window lists its actions in a footer: a grid of two columns, a chip per button and one word of at
most 9 characters. A, B, X and Y are known by colour
(red, yellow, blue, green, as on the FH1's caps); Start, Select and Menu are pills with an icon.
Values keep to one line (a name, then its value) and slide sideways, slowly, when they do not fit; so does
the focused item's text. A file or firmware arriving shows a progress overlay in the middle of the screen, with the icon of what
it is: a game shows its app's console with a cartridge sliding in, a package a parcel, firmware a chip.
Apps register the file types they open (`fh1_ui_register_file_type`).

Five themes, chosen in the Temas app (in System, before About) or in System > Devices > the screen
("Theme"), applied at once, with no restart. How they are drawn, and how to add one: [`docs/themes.md`](docs/themes.md).
- **Poke Gen1** (green) and **Poke Gen2** (colour): the rounded boxes above, on a dotted or latticed desktop.
- **Hex**: an early-warning console. Amber on black, with a honeycomb desktop. Boxes are square, cut at 45 degrees
  at the corners. The title sits on a black plate leaning with a band of warning stripes. The window in focus is
  amber, the ones behind it dim amber. Messages and questions are red: stripes, border and corners, with a white
  title. The item in focus reads amber, the others warm white, and the cursor is a small hexagon (hollow in a
  window out of focus). The stripes of the window in focus run.
- **Garden**: cream paper framed in wooden planks with grain, knots and a vine with leaves hanging over the edges;
  a flower on each corner (pink, white, lilac, apricot; buds out of focus, red roses on a message), a sign for the
  title, a lawn with daisies for the desktop. A window fades in while the vine grows round it and the flowers
  open; in the window in focus the flowers sway, a butterfly wanders over the whole window in fluttering arcs,
  resting here and there a moment or a while (on a flower now and then), and a bee darts in zig-zags from flower to
  flower, staying long and wobbling after a few seconds there. The cursor is a ladybird; the item in focus reads
  green.
- **Command**: a console of cyan light on near-black glass in steel plates with vents and rivets, a corner
  bracket with a status light on each corner, the title on a lit tab with three status lights, a grid desktop.
  A window builds itself: the brackets fly out from its middle, the plates close from the corners, the glass
  lights up behind a scan line, then the contents appear (closing runs backwards). In the window in focus the
  lights blink and cycle, and a spark appears at a random point of the line of light, runs along it either way a
  while and goes out until the next. Messages are red; the cursor is a chevron.

A file or a firmware arriving shows in a **notice window** in the middle of the screen, over everything (a game's
picture too): the theme's own window, with its frame, opening, decorations and motes, the file's icon and name and
a progress bar.

**System sounds** ("System sounds" beside "Theme", off at first): a short sound for moving, choosing, going back, a
message or question, a notice and a window opening, in the theme's voice: soft pulse-wave blips with Gen, crisp square-wave ticks
and a two-tone warning with Hex, wooden marimba notes and birdsong with Garden (leaves rustle as a window opens),
clicks, sweeps and chimes with Command (a clack, a servo's whine and a clack as a window builds). The UI makes them in memory (`components/fh1_ui/ui_sound.c`) and plays them
through the apps' sound mixer, under the sound volume.

## Start and restart

The boot screen is a BIOS-style self test: one line per step with what it really found (chip, memory,
storage, the board, each device with its state), and a progress bar. A restart (About, a firmware update,
a screen mode change) closes the running game, then the windows one by one, then restarts.

## Language

The interface is in Portuguese by default, English as the alternative (Devices > Video > Screen > Language). Strings are
written in English in the code, wrapped in `T()`, and translated in `components/fh1_core/i18n.c`. The pixel font
is unscii-8 (public domain) with Latin-1 accents, generated by `tools/hexfont.py`. The dev console stays in
English.

## Firmware updates

- **Over the network:** the console's Firmware panel, or `curl -X POST -H "X-FH1-Code: <code>" --data-binary @build/fh1.bin http://fh1.local/ota`.
- **From the SD card:** put the `.bin` in `/sd/firmware/` (the console's upload does this for `.bin` files), then System > Installer > Firmware from SD.

The image goes to the spare slot and the device restarts into it. A new firmware that has not run for 10 s with the UI up is rolled back on the next reset.

## Display health and the stress test

The console stats carry `disp`: panel frames, line bands prepared, bands that took longer than the DMA's
sending time (`late`), frames that did not get all their bands (`odd`), the longest band, and the frame
period range. A console command loads the system for a while: `{"t":"app","op":"stress","id":"20 psram flash ui"}`
(seconds, then any of `psram`, `flash`, `ui`).

Measured on the dev board (frame period 26.5 ms): no late bands or odd frames idle, with full UI redraws,
PSRAM copies on both cores, heavy Wi-Fi transfers, or a game running. **Continuous flash writes** do cause
odd frames: the flash chip holds the bus PSRAM shares, so the panel's refill waits. Normal use writes flash
only now and then (settings, installs, game saves on exit, updates).

## Screenshots

`http://fh1.local/shot.bmp?code=<code>` returns one still frame of the panel (480 × 480 BMP). It is on demand only; the console never streams the screen.

## Writing an app

An app is a small ESP-IDF project that includes only [`fh1_sdk.h`](components/fh1_sdk/include/fh1_sdk.h), packed
into a package (`.fpk`) and installed on the device; [`apps/packages/hello`](apps/packages/hello) is the template.
Everything about writing apps and games (the build loop, the API by SDK version, the example apps, the isometric
engine, testing, debugging, rules, known issues, limits and how to change the SDK) is in the SDK guide,
[`docs/sdk.md`](docs/sdk.md).

## Layout

| Path | What |
|---|---|
| `main/` | Start-up, serial keys and console commands |
| `components/fh1_core/` | Device manager, capabilities, settings, input events, the audio class |
| `components/fh1_board/` | Board layer (now: Guition 4848S040) |
| `components/fh1_storage/` | Internal LittleFS and the SD card device |
| `components/fh1_ui/` | LVGL port, desktop, window manager, widgets, dialogs |
| `components/fh1_apps/` | Launcher, built-in apps, packages, the app runner and the SDK symbol table |
| `components/fh1_devconsole/` | Wi-Fi device, mDNS, the web console (`web/index.html`) |
| `components/fh1_gb/` | The Game Boy emulator: Peanut-GB (vendored, see `VENDORED.md`) and its front end |
| `components/fh1_nes/` | The NES emulator: nofrendo (vendored, GPL v2, see `VENDORED.md`) and its front end |
| `components/fh1_sms/` | The Master System emulator: SMS Plus (vendored, GPL v2, see `VENDORED.md`) and its front end |
| `components/fh1_avatar/` | Avatars, style A: the renderer (a C port of `docs/avatar-showcase.html`, pixel-identical), the 12-byte avatar code, the editor's lists |
| `components/fh1_users/` | Users: keys, signed profiles, owner and visitors, PINs, controllers and players, backups; Monocypher (vendored, CC0/BSD-2, see `VENDORED.md`) |
| `components/fh1_player/` | The audio player engine and its decoders (minimp3, dr_flac, dr_wav, Tremor, pocketmod; see `VENDORED.md`) |
| `components/fh1_sdk/` | `fh1_sdk.h`, the app API |
| `apps/packages/` | Packaged apps |
| `tools/` | `fh1peer.py` (a pretend FH1 with one user on the PC, to test people nearby and friends), `fpk.py` (pack, check, list, upload), `keys.py` (serial keys), `fh1dev.py` (keys, screenshots, stats and console ops from the PC), `hexfont.py`, `gbc_palettes.py` (generators) |

## Restoring the dev board

The board's original firmware is backed up outside the repo. To give the board back:

```sh
esptool --port /dev/ttyUSB0 -b 460800 write_flash 0 <backup>.bin
```
