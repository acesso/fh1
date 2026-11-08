# Mechanical layout

Drawing: `layout/board-render.html` (both faces of both boards, the side section, and the parts as placed).

Both boards are 120 × 80 mm. The device is held **upright, like a Game Boy**: 80 mm wide, 120 mm tall. The landscape screen sits on top and the controls below.

Why upright: the chosen 2.8" module is 81.28 × 54.74 mm (`modules/display-2.8in-st7789.md`). Held sideways (120 mm wide), it would leave under 20 mm on each side for the stick and the buttons, which is too tight. Upright, it spans the 80 mm width (0.64 mm over each side) and leaves about 64 mm below it for the controls.

## Main board, front face (towards the user)

```
 ┌──────────────── 80 mm ────────────────┐
 │┌─────────────────────────────────────┐│  ← the module's top holes on the board's top corner holes
 ││                                     ││
 ││   2.8" IPS 320×240, landscape        ││  81.28 × 54.74 mm module, active area 57.6 × 43.2 mm
 ││   (0.64 mm over each board edge)     ││
 ││                                     ▪││  ▪ = 8-pin header, right short edge
 │└─────────────────────────────────────┘├┐ ← right edge: USB-C (breakout on the back)
 │                                       │ 120 mm
 │   ┌───────┐  [menu]       (X)       ⇆│  off switch on the edge
 │   │ stick │          (Y)       (A)   │  stick on the left; SNES diamond of 12 mm illuminated switches
 │   │  ◯    │               (B)        │  (X blue, Y green, A red, B yellow), ±14 mm apart
 │   └───────┘                           │
 │   (Sel) (Start)                       │  white illuminated, below the stick
 │                                       │  Menu = BOOT: small, plain, between the stick and X
 │ ┄┄┄┄┄ 18650 behind, between the boards ┄┄┄┄ │
 └──────────────[ cell in ]──────────────┘  ← bottom edge: the battery opening
```

## The sandwich, seen from the side

```
  front ◄─────────────────────────────────────────────── back
  display (4.4 mm, on sockets ≈ 12 mm)
  ═══════════ main board ════════════════════════════
  top half:  S3 mini board · codec · SD · expander · power modules   (≈ 12–14 mm tall)
  bottom band: 18650 holder on its side, open face to the bottom edge  ← between the boards
  ═══════════ second board ══════════════════════════
  inner face (hanging into the gap): speaker · LoRa radio (later) · camera coprocessor (later)
  outer face: Wi-Fi antenna · LoRa antenna on an SMA bulkhead, top-left edge (later) · camera window · speaker holes · L/R on the top edge (later)
```

**The battery goes at the bottom, between the two boards, loaded from the device's bottom edge**.

- **Why:** the cell is the heaviest part. Low and centred, it balances the device in the hands.
- **Spacers:** M3 spacers, 20 + 5 = **25 mm**; available in 5 mm steps, so the gap can be tuned during the build.
- **Holder:** a regular plastic 18650 holder with spring contacts, turned on its side.
  - It lies across the 80 mm width, in the bottom band, with its **open face toward the bottom edge**.
  - The cell is pushed in from the bottom of the device and snaps between the contacts. The case's bottom edge gets an opening or a small door.
  - The holder's base faces inwards, towards the top of the device. It is glued or screwed to a small bracket or to the boards, and its PC pins are wired, not soldered into a board.
- **Fit:**
  - Holders are about 77 mm long, which fits the 80 mm width; use the case wall for the last mm.
  - The chosen holder is 75 × 20 × 18 mm (`modules/battery-holder-18650.md`). On its side, its **20 mm width** sits between the boards (5 mm to spare in the 25 mm gap), and its **18 mm height** is the battery band at the bottom. Glue a side wall to the second board.
  - A cradle holder of this kind is sized for an **unprotected cell** (65 mm). The charger module's DW01 + 8205 already protects the cell, so that's fine.
  - The cell now takes only the bottom band of about 18 mm. Most of the control area, the stick and the ABXY diamond are clear. Only the Start/Select leads, if they sit that low, need trimming and Kapton.
- **Reverse polarity:** a cell can still go in backwards, so a **reverse-polarity P-MOSFET** sits right after the holder, before the charger and the power path. Mark the polarity at the opening.
- **Shared gap:** everything on the main board's back face (modules on sockets, 12–14 mm) and everything hanging from the second board's inner face shares the 25 mm. Where they overlap in plan, their heights must add up to less than that, minus a couple of mm. The radio is only 2.85 mm thick, so nothing here is tight any more.
- **Spacers vs the holder (found while drawing `layout/board-render.html`):** the 77 mm holder spans the bottom band, where the two bottom-corner spacers would go. The bottom pair moves up to just above the band (about 94 mm from the top, two M3 holes drilled through both boards), and the holder, glued to both boards, acts as the spacer along the bottom edge.
- **Spacers and antennas:** the two **top** spacers are **nylon**: each sits in an antenna corner. The bottom two are metal; grounding them to both boards' GND ties the boards together electrically.
- **Bring-up:** the main board still runs on bench power or USB without the second board, so phase-1 bring-up works with it off.
- **Battery wires:** thick (22–24 AWG) and short, through the board-to-board header on 2 pins each for + and −, or as a separate 2-pin JST.

## Main board, back face (seen from the back)

```
 ┌──────────────── 80 mm ────────────────┐
 │ ┌───────────┐┌──────────────┐┌───────┐│
◄┼─┤ S3 mini ◦ ││ES8311+NS4150B││micro-SD├┤► SD slot at the edge
 │▪└───────────┘└──────────────┘└───────┘│  USB-C at the side edge; ◦ = IPEX
 │▪            ┌─────────┐ ┌──────────┐  │  ▪ display header leads (left, seen from the back)
 │▪  (free)    │MCP23017 │ │MCP23017 #2│ │
 │             └─────────┘ └──────────┘ ║│  ║ board-to-board header
 │   [buck-boost] [path][rev]           ║│
 │                    ┌──────────┐      ║│
 │  (button and stick │  TP4056  │       │
 │   leads, trimmed)  └──────────┘       │
 │ ┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄ │
 │ ⊂ 18650 holder on its side, 77 × ~21 ⊃│  bottom band: the cell lies across the width,
 │   open face ↓ toward the bottom edge  │  between the boards
 └──────────────[ cell in ]──────────────┘  ← loaded from the device's bottom edge
```

- **USB-C clearance:** the plug in the S3's socket lies over the display header's pins (column 0 on the back). Trim those pins flush on the back. With the S3 on a 2.54 mm header the plug then clears by about 1.4 mm; on a socket, by much more. See `wiring.md`.
- **The S3 is an ESP32-S3 mini board** (`modules/esp32-s3-mini.md`): a chip-down ESP32-S3R8 on a 22.4 × 32 mm board.
  - **Placement:** on sockets, lying across the top-left corner (seen from the back), its **USB-C at the side edge**. Flashing and console; it also charges, through a Schottky into the charger. The **charging port** is the charger board's own USB-C, at the same side edge lower down, above the battery band.
  - **Antenna:** the board has a ceramic antenna and an **IPEX** socket. Its ceramic antenna would sit right behind the display's ground plane, so the 0 Ω is moved to the IPEX side. A U.FL cable runs to the **flex antenna on the second board's outer face**, top right (seen from the back).
  - **RST and BOOT** are on the board, near the USB-C. The Menu button on the front is wired to IO0 in parallel with BOOT.
  - **No RF shield:** fine for this build. Keep the display's SPI clock and the codec's MCLK from running right over the chip.

## Second board (sandwich)

- **Placement:** 25 mm behind the main board on the corner spacers, sharing one board-to-board header. The header carries VSYS, 3V3, GND, I2C, the RTC and IMU lines; a second header will carry the radio's SPI and control lines, and spare lines.
- **Inner face** (hanging into the gap):
  - **The battery holder**, glued along the bottom band (phase 1).
  - **The speaker** (36 × 25 × 6.5 mm, 8 Ω 2 W, `modules/speaker-36x25.md`), its adhesive gasket stuck to the board, firing out through a field of drilled holes to a grille in the back cover (phase 1).
  - **The E28 2.4 GHz LoRa radio** (V1), top right under its own radio header, glued flat with short leads from its castellated pads. It is 2.85 mm thick, so no height concern.
  - **The camera coprocessor** (later, XIAO ESP32S3 Sense), at the top centre, its lens through a hole in the board and the back cover.
- **Outer face** (the device's back; positions seen from the back):
  - **Wi-Fi/BT flex antenna, top right** (~26 × 8 mm, U.FL cable from the S3), phase 1.
  - **LoRa antenna, top left** (V1): the JKS rubber duck on an SMA bulkhead through the case edge, as far from the Wi-Fi antenna as possible. Same band as Wi-Fi: the firmware never transmits on both at once. Its pigtail runs from the radio's IPEX socket, and about 45 mm separates them across the camera window.
  - The camera window and the speaker holes.
  - L and R shoulder buttons on the top edge (later).
- **Keep-outs:** each antenna has a keep-out on **both faces** of this board: the inner face is only 1.6 mm away. Nothing metal, no modules, no wires. The back cover over the antennas must be plastic.
- **Why the top corners, not the centre:**
  - An antenna in the middle of the outer face would need the same keep-out on the inner face, right where the radio and the speaker now hang. It would also face the main board's modules across the gap.
  - The top corners have nothing behind them but the board edge and the plastic case, and hands rarely cover them.

## Development vs final build

- **Development:** the display and the S3 mini board sit on sockets, so they can be swapped.
- **Final build:** the display is soldered directly; the S3 mini board can stay socketed or be soldered.

## Thickness

| Layer | Thickness |
|---|---|
| Display on sockets (soldered: about 7 mm less) | ~12 mm |
| Two boards | 3.2 mm |
| Board gap (spacers) | 25 mm |
| Antennas and labels on the outer face | ~1 mm |
| **Whole device** | **~42 mm socketed, ~35 mm soldered** |

A Game Boy DMG is 32 mm.

## To settle next

1. The real module sizes, once the listings are chosen (display, charger, buck-boost, SD).
2. The battery holder actually bought: how it is anchored, and the bottom opening or door in the case. And whether the gap can come down from 25 mm.
3. The case: 3D print, or two acrylic plates with standoffs.
