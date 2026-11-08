---
component: ESP32-S3 mini board ("ESP32-S3_MINI" on the silkscreen), chip-down ESP32-S3R8, 22.4 × 32 mm, USB-C, ceramic antenna + IPEX
image: esp32-s3-mini-board.png (front, with dimensions), esp32-s3-mini-pinout.png (seller's interface definition), esp32-s3-mini-front-back.png (both faces)
role: the handheld's MCU
status: read off the seller's images
---

# ESP32-S3 mini board

## Read from the images
- **Size:** 22.4 × 32 mm.
- **Chip:** the ESP32-S3 die itself, no module can. The marking reads `ESP32-S3 / 152026 / R8...`, so it is an **ESP32-S3R8**, with **8 MB octal PSRAM** inside the chip package.
- **Flash:** a separate 8-pin chip below the S3: **16 MB** (the **N16R8** variant).
- **Crystal:** 40 MHz, beside the chip.
- **Antenna:** a ceramic chip antenna along the top edge, plus an **IPEX (U.FL)** socket in the top-right corner. Small 0 Ω positions (R2/R3, drawn diagonally) sit between them, so presumably the antenna is selected by moving a resistor.
- **USB-C:** on the bottom edge, wired to the S3's native USB (GPIO19/20 are not on the headers).
- **On the board:**
  - RST and BOOT buttons.
  - An RGB LED (WS2812-type, the white square).
  - A small regulator (5-pin, marked `CGCaB`).
  - A small LED by the BOOT button.
- **Headers:** two columns of 11 holes on each long side, 2.54 mm pitch, 44 holes in all, numbered from the top:

| Left outer | Left inner | | Right inner | Right outer |
|---|---|---|---|---|
| IO0 | EN | | IO1 | IO3 |
| IO7 | IO8 | | IO4 | IO2 |
| IO9 | IO10 | | IO6 | IO5 |
| IO11 | IO12 | | TX (43) | IO45 |
| IO13 | IO14 | | RX (44) | IO42 |
| IO15 | IO16 | | IO46 | IO41 |
| IO17 | IO18 | | IO39 | IO40 |
| IO21 | IO26 | | IO36 | IO38 |
| 3V3 | 3V3 | | IO35 | IO37 |
| GND | GND | | IO33 | IO34 |
| VIN | VIN | | IO48 | IO47 |

Note: the back silkscreen mirrors this, so the two columns swap sides when the board is turned over.

## Inferred — needs your confirmation
1. **Flash size:** settled. The user's board is the N16R8 (16 MB flash, 8 MB octal PSRAM). Read the flash chip's marking on arrival to be sure.
2. **PSRAM pins are taken.** With octal PSRAM (the R8), **GPIO33–37 and GPIO26** are used inside the package. They appear on the headers but must stay unconnected.
3. **The RGB LED is probably on GPIO48,** as on most boards of this type. It draws about 1 mA even when dark: remove it for battery life, or leave GPIO48 unused.
4. **VIN** is most likely VBUS from the USB-C through a Schottky diode, feeding the on-board regulator. Check it with a meter: whether VIN is before or after the diode, and the diode's current rating, decide whether the charger can draw its 1 A through it.
5. **CC resistors:** whether the USB-C has 5.1 kΩ on CC1/CC2 shows when a C-to-C cable powers it.
6. **Antenna selector:** which 0 Ω position routes the RF to the IPEX socket.
7. **No RF shield.** A chip-down board has no metal can over the RF section. For a hobby device this is fine. The can mainly helps certification and noise from nearby circuits.
   - **Keep fast, noisy lines from crossing right over the chip:** the display SPI clock and the codec's MCLK.
   - **Ground plane:** the second board doesn't sit over it, so nothing more to do.

## Use in this project
- **On the board already:** USB-C with the data lines, the RST and BOOT buttons and the EN circuit.
- **Placement:** on the main board's back, on sockets, lying horizontally in the top-left corner (seen from the back), its pins on columns 2–12 (two columns in from the edge; see `wiring.md`).
  - The USB-C sits at the side edge.
  - The IPEX end points into the board; a U.FL cable runs to the Wi-Fi flex antenna on the second board.
  - Move the 0 Ω to the IPEX side.
- **Power:**
  - **3V3:** the buck-boost feeds the board's 3V3 pins directly.
  - **USB in:** with USB plugged in, the on-board regulator also makes 3.3 V from VIN, so two regulators share the rail. That usually works, but measure it. If it misbehaves, lift the regulator's output pin, so the buck-boost (fed from USB through the power path) is the only source.
- **Charging:** VIN (USB 5 V) goes to the TP4056's input pads. The TP4056's own USB-C stays unused.
- **BOOT:** the Menu button on the front is wired to IO0, in parallel with the board's BOOT button.
- **Standby:** remove the RGB LED and the small LED. Then the board adds only the regulator's leakage (no USB-UART chip, no AMS1117).
- **Usable GPIOs:**
  - **Clean (25):** 1–18, 21, 38–42, 47.
  - **Also usable:** 43/44 (the console runs over native USB), 48 once its LED is gone.
  - **Strapping:** 0, 3, 45, 46.
  - **Not usable:** 26 and 33–37.
  - That is the same budget as before, so the plan in `architecture.md` still fits.
