# Parts

The parts for **two devices**. Module notes (dimensions, pinouts, checks) are in `modules/`.

## Modules

| Block | Part | Qty | Notes and checks on arrival |
|---|---|---|---|
| MCU | **ESP32-S3 mini board, N16R8** (22.4 × 32 mm, chip-down ESP32-S3R8, 8 MB octal PSRAM, 16 MB flash, ceramic antenna + IPEX, USB-C with data, RST/BOOT, regulator). `modules/esp32-s3-mini.md` | 2 | Remove the RGB LED and the small LED for standby. Move the 0 Ω to the IPEX side |
| Wi-Fi/BT antenna | Compact 2.4 GHz flex PCB antenna with a U.FL (IPEX) lead, ~26 × 8 mm, 10–15 cm cable | 2 | Top right of the second board's outer face; plastic back cover over it; keep-out on both faces |
| Display | **2.8" TFT ST7789V, 240 × 320, SPI, 54.74 × 81.28 × 4.4 mm**, 9-pin header (GND, VCC, SCL, SDA, RST, DC, CS, BL, SDA-O), 4 × Ø3.4 mounting holes, IM jumpers. `modules/display-2.8in-st7789.md` | 2 | Check the IM jumpers are set to SPI and that BL drives the on-board transistor. Socketed during development, soldered in the final build |
| Thumbstick | **PSP-style analog stick module** (Deek-Robot JOYSTICK-02 class, 25.8 × 25.8 mm, two 10 kΩ pots, GND, X, Y, VCC). `modules/joystick-psp-deek-robot.md` | 3 | Swap the right-angle header for straight pins. Mount with M2 screws. Measure its height |
| Buttons, A/B/X/Y | **TS-G010 12 × 12 mm illuminated tactile switches**, red (A), yellow (B), blue (X), green (Y). `modules/button-12mm-illuminated.md` | 3 of each colour | **Check one LED's wiring before wiring four.** The tab pins are 9 mm apart: bend onto the diagonal holes |
| Buttons, Start/Select/Menu | **7.8 × 7.8 mm tactile switch, 2-pin**. `modules/button-8mm-tact.md` | 20 | 2 legs 5.1 mm apart: two perfboard holes |
| GPIO expanders | **MCP23017 breakout module, 26 × 21 mm**. `modules/mcp23017-module.md` | 5 | GPA7/GPB7 are output-only: use them for enables. Second module's A0 to VCC for 0x21. Count the I2C pull-ups in parallel |
| Audio | **ES8311 + NS4150B module, 26.1 × 17 mm**, with its external electret mic on a lead. `modules/audio-es8311-ns4150b.md` | 2 | PA_EN isn't on the header: lift R8 and wire PA_EN to the second expander. VCC from VSYS. Confirm DI/DO direction |
| Speaker | **36 × 25 × 6.5 mm enclosed speaker, 8 Ω 2 W**, 1.25 mm plug. `modules/speaker-36x25.md` | 5 | Check its plug mates with the codec module's speaker socket |
| micro-SD | **SPI/SDIO breakout, Adafruit 4682 layout**, 9 pins (3V, GND, CLK, D0, CMD, D3, D1, DAT2, DET). `modules/micro-sd-sdio-board.md` | 3 | 1-bit SDMMC. Check the pull-ups and which way DET switches |
| LoRa radio | **EBYTE E28-2G4M27S** (SX1281, 27 dBm, SPI, IPEX) + **TX2400-JKS-IPX20** antenna (5 dBi, IPEX pigtail, SMA). `modules/radio-e28-2g4m27s.md` | 2 | Castellated 1.27 mm pads, hand-wired to a header strip. 3V3 (max 3.6 V), 580 mA TX bursts, 470 µF at the module. Cap TX power to the Anatel limit |
| RTC | **PCF8563 module, 43 × 17 mm, with coin-cell holder**. `modules/rtc-pcf8563-module.md` | 5 | Fit a coin cell (probably CR1220). Second board's inner face |
| Motion sensor | **QMI8658A 6-axis module, single 8-pin row**, mounted upright. `modules/imu-qmi8658-module.md` | 2 | Remove the module's power LED |
| Charger | **Mini Li-ion charger + protection board, 20 × 15 × 5 mm, USB-C** (TP4056-type + protector; IN, B and OUT pads). `modules/charger-tp4056-mini.md` | 5 | Its USB-C is the device's charging port. CHRG/STDBY from the LED cathodes to the expander. Read RPROG |
| 3.3 V converter | **TPS63802 buck-boost module, 25.8 × 12.8 mm** (3V3/4V2/5V jumpers, PS pad). `modules/buck-boost-tps63802.md` | 3 | **Move the jumper from 5V to 3V3 and measure 3.30 V before connecting anything.** PS must select power-save |
| Battery holder | **Single 18650 holder, pre-wired, 75 × 20 × 18 mm**. `modules/battery-holder-18650.md` | 5 | On its side between the boards; glue a side wall to the second board. Shorten the leads |
| Battery | 18650 Li-ion cells (a 4-cell kit with charger) | 4 | Check the real capacity with a meter-timed discharge; plan battery life on that, not the label |

## Power and small parts

| Part | Use | Qty |
|---|---|---|
| AO3401 P-MOSFET (SOT-23) + SOT-23 to SIP3 adapters | Reverse polarity, power path, off switch | 50 + 50 |
| 1N5819 Schottky | Power path; S3 VIN to the charger input | 100 |
| SS12D00 slide switch | Off switch (drives the off FET's gate, no load current) | 40 |
| BS170 (TO-92) | Display backlight switch; gate driver for the button-LED brightness FET | 2 (stock) |
| Polymer capacitor 10 V 470 µF, 6 × 8 mm | VSYS bulk; one more at the E28's VCC | 1 pack |
| Polymer capacitor 16 V 100 µF, 5 × 7 mm | 3V3 bulk | 1 pack |
| 1/4 W metal-film resistors (stock) | 220 kΩ ×4 (battery divider), 270 Ω ×8 (button LEDs), 100 kΩ (FET gates), 10 kΩ | assorted |
| 100 µH chokes, ceramic 104 (stock) | Supply decoupling | — |
| LP5907 (SOT-23-5, stock) | Optional quiet 3.3 V for the mic, if the buck-boost ripple shows in recordings | — |

## Board, wire and mechanics

| Part | Use | Qty |
|---|---|---|
| Double-sided perfboard, 8 × 12 cm, 1.2 mm | Main and second board | 10 |
| 2.54 mm round-pin headers, male and female (stock) | Every module, round into round only; about 170 positions per device | — |
| 28 AWG stranded wire, 10 colours, 10 m each | Signals; colours mapped in `wiring.md` | 2 boxes |
| 22 AWG solid wire, red, black and white (stock) | Battery, charger, power path, VSYS and the 3V3/GND trunks | — |
| M3 hex standoffs, male-female (stock) | 4 × 20 mm + 4 × 5 mm for the 25 mm gap, plus 5/10/15 mm for tuning; nylon for the top (antenna) corners | — |
| Dupont jumper kits, 15 cm and 10 cm | Bench use | 2 |

## Still to get

- Genuine microSD cards (2 × 32 GB, A1, SanDisk, Samsung or Kingston).
- The RTC coin cells (check the holder size; probably CR1220).
