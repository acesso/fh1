# FH1 firmware design

The base system for the FH1: the toolchain, the boards, peripherals managed as devices with capabilities, the
window-based interface, a web dev console, apps installed from packages without reflashing, the emulators, the
music player, the Bluetooth bridge and users. Building and using it: [README.md](README.md).

## Constraints

- **Buttons, not touch.** The interface is driven by the D-pad (the stick), A, B, X, Y, Start, Select and Menu. Even on boards that have a touch screen, touch is not the way to use it.
- **Retro windows, not a phone UI.** Windows cascade over a desktop. They have title bars, rounded corners, hard 1–2 px borders and a pixel font. There are no full-screen cards, swipes or kinetic scrolling.
- **Apps are packages.** An app can be copied to the SD card (or uploaded), then installed and uninstalled from the device. A full firmware flash is only needed for the base system.
- **Two boards.** The same base system and apps run on the FH1 and on the Guition 4848S040 dev board. Only a thin board layer differs.

## Toolchain

| What | Version | Note |
|---|---|---|
| ESP-IDF | v6.1 | |
| LVGL | 9.6.0 | The drawing and widget engine, with its default theme replaced (see [Interface](#interface)) |
| esp_lvgl_port | 2.9.0 | Display and input glue |
| elf_loader | 2.1.0 | Espressif's runtime ELF loader, for packaged apps |

Versions are pinned in `idf_component.yml`, and `dependencies.lock` is committed. The whole firmware builds
with `-O2`.

## Boards

| | FH1 | Guition 4848S040 (dev board) |
|---|---|---|
| MCU | ESP32-S3 N16R8 | ESP32-S3 N16R8 (8 MB PSRAM in the chip, 16 MB flash) |
| Display | 2.8" ST7789, 320 × 240, SPI | 4" ST7701S, 480 × 480, parallel RGB |
| Input | Stick, A B X Y, Start, Select, Menu (via two MCP23017) | Touch only. No buttons apart from BOOT and RST |
| Storage | micro-SD (1-bit SDMMC) | micro-SD |
| Audio | ES8311 and speaker | a PCM5102A module on the `H1` header (see [Dev-board audio](#dev-board-audio-a-pcm5102a-module)) |
| Radio | E28 (SX1281 LoRa) | none |
| USB | Native USB-C | CH340 USB-serial (reliable only up to 460800 baud for long reads) |

**Input on the dev board.** It has no buttons, so input comes from:

1. **The web dev console:** a gamepad on screen, the PC keyboard, or a real pad through the browser's Gamepad API. See [Dev console](#dev-console).
2. **Keys over the serial console:** `tools/keys.py` sends key presses when Wi-Fi is not up.
3. **Bluetooth controllers through a bridge:** the S3 only has Bluetooth LE, and most pads (DualSense, DualShock, Switch) are Classic-only, so they connect to an extension board (see [Extension board](#extension-board-a-bluetooth-bridge)).

Touch is not used as input; the touch controller is probed but switched off by default.

**Screen size.** The interface is laid out in units: the panel's own resolution by default, with spacing and
font doubled on high-resolution panels (the dev board: 480 × 480 at unit 2). The **FH1 320 × 240** mode draws
the FH1's screen 1.5× and centred on the dev board.

## Layers

```
apps (packaged .fpk, loaded at runtime)  │  built-in apps (launcher, installer, devices, games, music, users, ...)
─────────────────────────────────────────┴───────────────────────────────────────────────────
fh1 SDK: the versioned API apps may call (windows, canvas, input, gamepads, users, storage, audio, timers, capabilities)
window system and retro widget set (on LVGL)                     dev console (web)
services: settings · input events · events · storage · Wi-Fi and time · OTA · audio · bridges · users · log
device manager: registry · probing · enable/disable · capabilities · events
drivers, one interface per device class, and the null drivers
board: guition_4848s040 (which devices may be present, and where)
ESP-IDF v6.1
```

- **Board layer.** It declares which devices the board *may* have, and on which bus, address or pins. Everything above it asks the device manager, never the board directly.
- **Input events.** All input becomes the same events (`UP`, `DOWN`, `LEFT`, `RIGHT`, `A`, `B`, `X`, `Y`, `START`, `SELECT`, `MENU`, pressed or released, with repeat), whether it comes from the dev console, serial keys or a gamepad.

## Devices and capabilities

A peripheral can be missing, broken, or switched off by the user, and apps still start. They just run without
that feature. If the user disables the only audio output, a game starts silent; it does not crash.

### Five rules

1. **Every peripheral is a device in one registry.** Each one has an id, a class, a driver, a state and properties. Apps never hold a driver or a bus handle.
2. **The board declares, the driver probes.** The board layer lists the *candidate* devices ("an ES8311 may be at I2C 0x18"). At boot, each driver checks its chip is really there, by I2C acknowledge plus a chip-ID register, or a version register over SPI. Only then is the device marked ready.
3. **The user's choice wins.** Each device has an enabled/disabled setting, saved in NVS. A disabled device is not started, and where the hardware allows it is also powered down.
4. **There is always a driver.** When a class has no ready device, the HAL hands out a *null driver* of the same interface: `audio_out_write()` accepts the samples, drops them and returns `FH1_UNAVAILABLE`. An app that never checks anything still cannot crash on a missing device.
5. **Apps see capabilities and events, not hardware.** Capabilities are flags derived from the ready devices: `AUDIO_OUT`, `AUDIO_IN`, `LORA`, `WIFI`, `BLE`, `IMU`, `RTC`, `SD`, `STICK_ANALOG`, `BATTERY`, … When a device changes state, the manager posts `DEVICE_CHANGED` and `CAPS_CHANGED` events.

### Device states

```
absent ──probe ok──▶ ready ◀──enable── disabled
   ▲                   │ ▲                ▲
   └──── removed ──────┤ └── recovered ── failed
                       └──── user off ────┘
```

| State | Meaning |
|---|---|
| `absent` | Declared by the board, but the probe found nothing; or a hot-plug device not connected (SD card out) |
| `ready` | Probed and started; its capability is on |
| `disabled` | Switched off by the user; not probed, and powered down if possible |
| `failed` | Probed but it stopped answering, or errors repeated. The manager retries a few times with back-off, then leaves it failed until the next boot or a manual "probe again" |

### Handles, not pointers

Apps open a device class (`fh1_audio_out_open()`), not a device. They get a handle with a reference count. If
the device goes away while the app runs, the handle switches to the null driver, every call returns
`FH1_UNAVAILABLE`, and a `DEVICE_CHANGED` event is sent. If the device comes back, the handle switches back.

### Device classes

Each class has one small interface (a table of functions) that every driver implements, the null driver included:

| Class | Interface (main calls) | FH1 driver | Dev board |
|---|---|---|---|
| `audio.out` | open, write, volume, close | es8311 (+ NS4150B amp) | pcm5102a; a bridge's speaker |
| `audio.in` | open, read, gain, close | es8311 | null |
| `radio.lora` | config, send, receive (async), rssi, sleep | sx1281 (E28) | null |
| `input.buttons` | state, events | mcp23017 (A B X Y, Start, Select) + GPIO0 (Menu) | dev console, serial keys |
| `input.stick` | x, y (raw and calibrated), as D-pad events | ADC1 (GPIO2, GPIO4) | dev console |
| `input.gamepad` | as buttons + sticks, triggers, motion, rumble, light | through a bridge | through a bridge, dev console |
| `sensor.imu` | accel, gyro, steps, motion wake | qmi8658 | null |
| `rtc` | time, alarm | pcf8563 | null (system time from SNTP) |
| `storage.sd` | mount, unmount, card info; hot-plug | SDMMC 1-bit + DET on MCP #1 | SD slot |
| `power.battery` | voltage, percent, charging | divider on GPIO6 + charger CHRG/STDBY on MCP #2 | null |
| `display.backlight` | level | PWM | PWM |
| `light.buttons` | per-button LED, brightness | mcp23017 + PWM on GPIO44 | null |
| `link.bridge` | link state, devices, commands | BLE or Wi-Fi | BLE or Wi-Fi |

Wi-Fi and Bluetooth are devices too, so the user can switch them off and apps see `WIFI` or `BLE` go away.

### Apps declare what they need

The package manifest has two lists:

- **`needs`:** capabilities the app cannot run without. The launcher shows the app greyed out, with the reason ("needs LORA").
- **`wants`:** capabilities the app uses when present. It starts anyway and adapts.

### Shared resources

- **2.4 GHz airtime.** Wi-Fi, BLE and the LoRa radio share the band, through the ESP-IDF coexistence hooks.
- **Buses.** The I2C bus and the SPI buses each have one owner with a lock. Drivers never talk to a bus directly.
- **Power.** Each device reports its rough current draw when on, shown in the Devices window.

### The Devices window

A built-in window per device kind (Video, Sound, Network, Clock, Controls), with a view per device: its state,
driver, details and power draw, enable, disable, probe again, and its own settings. The dev console shows the same list.

## Interface

A desktop with cascading windows, operated by buttons.

- **Windows.** Text boxes in the style of late-1990s handheld RPGs: a light box with a double-line border and
  ball corners, the title in a small pill set into the top border. Items are plain text with a ▶ cursor;
  windows without focus show a hollow cursor. A new window opens down and right of the one before it. Windows
  open by growing from the top-left corner and close by shrinking back into it. All artwork is drawn by the code.
- **Buttons:**

| Button | Action |
|---|---|
| D-pad | Move focus between controls in the focused window |
| A | Activate |
| B | Back, or close the window |
| Select | Cycle the focused window through the stack |
| Start | The window's own menu |
| Menu | The launcher |
| X, Y | App-defined |

- **Look.** A bitmap pixel font (unscii-8, public domain, with Latin-1 accents), a small palette per theme.
  Themes: **Gen 1** (four greens) and **Gen 2** (white boxes, blue desktop), switched in Devices > Video.
- **LVGL underneath.** Our window and widget objects are built on LVGL objects, with LVGL's default theme
  replaced by ours. Each window gets an LVGL keypad group, so the D-pad moves focus inside the focused window only.
- **Widgets:**
  - **Paged lists** (`fh1_ui_list`): rows for one page only, filled by a callback when the page turns, and a
    page line ("Page 2 of 14 · 340 items"). Any long list uses it.
  - **Views** (`fh1_win_add_view`): a window with pages side by side, Left/Right to slide, a line on top naming
    the page; only the page shown takes focus.
  - **Sliders** (`fh1_ui_slider`): 0-100 % settings as a bar with the value in it; each step is previewed,
    the value is saved once the moving stops.
  - **Footers**: a two-column grid, one chip and a word of at most 9 characters per action (`FH1_HINT_CHARS`).
  - **Icons**: every System entry and every bridge menu entry has a small colour icon (`FH1_ICON_MENU_*`); the
    font has symbols for bridge, gamepad, Wi-Fi and Bluetooth (`FH1_SYM_*`).
  - **The tray** (bottom bar, right): one icon per connected gamepad, its shape by kind and drawn in its light
    colour, with its battery as a gauge; a speaker whose waves show the volume.
- **Overlays during a game**: an overlay event while a program owns the screen sets `fh1_app_should_yield()`;
  the program stops at a frame's end and calls `fh1_ui_lend_screen()`. The UI copies the frozen picture to a
  screen of its own, shows the overlay there, then returns to the windows' screen, and the game goes on.
- **Graceful restart** (`fh1_system_restart()`, `FH1_EV_SHUTDOWN`): the running game closes and saves, the
  windows close one by one, then the chip restarts (a fallback timer restarts anyway after 8 s).
- **Boot screen** (`fh1_post.h`): 8 × 8 text straight into the screen, one line per real step and device, and
  a progress bar; its buffer is freed when the window system starts.
- **Language**: Portuguese by default, English as the alternative. Strings are written in English in the code,
  wrapped in `T()`, and translated in `components/fh1_core/i18n.c`.

## Events

`fh1_events.h`: anything posts an event (a task, a timer, a driver); subscribers run in the UI task. Recurring
events fire at a random interval within a range (`fh1_event_every`).

`FH1_EV_TRANSFER`: a file arriving over the console, or a firmware update (over the network or from the SD
card), reports its progress with `fh1_transfer_report()`. The UI shows it in an overlay in the middle of the
screen: an animated icon of what it is, the file name and a progress bar. It closes 1.5 s after the end (3 s
after a failure, or after 15 s without news).

**Notifications** (`fh1_ui_notify`): a small box slides in at the top right for 4 s with a face, a title and a
line. They queue, and wait while a full-screen game runs.

## Dev console

A web page served by the device over Wi-Fi, for monitoring the system live and driving it remotely. It never
mirrors the screen; screenshots are taken on demand.

- `esp_http_server` serves one static page, gzipped and embedded in the firmware.
- A WebSocket carries live data from the device and input from the browser.
- **Wi-Fi setup:** credentials are set over the serial console (`!wifi`) or the Wi-Fi window. If there are
  none, the device starts its own access point.
- **Access:** a 6-digit code that the device prints in its boot log and shows in its System window.

| Panel | Shows / does |
|---|---|
| System | Uptime; CPU load per core; tasks with state, priority and free stack; free heap (internal and PSRAM, minimum, largest block); flash and SD usage; Wi-Fi signal; display health |
| Devices | The device registry, with enable / disable / probe again |
| Apps | Installed apps, the running app and its memory; start, stop, upload, install, uninstall |
| Log | The ESP-IDF log, live |
| Controller | An on-screen gamepad, the PC keyboard, or a real pad through the browser's Gamepad API. It is a gamepad ("Web console") that can be chosen as a player |
| Firmware | Uploads a new firmware image |

The task list comes from `uxTaskGetSystemState`, every 10 s, with the other core held inside an IPC call while
the tasks are read. `tools/fh1dev.py` drives the console from the PC (keys, screenshots, stats, console ops).

- **Stats pushes** go every 2 s to up to 4 clients (~0.8 KB each); the device and app lists (3.8 KB) only when
  they change or a client has just joined (the page keeps the last ones). A new client past 4 replaces the
  oldest. A client that cannot take a push is closed at once, and TCP keep-alive (5 s idle, 3 probes 2 s apart)
  closes one that went away: an open socket keeps its unsent data in internal RAM until TCP gives up.
- **Uploads** are received into one 4 KB DMA-capable buffer and written with `write()` in whole blocks: the SD
  driver writes straight from it (from PSRAM or through stdio it needed a 3.5 KB internal bounce buffer per write).
- **Dev ops for memory and data:** `heap` (internal RAM free, largest block, lowest ever, and the allocations that
  failed: counted, the last four kept; `heap reset` counts again), `data_list` (an app's files per user),
  `data_copy` (an app's data from one user to another, the target's kept in a `.bak` folder).

## App packages

**Format.** An `.fpk` file is an uncompressed tar holding:

| File | What |
|---|---|
| `manifest.json` | `id` (reverse-DNS), `name`, `version`, `sdk` (the SDK major.minor it was built for), `needs` and `wants`, `entry` |
| `app.elf` | The app, built against the fh1 SDK as a relocatable ELF |
| `assets/` | Optional: images, sounds, data |
| `SHA256SUMS` | Checksums of the files above |

**Install flow:**

1. The package lands in `/sd/packages/` (copied from a computer) or in `/int/packages/` (uploaded through the dev console).
2. The **Installer** lists the packages, checks the manifest (SDK version) and the checksums, then unpacks the package to `/int/apps/<id>/`.
3. The launcher shows it. The registry is the `/int/apps` folder itself: each app folder holds its `manifest.json`. An app whose `needs` are not met can still be installed: it shows greyed out until the capability is there.
4. **Uninstall** deletes `/int/apps/<id>/` and its saved data (after asking first).

**Run.** The launcher checks `needs` against the current capabilities, then loads `app.elf` with `elf_loader`
into PSRAM, resolves its symbols against the SDK's exported symbol table, and calls its entry. When the app's
last window closes, it is unloaded and its memory freed.

**SDK and ABI.**
- The firmware exports a versioned symbol table: our API (`fh1_sdk.h`) and a C library subset (listed at the top of `fh1_sdk.h`, including the soft-float division `__divsf3`: the S3's FPU has no divide).
- A minor version only adds symbols. A major version means apps need rebuilding. The installer refuses apps built for a newer SDK than the firmware has.
- Apps are built with an SDK CMake template (`apps/packages/hello`), and `tools/fpk.py` packs, checks and uploads them.
- Packages build with ESP-IDF's default `-Og`; an app that draws in loops sets `CONFIG_COMPILER_OPTIMIZATION_PERF=y` in its `sdkconfig.defaults` (Duelo's sprites draw twice as fast with it).

| SDK | Adds |
|---|---|
| 0.1 | Windows, timers, input, storage, audio, capabilities |
| 0.2 | The players' controllers (`fh1_pad`, `fh1_pad_info`, `fh1_pad_motion`, `fh1_pad_hold_keys`) and a canvas (`fh1_canvas*`) |
| 0.3 | `fh1_canvas_tri`, `fh1_canvas_text_width` |
| 0.4 | Users: `fh1_user`, `fh1_user_face`, `fh1_notify`, `fh1_setting_get` / `_set`, `fh1_state_save` / `_load`; `fh1_app_data_dir()` is player 1's user's folder |
| 0.5 | Full screen: `fh1_screen_open` / `fh1_screen_close` (the app's `frame()` on its own task, a back buffer), `fh1_app_dir`, `fh1_px_text` |
| 0.6 | Development commands from the console: `fh1_dev_command` |
| 0.7 | Asset packages (`fh1_asset_dir`, `"uses"`) and sound effects (`fh1_sfx_play`, `_stop`, `_stop_all`, `_playing`) |
| 0.8 | Player 1's controller's button pictures: `fh1_pad_style`, `fh1_px_key`; the data folder follows player 1's user at every call |

**Limits.**
- Apps run in the same address space as the firmware, with no isolation. A crashing app reboots the device, and the watchdog catches hangs.
- Code run from PSRAM is slower than code from flash.

**Duelo** (`apps/packages/duel`): a sword duel for two players, best of three rounds, on SDK 0.3. The
controllers' motion are the blades; a shoulder button guards (pressed just before a blow: a parry that
stuns), the right face button dodges back. Without motion the D-pad aims and the bottom button strikes; the
FH1 plays when player 2 has no controller. The fighters (`main/fighter.c`) are 80 × 92 sprites in a 16-bit
fighting-game style, drawn from shapes into a mask by row spans (3.8 ms each on the S3) and drawn again only
when their pose changes; the game blits them as runs of one colour. 30 frames a second, ~12 ms a frame.

## Flash layout (16 MB)

| Name | Type | Size | What |
|---|---|---|---|
| nvs | data | 24 KB | Settings, identities |
| otadata | data | 8 KB | |
| phy_init | data | 4 KB | |
| ota_0 | app | 4 MB | Firmware, slot A |
| ota_1 | app | 4 MB | Firmware, slot B |
| apps | data (LittleFS) | ~7.8 MB | `/int`: installed apps, games, music and data |
| coredump | data | 64 KB | Crash dumps for debugging |

## Firmware updates

Firmware updates go through the two OTA slots, over Wi-Fi (`POST /ota`) or from a file on the SD card. A new
firmware that has not run for 10 s with the UI up is rolled back on the next reset. Apps are independent of
firmware updates, as long as the SDK major version stays the same.

## Emulators

Three emulators are built into the firmware, not packaged: they are memory-bound, and loading them as an ELF
into PSRAM would add instruction fetches to the same bus. They share one front-end shape: a game library
(`apps_games.c`), emulation on core 1, a blitter on core 0, an options menu over the frozen picture, battery
saves and save states, and a 20-second speed test.

| App | Emulator | Licence | Speed test (emulation only / while drawing) |
|---|---|---|---|
| GameBoy Color | Peanut-GB (cgb branch), sound by `minigb_apu` | MIT | Pokémon Red 132 / 101 frames/s; Tobu Tobu Girl (Color) 108 / 70 |
| NES | nofrendo as maintained in retro-go (~40 mappers) | GPL v2 | Metroid (PAL) 163 / 119 frames/s |
| Master System | SMS Plus as maintained in retro-go (also Game Gear, SG-1000) | GPL v2 | Rassak's Retribution 291 / 176 frames/s |

- **Game Boy:** 3x (480 × 432) on the dev board. Sound runs on core 0: the game's register writes are logged
  per frame and replayed there (~1 ms per frame). Drawing drops to half rate when it falls 20 ms behind, and
  Peanut-GB's frame skip saves the rendering of undrawn frames. Original Game Boy games are coloured as a
  Game Boy Color colours them (tables from SameBoy's boot ROM, `tools/gbc_palettes.py`).
- **NES:** 1:1, 240 × 224 on the dev board's 240 × 240 screen (8 columns cut off each side), 256 × 240 on the
  FH1. Playing Metroid: 50.0 frames/s, 10.1 ms of the 20 ms per frame. The output gets a 2.5x makeup gain.
- **Master System:** patched to free its lookup tables (~576 KB of PSRAM) at shutdown, to keep the cartridge
  RAM in PSRAM, and to skip sound emulation without a sound device.
- **Licence:** the two GPL v2 emulators are the parts to replace if the firmware is ever distributed under
  another licence; see their `VENDORED.md`.
- Without a sound device, the sound chips are not emulated.

## Memory: the running app first

Internal RAM (~70 KB free at the desktop) is several times faster than PSRAM and is what Wi-Fi needs; the
emulators are memory-bound. A full-screen app gets the internal RAM, and nothing that is not on screen keeps
any while it runs:

- **The window system lives in PSRAM.** LVGL's heap uses a PSRAM allocator (`fh1_ui/lv_mem_psram.c`), as do
  sprites, title frames, the dev console's JSON and log ring. Opening windows costs about 1 KB of internal RAM.
- **Freeze while paused.** `fh1_ui_pause(true)` gives back the one internal block the interface needs, its
  15 KB draw buffer; resuming takes it again.
- **Fast memory in priority order.** `fh1_mem_fast()` (`fh1_core/fh1_mem.h`) returns internal RAM while 32 KB
  stays free (Wi-Fi bursts, the app's tasks), else PSRAM. Apps ask for their hottest data first. Internal RAM
  is fragmented (largest block ~31 KB), so big blocks are split: the Game Boy's work RAM in two halves and ROM
  bank 0 in 4 KB pieces.
- **Nothing static per app.** The emulators allocate their machine state when a game opens.
- **Wi-Fi and lwIP buffers stay in internal RAM** (`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` is off: it forces
  static TX buffers that hold ~26 KB of internal RAM for good), **with a cap** on what Wi-Fi borrows: 8 dynamic
  TX and 16 dynamic RX buffers (~1.6 KB each; 32 of each let five screenshots take internal RAM to 120 bytes;
  10 RX buffers halved the upload speed).
- **Measured** (internal RAM free, lowest): idle ~23 KB after boot, ~35 KB once boot settles; boot's lowest
  ~8 KB; five screenshots in a row ~2.5 KB (Wi-Fi drops some frames and retries; free again at once); a 1 MB
  upload no failed allocation, 3.2 s. Before the console fixes, clients that went away held it at 3 KB for
  minutes and at 0 bytes at times.
- **Bluetooth** takes ~26 KB of internal RAM while on; its controller code runs from flash
  (`CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY`) and its pools are sized for small use. It is off unless adding a bridge.

**Speed settings.** CPU 240 MHz; caches at their maximum (32 KB instruction, 64 KB data, 64-byte lines);
octal PSRAM at 80 MHz (120 MHz is experimental for octal PSRAM and fails when the chip's temperature drifts);
code runs from PSRAM, faster than the quad flash; `-O2`.

## Display path

The dev board's RGB panel has no memory of its own: it must be fed every line, about 38 times a second. The
screen is kept at its logical size (240 × 240 or 320 × 240) in PSRAM, and the panel driver's bounce-buffer
refill scales each band of lines on the fly. With the stress test (`op stress`): no missed lines under UI,
PSRAM, Wi-Fi or emulator load. Continuous flash writes still cause hiccups (the flash chip holds the shared
bus), so flash writes stay out of busy moments. Wi-Fi modem power save is off.

Long-running loops must block now and then (the emulators sleep one tick every 4 frames when behind): a loop
that never blocks starves core 1's idle task, and the task watchdog's report from an interrupt makes the panel
miss its refills. The console reports late refill starts (`late`, `odd`, `max_gap_us`), and the driver logs
each glitch as a `disp:` warning.

## Dev-board audio: a PCM5102A module

The dev board has no sound device of its own. A PCM5102A module (I2S DAC, line out, the purple "EstarDyn"
board) on its `H1` header gives it one. Reference photo: `docs/guition-4848s040.jpg`. `H1` seen from the
component side, with the `H1` label above it and the panel's flex connector to its left:

| Left column | Right column |
|---|---|
| GND | GND |
| GPIO40 | RXD (UART0, the console) |
| GPIO2 | TXD (UART0, the console) |
| GPIO1 | 5V |

| PCM5102A | 4848S040 `H1` | Note |
|---|---|---|
| `VIN` | `5V` | the module makes its own 3.3 V |
| `GND` | `GND` | |
| `BCK` | `GPIO1` | I2S bit clock |
| `LCK` | `GPIO2` | I2S word select |
| `DIN` | `GPIO40` | I2S data |
| `SCK` | GND, by the module's solder bridge | the chip makes its own clock from BCK |
| `H1L` (FLT), `H2L` (DEMP), `H4L` (FMT) | solder to `L` | normal filter, no de-emphasis, I2S format |
| `H3L` (XSMT) | solder to `H` | always unmuted; the firmware mutes by stopping I2S |
| `LROUT` / `ROUT` + `AGND` (or its 3.5 mm jack) | headphones or a small amplifier | line level, up to 2.1 Vrms |

Those three GPIOs reach `H1` while the board's 0-ohm resistors R25-R27 are in their factory place.

**Driver** (`fh1_board/pcm5102a.c`): I2S standard mode, 16-bit stereo at the source's rate, no MCLK; volume
is a digital gain. The chip cannot be probed (no I2C, nothing to read back), so the board registers `audio0`
switched off; the user enables it in Devices once wired. The driver logs its rate, dropped frames,
underruns and peak level every 5 s while sound plays.

## Music

**What it plays:** files in `/sd/music` and `/int/music`, three folder levels deep (artist/album/track), in
path order. Formats and decoders (`components/fh1_player`, all permissive, see its `VENDORED.md`):

| Format | Decoder | Tags | Seeking |
|---|---|---|---|
| MP3 (MPEG 1/2/2.5, layers I-III; CBR and VBR) | minimp3 | ID3v2.2-2.4, ID3v1 | by file position (Xing table of contents when there is one) |
| FLAC | dr_flac | Vorbis comments | exact |
| Ogg Vorbis | Tremor (integer) | Vorbis comments | exact |
| WAV (PCM 8-32 bit, float, A/mu-law, ADPCM) | dr_wav | - | exact |
| MOD (ProTracker and compatible) | pocketmod | song name | none; plays once through |

**Engine (`fh1_player`):** one decoding task on core 0 with its 16 KB stack in PSRAM, so playing costs almost
no internal RAM. A task whose stack is in PSRAM must not touch the flash, and internal storage is flash, so
every file operation runs on a small helper task (`player_io`, 3.5 KB internal stack) through a 4 KB internal
buffer. The decoders allocate in PSRAM first (`shim/pl_alloc.h`). The output is stereo at the file's rate
through `fh1_audio`. The last 8192 frames played are kept for the visualisers (`fh1_player_tap`).

**Library window:** a paged list (the track playing marked with »). A plays and opens the deck, X opens the
deck, Y stops, B closes (music plays on). The play mode (repeat all, repeat one, shuffle) is saved. Starting a
game stops the music.

**The deck** (`music_deck.c`): full screen, drawn straight into the logical screen through a back buffer (the
window system paused), 30 frames a second on core 1. A seven-segment time, track and state, a 12-segment
stereo level meter with a clipping light, the title scrolling, the position as a segmented bar. Four
visualisers (X): **SPECTRUM**, **WATERFALL** (a spectrogram), **SCOPE** and **VECTOR** (goniometer), from a
2048-point FFT. Four colour schemes (Y): amber, phosphor green, synthwave, Game Boy LCD.

## Extension board: a Bluetooth bridge

**Why.** The S3 has only Bluetooth LE. Most game controllers (DualSense, DualShock 4, Switch Pro, Joy-Con, Wii)
speak only Bluetooth Classic, which of Espressif's chips only the original ESP32 has. A bridge is an ESP32 board
(`bridge/`) that holds the controllers on Classic with Bluepad32 (Apache 2.0, on BTstack) and talks to the FH1
over BLE or Wi-Fi. On the FH1 its devices appear in the device manager like local ones; apps see gamepads, not
bridges. Boards: ESP32-WROOM-32 and ESP32-A1S Audio Kit (`bridge/README.md`).

### Links

| | BLE | Wi-Fi (UDP) |
|---|---|---|
| Roles | the bridge is a GATT server (`bridge/main/fh1_bridge.gatt`), the FH1 a NimBLE central | the same messages as datagrams on port 47211 (`'F' '1' type message`) |
| Device on the FH1 | `bridge0`, off by default (Bluetooth starts only then) | `bridge1`, on by default |
| Finding bridges | scanning while none is connected, and during a search | a bridge with Wi-Fi broadcasts a beacon every 2 s while unlinked |
| Use | adding a bridge and giving it a Wi-Fi network (credentials only over an encrypted BLE link: LE Secure Connections, Just Works, nothing stored) | normal use |

A bridge that beacons is taken over Wi-Fi. A link with nothing for 3 s is dropped and made again. The FH1 holds
up to 2 bridges at once and remembers 4 (NVS `bridges`: address and alias, the alias being the address's last
4 hex digits). Over Wi-Fi the input goes when it changes (a button change sent twice, 16 ms apart), at most
every 16 ms, with a 50 ms heartbeat while a controller is connected; motion streams only while something asks
(`fh1_gamepad_want_motion`), at most every 25 ms. Bridges answer mDNS as `fh1-bridge-<4 hex>.local` and take
firmware updates over Wi-Fi (`POST /ota`).

### Protocol

`components/fh1_bridge/include/fh1_bridge_proto.h`, shared by both firmwares (protocol 7):

| Message | Direction | Content |
|---|---|---|
| `info` | read | protocol version, firmware version, board name, capability flags, limits |
| `devices` | read, notify on change | the attached devices: id, class, state, name and model, what each reports, battery, light colour |
| `input` | notify | packed states of every ready input device, with a sequence number and the bridge's time in ms |
| `command` | write | pair (scan for 60 s), forget, enable/disable, rumble, light colour, player LEDs, adaptive triggers, motion, status, board devices and their controls, Wi-Fi |
| `event` | notify | device appeared or gone, pairing finished, keys, controls, low battery, errors |

On connect the FH1 reads `info` and `devices`, registers each device, and subscribes to `input`, `devices` and
`event`. A version mismatch shows as the bridge device in `failed` with the reason.

### On the FH1

- **Gamepads** (`fh1_core/gamepad.c`, `fh1_gamepad.h`): the core keeps each gamepad's full state (every button,
  both sticks, both analog triggers, gyro and accelerometer) and its outputs (rumble, light, DualSense
  adaptive triggers). Player 1's controller drives the interface (circle A, cross B, triangle X, square Y,
  Options Start, Create Select, PS Menu, the left stick as the D-pad). `fh1_input_player_keys(n)` gives player
  n+1's keys to the emulators. Player LEDs show each controller's player number.
- **System > Inputs and outputs:** players 1 and 2 (automatic, the FH1's own keys, or a chosen controller,
  remembered by its address and shown "(away)" while it is not connected), the sound output, and every
  connected controller with its window: Test inputs (buttons, sticks, triggers, gyro, accelerometer, a tilt
  bubble and a heading needle), Vibrate, Light colour, Triggers, player, battery, bridge, vid:pid, address.
- **System > Bridges:** Bluetooth's switch, "Search for bridges", and each bridge's window: Devices (pair,
  forget, its controllers, its board's devices), Connectivity (the BLE link; its Wi-Fi network), Storage,
  About, Rename, Forget.
- **A bridge's board devices** (`FH1B_CMD_BOARD`): on the A1S (ES8388 codec, `bridge/main/board_a1s.c`), its
  speaker and headphone outputs, its 6 keys (a gamepad "Audio Kit keys" a player can choose), its LED, the
  headphone jack's detect pin and the SD card's size. Each device describes its controls (a switch, a choice
  or a range, `fh1b_ctrl_t`); the bridge keeps their values in NVS and the FH1 draws them generically.
- **Sound to a bridge:** a bridge whose board has an audio out is a sound output on the FH1 (`baudio0` /
  `baudio1`, class `audio.out`) while linked over Wi-Fi, chosen in Inputs and outputs > Sound. The FH1 sends
  16-bit PCM at 24 kHz stereo by default (a 31-tap low-pass before halving 44.1/48 kHz; 48 kHz stereo and
  24 kHz mono can be chosen), paced as a codec would (250 ms ahead). The bridge (`audio_rx.c`) plays from a
  ~370 ms ring, starting at 200 ms, filling lost packets with silence. One writer at a time.

### Measured

| | Result |
|---|---|
| DualSense report rate on the bridge (Classic) | ~395 a second alone; 324 / 416 with two |
| BLE link, two DualSense on the bridge | ~111 input notifications a second, 0 lost; round trip median 34.5 ms (20-61), ~10 ms with no controllers |
| Wi-Fi link (A1S) | one-way input delay spread 8-30 ms, no losses |
| Sound over Wi-Fi to the A1S | 24 kHz stereo: 0 lost, 0 underruns |
| FH1 internal RAM | Bluetooth on: ~26 KB less; Wi-Fi uploads about halve while a BLE link runs |

- The bridge scans for new controllers only when asked to pair: scanning steals its radio (a ~24 ms hole every
  second). It also pauses its Wi-Fi while pairing.
- The bridge's run loop has a watchdog: 5 s without a tick aborts, saves a core dump and restarts it.
- Not in Bluepad32, so not here: the DualSense's touchpad, speaker, microphone, headset jack and voice-coil haptics.

## Users

Users with key-based ids (no collisions between FH1 units, no server, no clock), an owner per unit, avatars
drawn by code from a 12-byte code, notifications with faces, people nearby and friends between units. The
design and formats are in [docs/users.md](docs/users.md).

## Open questions

1. **Package signing.** Checksums only. Add Ed25519 signatures before packages come from anywhere but our own builds?
2. **Package index.** A static JSON file plus `.fpk` files on GitHub Releases is the simplest.
3. **Hot-plug on the FH1.** The SD card has a detect line. Should the I2C devices (RTC, IMU) also be re-probed now and then, or only at boot and on "probe again"?
4. **Device power numbers.** The current-draw figures per device start as datasheet values. Measure them on the first FH1 build.
