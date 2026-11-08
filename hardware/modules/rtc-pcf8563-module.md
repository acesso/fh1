---
component: PCF8563 RTC module, 43 × 17 mm, coin-cell holder, 6-pin header (INT, COT, SCL, SDA, GND, VCC)
image: rtc-pcf8563-module.png (seller's photo with dimensions)
role: real-time clock for the pet and scheduled wake
status: read off the seller's photo
---

# PCF8563 RTC module

## Read from the image
- **Size:** 43 × 17 mm, two mounting holes at the right end.
- **Chip:** a **PCF8563** (SOIC-8, marked `PCF8563 XSL2130`) with a 32.768 kHz crystal (Y1, cylinder).
- **Backup:** a **coin-cell holder** (B1, small: probably CR1220, check), with two diodes (D7, D8) that switch between VCC and the coin cell.
- **Parts:** C10, C16, C22, and a larger leaded part beside the VCC/GND pins: a buffer capacitor, or a small supercapacitor. Read its marking on arrival.
- **Header:** 6 pins: **INT, COT** (clock out), **SCL, SDA, GND, VCC**.

## Inferred — needs your confirmation
1. **Coin-cell size:** CR1220 by the look of the holder. Fit a cell: it keeps time while the off switch has the 3.3 V rail down.
2. **I2C pull-ups:** check whether the board has its own. With the codec, two expanders and the IMU, the bus should end up at about 2.2–4.7 kΩ total.
3. **INT:** open-drain on the PCF8563, so the expander's internal pull-up serves it.

## Use in this project
- **VCC** from 3V3. The coin cell takes over when the device is off.
- **I2C** at address **0x51**: no clash with the codec (0x18), the expanders (0x20/0x21) or the IMU (0x68/0x69).
- **INT** (alarm and timer) goes to MCP #2 GPB3, through board-to-board row 17. It wakes the S3 through the shared expander interrupt, for pet events while the device sleeps.
- **COT** is not used. Disable the clock output in firmware, to save current.
- **Placement:** on the second board's inner face, upright beside the board-to-board header, which carries its I2C, 3V3, GND and INT.
