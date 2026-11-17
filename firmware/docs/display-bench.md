# Screen speed: the bench and the boards

How fast each board gets an app's picture to its panel, measured on the units over Wi-Fi.

## Running it

```sh
FH1_HOST=<unit> FH1_CODE=<code> python3 tools/fh1bench.py              # all scenarios, a table
FH1_HOST=<unit> FH1_CODE=<code> python3 tools/fh1bench.py screen menu  # some of them
FH1_HOST=<unit> FH1_CODE=<code> python3 tools/fh1bench.py --json       # a JSON line per scenario
```

The `screen` scenario needs the **Teste de tela** package (`apps/packages/screentest`; build it with
`tools/pkgbuild.sh apps/packages/screentest`, install it like any package). Opened by hand it is also a test card for
the eye: colour bars, four ramps (banding or a missing bit shows), 1-pixel lines and a checker (a shifted or dropped
bit shows), a red border on the screen's very edge. A and B step through the patterns.

Scenarios:

| Scenario | What runs |
|---|---|
| `home` | the home screen, still, 10 s |
| `menu` | the Apps menu scrolling up and down, 10 s |
| `screen:motion` | every pixel changes every frame (sliding stripes): the worst case for every link |
| `screen:sprites` | 16 boxes bouncing over the test card: small changed areas, the usual case in games |
| `screen:half` | the top half scrolling, the bottom still |
| `screen:card` | the test card, still |

Teste de tela also has **tear**, not in the bench (tearing happens on the glass: no counter or screenshot sees it):
white bars sweeping across a dark screen, a screen's width in half a second. Where a frame reached the panel while
it was redrawing itself, a bar shows a step; film it at 240 fps to see. Open it by hand (A steps to it) or with the
dev command `mode tear`.
| `construcao` | Construção Legal walking on open ground in the player's own world (it saves the player's place on leaving) |
| `duelo` | Duelo against the FH1, moving and striking |
| `taticas:title` | Táticas da Ilha's title screen and its music (the game's saving off) |
| `taticas:show` | Táticas da Ilha's heroes' showcase: effects and sounds |

Columns:

- **App fps:** the frames a second the app reached. The test app asks for a frame every 10 ms (the firmware's
  shortest period), so 100 is the ceiling: below it, drawing and the copy of the changed rows to the screen took
  longer.
- **Draw ms:** the app's own drawing, a frame (whole milliseconds, averaged).
- **Gap ms:** the longest time between two frames.
- **Panel fps:** what the panel got. A panel with its own memory (ST7789) gets only the changed areas, at most 60
  times a second at 80 MHz SPI (30 at 40 MHz: the time the link takes for a full frame); one without (the Guition's
  ST7701S) is refreshed at its fixed rate, whatever changed.
- **Bands/s, Band us:** 20-line bands prepared for the panel a second, and the slowest one.
- **Late, Odd:** bands too late for the panel and frames without their bands: likely visible. Zero is right.
- **CPU0%, CPU1%:** each core's load. Apps run on core 1; the ST7789's sends are prepared on core 0.
- **Audio%, ADrop, AUnder:** where the sound driver logs its health (the radio's PCM5102A, every 5 s of continuous
  sound): the slowest window's frames against the rate, samples dropped, underruns (gaps you hear).

## The boards

The Guition with PSRAM and flash at 120 MHz (`sdkconfig.defaults`). Radio is the S3 bridge board running the FH1
firmware with the FH1's ST7789 module at 80 MHz SPI, the closest unit to the FH1 board; its PSRAM and flash run at
80 MHz (below).

| Scenario | Guition: app fps | draw ms | panel fps | CPU1 | Radio: app fps | draw ms | panel fps | CPU0 | CPU1 |
|---|---|---|---|---|---|---|---|---|---|
| home | | | 38.1 | 7% | | | 30.2 | 4% | 8% |
| menu | | | 38.0 | 15% | | | 35.9 | 8% | 15% |
| screen:motion | 61.9 | 8.8 | 38.1 | 94% | 84.9 | 5.0 | 52.2 | 38% | 92% |
| screen:sprites | 72.2 | 6.7 | 38.0 | 93% | 78.9 | 5.6 | 62.2 | 34% | 92% |
| screen:half | 86.9 | 5.5 | 38.1 | 92% | 99.8 | 3.3 | 49.2 | 14% | 86% |
| screen:card | 85.7 | 5.7 | 38.0 | 91% | 99.7 | 4.0 | 1.0 | 3% | 88% |
| construcao | | | 35.2 | 52% | | | 30.2 | 19% | 37% |
| duelo | | | | | | | 47.1 | 15% | 53% |
| taticas:title | | | | | | | 18.7 | 6% | 34% |
| taticas:show | | | | | | | 34.9 | 16% | 65% |

On the radio nothing came late or broken in any scenario, its internal RAM's low point stayed at 8-10 KB, and its sound
stayed whole where the driver measured it (the showcase: 101% of the rate, nothing dropped, no underruns). Construção
Legal stays at 30 because the game asks for a frame every 33 ms (it draws one in about 12 ms). The emulators are not
in the bench: it would need a game the repository can carry.

- **The Guition does not tear:** its panel is fed from a copy of the screen, into which the rows that changed go
  just after each v-sync, top first (well ahead of the scan), and only between whole frames (a frame being written
  waits for the next v-sync). That costs it about 8% of the app's frame rate on full-screen motion (61.9 to 57.3 fps;
  the panel still gets its 38) and 11% of core 0. The emulators, which draw into the screen by themselves, are shown
  straight from it as before.
- **The radio and the FH1 board tear on fast motion:** the ST7789 redraws itself from its own memory on its own clock,
  top to bottom in landscape, and its module brings no TE (tearing-effect) line out; its SDA-O does not answer reads in
  the module's serial mode. A module with TE out is the fix (`hardware/open-questions.md`, 25).
- **The Guition draws a third more pixels** (320 x 320, shown 1.5x on its 480 x 480 panel; radio 320 x 240), and its
  panel is refreshed whole from PSRAM 38 times a second, which competes with the app for memory: the same patterns
  take it 1.3-1.8x as long.
- **The panels:** the Guition shows a fixed 38 frames a second; the radio up to 60 changed screens a second (its link,
  80 MHz SPI, carries a full frame in 16.8 ms). Full-screen motion reaches 52 there, small changes 62, Duelo 47.
- **Still screens cost the radio nothing** on the link (card: 1 panel update a second); the Guition refreshes all
  the time.

**120 MHz on the radio:** its screen went faster (motion 100 fps, the test's ceiling, at 88% CPU; sprites 100 at 78%;
the slowest band 0.8 ms against 2.2 ms), but its flash, XMC in DIO mode, does not hold that clock: a game hung while
saving and the sound went wrong. Writes to its storage: 10 KB fine; 300 KB a panic when sent at full speed, fine at
20 KB/s; an OTA panicked 15-18 s in at any upload speed (5-20 KB/s, Bluetooth on or off), during the long erase of
the slot (the updater now erases as it writes instead). All at a steady 48 °C, minutes after power-on (not the temperature drift ESP-IDF warns about;
its XMC chip is one ESP-IDF's high-performance mode supports, but the board's flash runs in DIO). So the radio's build keeps
PSRAM and flash at 80 MHz. A board for 120 MHz needs its flash in QIO (the Guition's is; the FH1 board's module is
too).
