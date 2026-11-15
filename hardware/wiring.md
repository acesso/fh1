# Wiring

The pin map and the module-to-module connections. The routed wires are drawn in `layout/board-render.html` ("Main board · back · wiring"), on the perfboard's 30 × 42 hole grid.

## The hole grid

- **The perfboard is 30 × 42 holes**, the same type as the bridge's board. Columns run 0–29 and rows 0–41, seen from the back, with column 0 at the left.
- **The front view mirrors it:** back column *i* is front column 29 − *i*.
- **Every pinned part sits on these holes.** Wires run along columns and rows, or half a step between them (between two pins) where they have to pass a pin row.
- **Wires mostly run on the back face.** A few run on the front face, under the display (drawn dashed): the Menu wire and the power trunks.

## Where each module's pins sit (back view)

| Part | Columns | Rows | Notes |
|---|---|---|---|
| S3 mini board | 2–12 | 1, 2 (top) and 8, 9 (bottom) | Two columns in from the edge. Assumes its two sides are 20.32 mm (8 rows) apart: check it |
| Display header | 0 | 4–12 | 9 pins, through from the front (front column 29): GND, VCC, SCL, SDA, RST, DC, CS, BL, SDA-O (unconnected). The display overhangs the board's left edge (front view) by 2.15 mm |
| Codec header | 15 | 0–9 | Turned so GND is at the top: GND, VCC, 3V3, DO, WS, DI, BCK, MCK, SCL, SDA |
| micro-SD header | 22 | 1–9 | 3V, GND, CLK, D0, CMD, D3, D1, DAT2, DET |
| MCP23017 #1 | 12 (single), 18 (A), 19 (B) | 12–21 | |
| MCP23017 #2 | 21 (single), 27 (A), 28 (B) | 12–21 | |
| Stick header | 21–24 | 22 | Through from the front: VCC, Y, X, GND (left to right, seen from the back) |
| Board-to-board header | 29 | 14–23 | |
| IMU (QMI8658A), upright | second board, inner face | — | Below the RTC; shares its I2C, 3V3 and GND from the board-to-board header. INT to MCP #2 GPB2 |
| RTC (PCF8563) | second board, inner face | — | Beside the board-to-board header; I2C, 3V3, GND and INT (MCP #2 GPB3) through it |
| Buck-boost, charger | — | — | Wired by their end pads, not on the grid |

## S3 mini board pins on the back face

The mini board lies across the top-left corner, its USB-C towards the left edge (now about 5 mm in from it) and its antenna end towards the right. Position 0 is column 2 (the USB end), position 10 is column 12 (the antenna end).

| Row (board row) | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Top outer (1) | VIN | GND | 3V3 | 21 | 17 | 15 | 13 | 11 | 9 | 7 | 0 |
| Top inner (2) | VIN | GND | 3V3 | 26 ✗ | 18 | 16 | 14 | 12 | 10 | 8 | EN |
| Bottom inner (8) | 48 | 33 ✗ | 35 ✗ | 36 ✗ | 39 | 46 | 44 (RX) | 43 (TX) | 6 | 4 | 1 |
| Bottom outer (9) | 47 | 34 ✗ | 37 ✗ | 38 | 40 | 41 | 42 | 45 | 5 | 2 | 3 |

✗ = used by the in-package PSRAM (GPIO26, 33–37): leave unconnected. Rows 3–7 under the board are free holes, and they carry the I2S lanes.

## GPIO map

The map follows the geometry.
- **The display's SCK and MOSI are on SPI2's IO_MUX pins** (GPIO12, GPIO11), the only pins that carry SPI past 40 MHz (`architecture.md`, open question 22). They leave on the front face, under the display, and come back to the back face two holes from the display header.
- **Top inner row:** the other pins drop straight down into the free rows 3–7 and run across to the codec, which sits just past the antenna end. They carry I2S.
- **Bottom right pins:** they run straight across rows 8 and 9 to the codec's SCL and SDA.
- **Bottom left pins:** they reach the display header at column 0.
- **The rest:** micro-SD, the expanders and the analog inputs go down below the board.

| GPIO | Signal | To | Route |
|---|---|---|---|
| 12 | Display SCK (IO_MUX FSPICLK) | display SCL | Front face: down column 9 to row 6, left to hole (4, 6); back face: left along row 6 |
| 11 | Display MOSI (IO_MUX FSPID) | display SDA | Front face: down half-column 9.5 to row 7, left to hole (4, 7); back face: left along row 7 |
| 48 | Display RES | display RES | Straight across row 8 |
| 47 | Display DC | display DC | Straight across row 9 |
| 38 | Display CS | display CS | Down to row 10, left. Through the GPIO matrix: chip select is not the speed limit |
| 40 | Display backlight PWM | display BLK | Down to row 11, left |
| 8 | I2S data in (ADC/mic) | codec DO | Down to row 3, right |
| 10 | I2S WS | codec WS | Row 4 |
| 14 | I2S data out (DAC) | codec DI | Row 5 |
| 16 | I2S BCK | codec BCK | Row 6 |
| 18 | Codec MCK | codec MCK | Row 7 (may become unnecessary) |
| 1 | I2C SCL | codec SCL, then both MCPs | Straight across row 8; trunk down half-column 16.5 |
| 3 | I2C SDA | codec SDA, then both MCPs | Straight across row 9; trunk down half-column 15.5. Strapping pin, held high by the I2C pull-up: harmless with the default eFuse |
| 43 | SD CLK | SD CLK | Row 10 lane, up column 19 |
| 42 | SD D0 | SD D0 | Row 10.5 lane, up column 20 |
| 41 | SD CMD | SD CMD | Row 11 lane, up column 21 |
| 5 | Expander INT (shared) | MCP #1 INTA, MCP #2 INTA | RTC GPIO (deep-sleep wake); half-row 12.5 |
| 2 | Stick X | stick X | ADC1; down column 11 |
| 4 | Stick Y | stick Y | ADC1; down half-column 11.5 |
| 6 | Battery voltage | divider tap | ADC1 |
| 44 | Button-LED brightness PWM | LED brightness switch | |
| 0 | Menu button (BOOT) | Menu switch | On the front face, under the display |
| 7, 9, 21 | SPI3 SCK, MOSI, MISO | E28 radio (camera coprocessor later) | One-hole hops to the radio header; 4–8 MHz |
| 13 | E28 NSS | radio header | |
| 15 | E28 BUSY | radio header | |
| 17 | E28 DIO1 | radio header | RTC GPIO: a packet can wake the S3 |
| 39 | Spare | | Bottom inner row. A candidate for the camera's CS (open question 23) |
| 45, 46 | Spare (strapping) | | Never with a pull-up at boot: GPIO45 high selects 1.8 V flash/PSRAM, and GPIO46 high blocks download mode |

- **Crossings left:** the two I2C trunks cross the three SD lanes and the INT lane, and the stick and battery wires cross the SD and INT lanes. Insulated wire, no pin holes involved.
- **No crossings:** the display, I2S, and the SD lanes among themselves.

## MCP23017 pin allocation

Each expander serves the parts nearest to it.
- **MCP #1 (left, under the buttons)** takes the button inputs and the button-LED outputs.
- **MCP #2 (right)** takes the charger status, the amp enable and the second board's lines. The rows of the board-to-board header match MCP #2's GPB rows, so the RTC and IMU interrupts are one-hole hops, NRESET and TX_EN are short diagonals, and R comes straight down beside GPB7.
- GPA7 and GPB7 are output-only.

| Pin | MCP #1 (0x20) | MCP #2 (0x21) |
|---|---|---|
| GPA0 | A button | Amp enable (PA_EN), out |
| GPA1 | B button | Camera PWDN (later) |
| GPA2 | X button | BT transmitter (later) |
| GPA3 | Y button | E28 RX_EN, out |
| GPA4 | Start | spare |
| GPA5 | Select | Charger STDBY |
| GPA6 | SD card detect | Charger CHRG |
| GPA7 (out) | Stick power (VCC) | spare (output only) |
| GPB0 | A LED cathode, out | E28 NRESET, out |
| GPB1 | B LED cathode, out | E28 TX_EN, out |
| GPB2 | X LED cathode, out | IMU INT |
| GPB3 | Y LED cathode, out | RTC INT |
| GPB4 | spare | spare |
| GPB5 | spare | L button |
| GPB6 | spare | R button |
| GPB7 (out) | spare | spare (output only) |

- **Inputs:** the internal pull-ups are on, and the switches and open-drain status lines pull to GND.
- **LED outputs:** they sink the cathodes; low = on.
- **Lit switches:** each one's signal is on one corner leg and GND on the diagonally opposite leg. On a 4-leg tactile switch the diagonal pair is always across the switch, whichever way its internal pairs run.

## Board-to-board header (main board back column 29 = second board inner face column 0, rows 13–23)

The rows were chosen so that both ends route without crossings:
- **Main board:** the two interrupts hop straight onto MCP #2's GPB2/GPB3 (same rows), NRESET and TX_EN are diagonals from GPB0/GPB1, RX_EN comes from GPA3 between the expander's pin columns, R comes down from GPB6 beside GPB7, L takes a short hop on the front face from GPB5, and SDA/SCL arrive on the I2C lanes.
- **Second board:** the radio's three control lanes leave the top rows nested, the RTC lanes turn down in order, and L and R leave the bottom rows to their buttons beside the speaker. No crossings.

| Row | Signal | V1 |
|---|---|---|
| 13 | E28 NRESET (MCP #2 GPB0) | yes |
| 14 | E28 TX_EN (MCP #2 GPB1) | yes |
| 15 | E28 RX_EN (MCP #2 GPA3) | yes |
| 16 | IMU INT → MCP #2 GPB2 (one-hole hop) | yes |
| 17 | RTC INT → MCP #2 GPB3 (one-hole hop) | yes |
| 18 | I2C SDA | yes |
| 19 | I2C SCL | yes |
| 20 | GND | yes |
| 21 | 3V3 | yes |
| 22 | R button (MCP #2 GPB6) | yes |
| 23 | L button (MCP #2 GPB5) | yes |

The E28 radio's control lines use rows 13, 14 and 15; the buttons use 22 and 23. Its SPI, BUSY, DIO1 and power go through the separate radio header (below).

## Radio header (main board back row 0, columns 3–11 = second board inner face row 0, columns 26–18)

| Main column | S3 pin below it (row 1) | Signal |
|---|---|---|
| 3 | GND | GND |
| 4 | 3V3 | 3V3 (radio supply) |
| 5 | GPIO21 | MISO |
| 6 | GPIO17 | E28 DIO1 |
| 7 | GPIO15 | E28 BUSY |
| 8 | GPIO13 | E28 NSS |
| 9 | GPIO11 | spare: GPIO11 is the display's MOSI, and leaves on the front face |
| 10 | GPIO9 | MOSI |
| 11 | GPIO7 | SCK |

Every main-board connection is a one-hole hop from the S3's top outer row: no lanes, no crossings. The header sits just outside the S3's top edge. The camera's CS (later) needs a pin of its own (open question 23).

## Second board wiring (V1)

The V1 parts: the RTC, the IMU, the E28 radio and the L and R buttons. The speaker plugs into the codec's SPK socket with its own lead. The battery holder's leads run to the main board's reverse FET and charger. The camera is not wired.

- **RTC:**
  - It stands with its 6-pin header at the bottom, on row 22, columns 2–7, in the order VCC, GND, SDA, SCL, COT, INT. COT is unused.
  - Its lanes leave the header on rows 17–21 and turn down to the pins in order, so they don't cross.
  - The one exception: SDA and SCL cross once, because the RTC's pin order is SDA, SCL while the header rows are SDA, SCL from the other side.
- **IMU:**
  - Upright, its 8 pins on row 23, columns 2–9: VCC, GND, SCL, SDA, XDA, XCL, AD0, INT.
  - VCC and GND are one-hole hops from the RTC's pins just above. SDA and SCL are two short crossed jumpers.
  - AD0 goes to GND (address 0x6A).
  - Its interrupt lane comes from header row 16 down half-column 9.5.
- **E28 radio:**
  - It sits at the top right under its header, glued flat, antenna end to the right.
  - The header's 8 used pins go straight down to the module's top-edge pads as short leads.
  - **The three control lanes** come from the top rows of the board-to-board header, nested, without crossing anything:
    - NRESET: row 13, then up column 20.
    - TX_EN: row 14, then up column 21.
    - RX_EN: row 15, then up column 22.
  - The 470 µF sits beside it. The SMA pigtail runs from its IPEX socket to the corner.
- **L and R buttons** (`modules/button-pb86.md`): two PB86 on the **outer face**, one on each side of the speaker, their tops level with the speaker's top, pressed from behind. Their pins come through to the inner face:
  - **L:** columns 2–5, **R:** columns 24–27, rows 26–30. Seen from the inner face: pin 2 (unconnected) and pin 3 (normally open) on row 26, the two pins 1 (common) on row 28, the LED pins 4–7 on row 30 (left floating).
  - **Signals:** R from header row 22, down half-column 0.5 to row 24, along row 24 above the speaker, then down column 27 to its pin 3 (27, 26). L from header row 23, down column 0 to row 25, then along row 25 to its pin 3 (5, 26).
  - **GND:** L's from header row 20, down the edge margin (half-column −0.5) to row 28, then across to its pin 1 (2, 28). R's from the IMU AD0 lane, along half-row 23.5 to column 28, then down to its pin 1 (27, 28).
  - No crossings.
- **Camera (later):** moved down slightly (rows 1–8) to free the top edge for the radio header.
- **Speaker:** centred across the board (22–58 mm), 2.5 mm down, clear of the R lane on row 24; L and R on either side of it.
- **Wi-Fi antenna:** its U.FL cable runs from the S3's IPEX socket (main board back) through the gap to the flex antenna on the second board's outer face. It's a cable, not a board wire.

## Power

| Rail | From | To |
|---|---|---|
| USB 5 V | the charger board's USB-C (charging port), and the S3 board's VIN through an 1N5819 | charger IN+, the power path's USB input |
| VSYS | power path (after the off-switch P-FET) | buck-boost VIN, codec VCC (amp), the LED brightness switch |
| 3V3 | buck-boost VOUT | S3 mini 3V3, display VCC, codec 3V3, SD 3V, both MCP VCC, stick VCC (through MCP #1 GPA7) |
| GND | one star at the buck-boost's GND pads | everything |

## Wire

The two 28 AWG boxes give nine colours: white, red, grey, orange, purple, black, yellow, blue, green.

| Use | Wire | Colour |
|---|---|---|
| Display SPI | 28 AWG | blue |
| I2S and MCK | 28 AWG | purple |
| I2C | 28 AWG | yellow |
| micro-SD | 28 AWG | green |
| Expander INT, Menu | 28 AWG | orange |
| Analog (stick, battery) | 28 AWG | grey |
| LED PWM, button LEDs | 28 AWG | red (thin, so it can't be mistaken for the 22 AWG power red) |
| Button lines to the expanders | 28 AWG | white |
| Signal grounds that need a thin wire | 28 AWG | black |
| 3V3 | 22 AWG solid (in stock) | white |
| VSYS, battery + | 22 AWG solid (in stock) | red |
| GND | 22 AWG solid (in stock) | black |

## Routing rules

- **Face:** wires run on the main board's back face, under the modules, on the hole grid or half a step between holes.
- **Crossings:** insulated wire may cross insulated wire. A wire must not pass through another part's hole or pad.
- **Lanes:** parallel wires keep at least half a step (1.27 mm) apart. The 28 AWG wire is about 0.9 mm across.
- **Grouping:** each bus leaves the S3 as a ribbon of parallel wires: the display to the left, I2S and I2C to the right, SD in three lanes below.

### The front face (the display side)

Kept to short paths and power trunks that would otherwise cross a whole bus on the back. All run under the display or in the edge margins:

| Wire | Path |
|---|---|
| Menu (GPIO0) | down half-column 12.5 to the Menu switch |
| Display SCK (GPIO12) | down column 9 to row 6, left to hole (4, 6), then through to the back face. Crosses the 3V3 lane on half-row 2.5 |
| L button (MCP #2 GPB5) | from GPB5 down half-column 27.5 to half-row 22.5, then to board-to-board row 23. Crosses the 3V3 lane to the header once |
| Display MOSI (GPIO11) | down half-column 9.5 (between GPIO12 and GPIO10) to row 7, left to hole (4, 7), then through to the back face. Crosses the 3V3 lane on half-row 2.5 |
| GND trunk | from the buck-boost GND pad along half-row 0.5 to the S3, display, codec, SD, both expanders and the board-to-board header |
| VSYS → codec VCC | half-row 1.5 |
| 3V3 | half-row 2.5 to the codec and SD, then down column 17 to the expanders and the board-to-board header; one hop to the display VCC |
| S3 VIN → 1N5819 → charger IN+ | the top margin and the edge margin |
| Off switch → Q1 gate | the edge margin and the bottom row |

The front-face lanes cross each other only where a trunk has to pass another (3V3 × Menu, VSYS × GND and 3V3) and where the display's SCK and MOSI, and the L button's hop, cross a 3V3 lane.

### Modules soldered without sockets

The S3 and the display can be soldered straight onto pin headers instead of sockets. The header's plastic strip leaves a gap of about 2.5 mm between the module and the perfboard.
- **Under the S3 (back face):** the display, I2S and I2C lanes run with **no crossings**, so single wires fit the gap. SCK and MOSI run on the front face there.
- **Under the display (front face):** a few crossings. Two 28 AWG wires stacked are about 1.8 mm, still inside the gap.

### The S3's USB-C and the display's header pins

- **The problem:** a USB-C plug in the S3's socket lies over the display header's pins. The display's pins come through the main board on the back at column 0, rows 4–12, and the plug's moulding covers about rows 3–7.
- **With the S3 on a 2.54 mm header** (2.5 mm plastic), the socket's centre sits about 5.7 mm above the board, and a typical plug moulding (about 6.5 mm thick) bottoms out about 2.4 mm above it.
  - **Trim the display's pins flush on the back,** to about 1 mm with the solder, and it clears.
  - **Untrimmed pins (2–3 mm) would hit the plug.**
- **With the S3 on a socket** (about 8 mm), there is plenty of room either way.
- **The charging port** (the charger board's USB-C) is lower down and clear of all this.

## Routed

- **Buttons:** the four lit buttons, Start and Select to MCP #1 GPA0–5; their GND legs to a bus along row 35. L and R to MCP #2 GPB5 and GPB6 through the board-to-board header.
- **Button LEDs:** a 270 Ω resistor at each anode, fed from the LED brightness switch; the cathodes to MCP #1 GPB0–3.
- **The stick's power:** from MCP #1 GPA7.
- **MCP #2:** charger CHRG/STDBY, amp enable, and the board-to-board header (I2C, 3V3, GND, the RTC and IMU interrupts, the radio's control lines, L and R).
- **The power stage, simplified:** cell to the reverse FET to the charger; charger OUT to the power path; S3 VIN through the 1N5819 to the charger IN+ (front face); the off switch to its FET (front face); VSYS to the buck-boost, the codec amp and the LED switch; the battery divider.
- **Ground:** a front-face trunk from the buck-boost GND pad to every module, plus the button bus on the back.
- **Bulk capacitors:** 470 µF at the buck-boost input, 100 µF at the S3's 3V3.

## To route

- **The power stage in detail:** the parts inside the path/off and reverse-FET areas, once the AO3401 adapters are in hand.
- **Second board:** the RTC and IMU wiring on its inner face, the speaker leads, and later the radio and camera.
- **Check against the real parts:** the S3's row spacing, the lit switches' LED pins, the codec's PA_EN pad position, the charger's LED pads.
