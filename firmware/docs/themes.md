# Themes: how they are built, and how to build one

A theme is a palette, a **look** (how windows, the desktop and the cursor are drawn) and a **voice** (the system
sounds). Five themes are built in:

| Theme (id) | Look | Voice |
|---|---|---|
| Gen 1 (`gen1`), Gen 2 (`gen2`) | Gen: rounded boxes, corner balls, the title in a pill | soft |
| Hex (`hex`) | Hex: chamfered boxes, a band of running warning stripes, a honeycomb | alarm |
| Garden (`garden`) | Garden: wooden frames, a vine, corner flowers, a butterfly, a lawn | garden |
| Command (`scifi`) | Command: steel plates, corner brackets, a lit tab, a running spark, a grid | command |

They are chosen in the Temas app (in Apps, and in System before About), in System > Devices > the screen, or with
`!theme <id>` on the console. A change applies at once: no restart.

## Where things are

| File | What |
|---|---|
| `components/fh1_ui/ui.c` | The theme table (`s_themes`), the window system, Gen and Hex drawn in place, the framed looks' glue (`frame_render`, `frame_build`, `look_tick`, `mote_tick`), `apply_theme` |
| `components/fh1_ui/ui_look.h`, `ui_look.c` | The framed looks' pictures, plain C with no LVGL: desktop tile, frame strips, corners, title plate, cursor, mote |
| `components/fh1_ui/ui_sound.c` | The voices: each system sound synthesised in memory the first time it is needed |
| `components/fh1_ui/ui_priv.h` | The sound events (`UI_SND_*`) and voices (`UI_VOICE_*`) |
| `components/fh1_core/i18n.c` | The theme names in Portuguese |
| `tools/lookpreview.c` | Renders a framed look on a computer: three windows and the opening, as PPM files |
| `components/fh1_apps/apps.c` | The Temas app (`themes_open`) |

## The theme table

```c
{ "garden", "Garden", paper, ink, dim, desk_a, desk_b, alert, lattice, LOOK_GARDEN, text, inset, UI_VOICE_GARDEN },
```

- **paper**: the windows' background; **ink**: lines, the status bar, icons, tabs; **dim**: windows out of
  focus, hints, separators; **text** and **inset**: body text and the inner line (0 takes the ink).
- **desk_a**, **desk_b**: the desktop's two main colours; the look's tile builds on them.
- **alert**: messages and questions (Gen uses it for the top half of its corner balls).
- Widgets that set a colour of their own from the palette (a footer's line, a pager, a slider) follow a theme
  change: `recolour_tree` maps the old paper, ink and dim to the new ones. A widget that needs another palette
  colour must be handled the same way.

## A framed look (Garden, Command)

The window keeps its 2-unit LVGL border, made invisible, so everything inside sits where it does in every look.
The look draws over it, as ARGB8888 pictures in PSRAM, one per part:

| Part | Function | Size and place |
|---|---|---|
| Four strips (top, bottom, left, right) | `look_strip` | `look_strip_thick` units in from the box's edge, plus `look_strip_out` units outside it (leaves hanging over) |
| Four corners | `look_corner` | `look_corner_size` square, `look_corner_out` units past the box's corner |
| Title plate | `look_title_box`, `look_title`, `look_title_ink` | anywhere along the top; the title's text is placed on it |
| Desktop tile | `look_desk` | RGB565, at most `LOOK_DESK_MAX` pixels a side |
| Cursor | `look_cursor` | at most 16 x 16 pixels; solid in the window in focus, `hollow` in the others |
| Mote | `look_mote`, `look_mote_at` | at most 16 x 16 pixels, two pictures; where it is at a given time |

Each part is drawn for a **state**: 0 out of focus, 1 in focus, 2 an alert. A **grow** value (0 to 256) says how
far the opening has got, and a **phase** counts the idle decoration's steps. Everything is drawn in units:
`put()` paints one unit as a `u` x `u` block (u is 1 on the FH1, 2 on a 480 x 480 screen), so a look is designed
once, at 320 x 240.

Rules the built-in looks keep:

- **Each pixel is chosen.** Outlines one unit wide and darker than what they hold; light from the top-left
  (lit edges top and left, shade bottom and right); no anti-aliasing except a half-transparent glow.
- **A strip's row 0 is its outermost.** `frame_render` flips the bottom and right strips into place.
- **Perimeter order is clockwise from the top-left** (top, right, bottom, left), so something can grow or run
  round the window (`perimeter_pos` in `ui_look.c`).
- **Repeating details by a hash, not a grid.** Grain, blades of grass and flowers fall where `hash()` puts them,
  so a tile repeats as little as it can. Make the tile as large as `LOOK_DESK_MAX` allows.
- **Out of focus is dimmer, an alert is red.** `state_tint()` does it for wood or metal; the look decides its
  lights and flowers.
- **Keep the window's inside clear.** A strip over 5 units deep, or a corner arm wider than the strip,
  covers the text and cursor.
- **The title plate rises at most 4 units above the box; nothing reaches more than 10 units out.** The box
  redraws 10 units round itself (`box_ext_draw`); past that, pixels are left behind when things move.

## Motion, and what it costs

Each kind of motion has its own cost, so each is limited:

- **The opening** (`frame_build`, 650 to 700 ms): the strips are drawn again while they change. Garden draws
  only the sides the vine passed over in that frame, and the corners, while the window fades in. Command moves
  its corners (positions only), draws the strips while the plates close, then fades in the glass behind a scan
  line. Closing runs the same backwards.
- **The idle decoration** (`look_tick`, every 125 ms, only the window in focus): only small parts are drawn
  again. Garden draws its corners every 500 ms (the flowers sway). Command draws its corners every 750 ms (the
  lights blink) and its tab every 375 ms (the lights cycle). Never draw a whole strip on a tick.
- **The mote** (`mote_tick`, 20 times a second, only the window in focus) is moved, never drawn again, and only
  when its place changes (`move_to`).
- **Objects are raised once.** `lv_obj_move_foreground` makes the whole window redraw. Raise only when a
  part is created (`frame_order`), never on a tick.

To measure, run the same key script on each theme in the native simulator and compare the instruction counts
it prints at the end (`insns=` for each core). A 24-second run, menu open, measured on both cores:

| Theme | Gen 1 | Hex | Garden | Command |
|---|---|---|---|---|
| Instructions | 276 M | 275 M | 311 M | 318 M |

A new look should stay near Garden and Command.

## Sounds

`ui_sound(voice, event)` plays one of `UI_SND_MOVE`, `SELECT`, `BACK`, `ALERT`, `NOTE` and `OPEN` (a window
opening; only Garden and Command have it). A voice is a `case` in `make_set()` in `ui_sound.c`, built from
`tone`, `ping`, `marimba`, `sweep`, `noise` and `silence`, at 22050 Hz, 16-bit, up to one second each. Keep them
short (moves under 20 ms, choices under 150 ms) and quieter than games (amplitude 0.1 to 0.3).

## Adding a theme

1. Pick the palette and add a row to `s_themes` in `ui.c`, and its name to `i18n.c`.
2. With a new look: add `LOOK_*` to `ui_look.h` and its functions to `ui_look.c` (strips, corners, title, desk,
   cursor, mote), and its case in each `look_*` entry point. Copy its palette into `tools/lookpreview.c`.
3. Iterate with `tools/lookpreview.c`: look at the three windows at 3x, then the opening strip. Zoom in on the
   corners and the title plate, pixel by pixel.
4. With a new voice: add `UI_VOICE_*` to `ui_priv.h` and its sounds to `ui_sound.c`.
5. Build for the FH1 board, run it in the native simulator and screenshot (`--tft-png`):
   - every state: the menus, a window in focus over one out of focus, a message, a question;
   - the Temas app, switching to the theme and away from it (the widgets must follow);
   - the opening, by stopping the run at times 100 ms apart;
   - the sounds, by recording `--wav` with `!sounds on` and checking each key press makes one.
6. Check Gen 1 still matches its reference screenshot pixel for pixel, and measure the cost (above).
