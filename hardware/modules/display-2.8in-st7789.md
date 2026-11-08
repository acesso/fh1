---
component: 2.8" TFT module, 240 × 320, ST7789, blue PCB, 9-pin SPI header plus 8080 parallel FPC sockets (16-pin and 8-pin), IM0–IM2 interface jumpers
image: display-2.8in-st7789-v2-board.png (front and back of the chosen module), display-2.8in-st7789-v2-drawing.png (its drawing).
role: the handheld's screen
status: dimensions read off the drawing, not measured yet
---

# 2.8" IPS display module (ST7789)

## Read from the image (2.8" drawing; portrait, header at the top, "12 o'clock" viewing)

- **Listing summary:** 240 RGB × 320, TFT IPS, SPI, ST7789, size 55 × 81 × 4.4 mm.
- **Width (short side):**
  - 54.74 mm: PCB.
  - 51.00 mm: backlight (BL).
  - 50.00 mm: unlabelled. Most likely the glass, which is the standard 2.8" outline.
  - 46.26 mm: unlabelled.
  - 43.20 mm: active area (A.A).
  - 40.00 mm: unlabelled.
- **Height (long side):**
  - 81.28 mm: PCB.
  - 76.56 mm: unlabelled.
  - 70.20 mm: backlight (BL).
  - 69.20 mm: unlabelled.
  - 65.15 mm: unlabelled.
  - 57.60 mm: active area (A.A).
- **Thickness:**
  - 4.40 mm in total.
  - 1.60 mm: PCB.
  - 2.90 mm: the panel and frame above it.
- **Back face:**
  - The width splits 15.29 + 24.00 + 15.45 mm: a 24 mm-wide feature, the FPC connector area, sits roughly centred.
  - Also printed: 50.70 mm wide, 51.56 mm tall, small offsets of 1.10, 1.65 and 5.68 mm.
- **Holes:** at the four corners of the PCB. Their diameter and centres aren't legible on the drawing.

## Read from the listing
- **Outline:** PCB 54.74 × 81.28 mm, 4.40 mm thick (1.50 mm PCB), active area 43.20 × 57.60 mm.
- **Header:** **9 pins**, pin 1 square, with its pitch printed as 2.54 × 8 = 20.32. In portrait from the front, left to right: **GND, VCC, SCL, SDA, RST, DC, CS, BL, SDA-O**. SDA-O is the SPI data-out (MISO), not needed for a write-only display.
- **Mounting holes:** **4 × R1.70**, which is Ø3.4 mm: room for M3 screws.
- **On the back:**
  - An **8080-16P** and an **8080-8P** FPC socket (parallel interface).
  - The panel's 40-pin FPC.
  - A small regulator (U8, SOT-23-5) and a transistor (U7). U7 is most likely the backlight switch driven by BL.
- **Interface jumpers IM0–IM2,** with a table:

| Jumpers (IM0, IM1, IM2) | Interface |
|---|---|
| 1, 0, 0 | P8 (8-bit parallel) |
| 0, 0, 1 | P16 (16-bit parallel) |
| 0, 1, 1 | SPI (4-wire, the one we use) |
| 1, 0, 1 | SPI3 (3-wire) |

  Check that it ships set to **SPI**.

## Inferred — needs your confirmation

1. **Glass and viewing area:** 50.00 × 69.20 is the glass, the standard 2.8" panel outline. 46.26 × 65.15 is probably the viewing area (the window in the frame).
2. **Active area position:** the active area sits about 3.5 mm below the top of the glass, leaving the thicker border at the bottom, where the FPC folds to the back. This is the usual 2.8" panel layout.
   - The drawing in `layout/board-render.html` uses this, with the glass centred in the backlight outline and the backlight centred on the PCB.
   - Measure it on the module.
3. **Hole centres** are about 2.5 mm in from each edge. The user's measurement on the board puts the top pair on the main board's top corner holes with the module placed as in `layout.md`.
4. **Header pinout:** GND, VCC, SCL, SDA, RST, DC, CS, BL, SDA-O (pin 1 = GND). Still to check: that BL is active-high and drives the on-board transistor (U7), so a GPIO can PWM it directly.

## Use in this project

- **Mounting:**
  - **Orientation:** landscape, header on the right as seen from the front. 81.28 mm across the 80 mm board: 0.64 mm over each edge.
  - **Position:** the top edge of the PCB is 0.92 mm below the board's top edge.
  - **Fixing:** the module's top holes line up with the board's top corner holes, so the same screws fix the module and the top spacers.
  - **Room gained:** it frees about 5 mm below the screen for the controls.
- **Interface:** SPI2 (MOSI, SCK, CS, DC) + RST on a GPIO, plus backlight PWM through a small N-MOSFET. See `architecture.md`.
- **Emulator scaling:** Game Boy Color at 1.5× is 240 × 216, which fits the 320 × 240 landscape panel with bars on the sides.
- **Build stages:** socketed during development, about a 12 mm stack. Soldered in the final build, about 5 mm.
- **Header on the board:** 9 pins on back column 0, rows 4–12 (front column 29). In landscape with the header on the right (front view), pin 1 (GND) is at the top, so SDA-O (unconnected) is at row 12.
