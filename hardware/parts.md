# Parts

Two lists: what can come from the bridge project's stock (`../Signal-Decoder/hardware/bridge-s3/current-hardware-setup.md`), and what to buy. Prices and exact listings still need to be picked; candidates are the starting point for that dig.

## Reused from stock

| Part | Use here | Note |
|---|---|---|
| Older ESP32 (classic) WROOM dev board | Bench fixture only: try the display, SD, buttons, expander and audio modules while the S3 ships | No PSRAM and a different pin map, so it can't be the main MCU. Retro-Go needs PSRAM |
| 8 × 12 cm double-sided perfboard (×2) | Main and second board | — |
| 2.54 mm round-pin headers, male and female | Every module | Same rule as the bridge: round into round only. Sockets add 7–9 mm of height |
| BS170 (TO-92) ×2 | Display backlight switch; gate driver for the button-LED brightness FET | An AO3400 (SOT-23) is smaller if bought |
| 1N4148 | Spare | — |
| 100 µH chokes, ceramic 104 | Supply decoupling | — |
| 1/4 W metal-film resistors | Pull-ups, dividers, load-sharing gate | — |
| LP5907 (SOT-23-5) | Optional quiet 3.3 V for the mic, if the buck-boost ripple shows in recordings | — |

Not reused:
- **Nokia 5110 LCD:** monochrome, 84 × 48.
- **AMS1117:** its dropout is too high for one Li-ion cell.
- **LM7805:** wrong for the job.

## In the cart (2026-10-31, full: `cart-2026-10-31.png`), for **two devices**

| Item (listing, variant) | Qty | Needed for 2 | Verdict |
|---|---|---|---|
| Polymer solid capacitor (**16V 100uF 5x7mm**, Lechee) | 1 pack | 2 | OK: polymer = low ESR. 3V3 bulk |
| Polymer solid capacitor (**10V 470uF 6x8mm**, Lechee) | 1 pack | 2 | OK: VSYS bulk, 10 V margin |
| SS12D00 slide switch, 40 pcs box (LCdatalink) | 40 | 2 | OK |
| Double-sided perfboard, **8 × 12 cm, 1.2 mm**, 10 pcs (DWC-LSI) | 10 | 4 | OK. 1.2 mm thick, not 1.6: the stack gets 0.8 mm thinner |
| QMI8658 module (**QMI8658A**) | 2 | 2 | OK, the A variant (pedometer). No spare |
| 1 × 40 right-angle male header, 10 pcs (Repair-parts) | 10 | — | For the user's other needs (not this project) |
| 1 × 40 right-angle male header, "reverse bending", 10 pcs (FuXing) | 10 | — | For the user's other needs (not this project) |
| PCF8563 RTC module, 5 pcs (EGBO) | 5 | 2 | OK |
| SOT-23 to SIP3 adapters, 50 pcs (CNIM) | 50 | ~10 | OK |
| AO3401 (BXV), 50 pcs | 50 | ~10 | OK |
| 1N5819 × 100 (LXQY) | 100 | ~6 | OK |
| 28 AWG stranded, Boxed 2 (white, red, grey, orange, purple) and Boxed 1 (red, black, yellow, blue, green), 5 × 10 m each (Beteno) | 2 | — | OK: 9 distinct colours. Mapped in `wiring.md` |
| Dupont jumper kits, 15 cm and 10 cm (Realpoy) | 2 | — | OK, bench use |
| 2.8" TFT ST7789V, SPI/8/16-bit (**2.8 tft**, Great IT) | 2 | 2 | OK. No spare |
| 8 × 8 × 5 mm tactile, **2-pin**, 20 pcs (Javino) | 20 | 4–6 | OK: Start, Select (and Menu) |
| 8 × 8 × 5 mm tactile, **4-pin**, 20 pcs (Javino) | 20 | — | Spare type; fine for L/R or Menu later |
| ESP32-S3-MINI S3-N16R8 (**Mini antenna/1pc**, NEW GREAT BOBOT) | 2 | 2 | OK, with an antenna included (check it's IPEX). **No spare**: a third is worth it |
| TPS63802 buck-boost (**TPS63802/5V**, W Official) | 3 | 2 | OK: the output is a solder jumper. **On arrival, move it from 5V to 3V3 on each board and measure 3.30 V before connecting anything** |
| MCP23017 module, 5 pcs (Great IT) | 5 | 4 | OK, one spare |
| ES8311 + NS4150B audio module (**Type 2/1PCS**, with external mic) | 2 | 2 | OK. No spare |
| 18650 holder, pre-wired (**5pcs-one grid**) | 5 | 2 | OK |
| TP4056 USB-C charger board, 5 pcs | 5 | 2 | OK |
| Speaker 36 × 25, **8 Ω 2 W**, 5 pcs, 1.25 mm plug (Module Sky) | 5 | 2 | OK. Check the 1.25 mm plug against the codec's SPK socket |
| PSP joystick module (**MZ-YG**, ZC Technology) | 3 | 2 | OK |
| 4682-type micro-SD SPI/SDIO breakout (ZT-Stationery) | 3 | 2 | OK |
| TS-G010 illuminated switch (**1 × 5 colours**, DQLZV) | 3 lots | 2 × (red, yellow, blue, green) | OK: 3 of each colour, one spare per colour (the probe for the LED wiring); whites spare |
| 18650 "3.7V … for LED" (**4 Battery 1 Charger/Brazil**, Global Battery Factory) | 1 kit | 2–4 | Accepted by the user for the first builds. Check the real capacity with the kit's charger or a meter-timed discharge; plan battery life on that, not the label. Genuine Samsung 35E / LG MJ1 later |

Not for this project: the right-angle headers, the Samsung remote and the AA batteries in the same cart.

## Still to get (2026-10-31): only the local-market items; the cart covers the rest for two devices

- **Local market:** genuine microSD cards (2 × 32 GB, A1, SanDisk, Samsung or Kingston) and the RTC coin cells (check the holder size; probably CR1220). The fake 512 GB cards were removed from the cart.
- **From stock:** 1/4 W film resistors (assorted box): 220 kΩ ×4, 270 Ω ×8, 100 kΩ, 10 kΩ. Check the box has 220 kΩ and 270 Ω; 200 kΩ / 300 Ω also work.
- **Female round-pin sockets and straight round male pins:** in stock (about 170 positions per device: S3 44, expanders 60, codec 10, SD 9, display 9, stick 4, board-to-board 20, IMU 8).
- **No spare S3 board** (the user's call).

## To buy, phase 1

| Block | Candidate | Why | Check before buying |
|---|---|---|---|
| MCU | **ESP32-S3 mini board** ("ESP32-S3_MINI", 22.4 × 32 mm, chip-down **ESP32-S3R8** with 8 MB octal PSRAM, external flash, ceramic antenna + IPEX, USB-C with data, RST/BOOT, on-board regulator). Notes and images: `modules/esp32-s3-mini.md` | The user's pick (2026-10-31). One small board does what the bare module needed four parts for: USB-C with data, reset/boot, regulator, and an IPEX socket for the flex antenna. Smaller than the WROOM-on-adapter | Variant: **N16R8** (16 MB flash, 8 MB PSRAM), confirmed. Remove the RGB LED and the small LED for standby. No RF shield, which is fine here. Move the 0 Ω to the IPEX side |
| Wi-Fi/BT antenna | Compact 2.4 GHz flex PCB antenna with a U.FL (IPEX) lead, ~26 × 8 mm, 10–15 cm cable | Top right of the second board's outer face (seen from the back) | Plastic back cover over it; keep-out on both faces |
| Display | **2.8" TFT ST7789, 240 × 320, SPI module, 54.74 × 81.28 × 4.4 mm**, 9-pin header (GND, VCC, SCL, SDA, RST, DC, CS, BL, SDA-O), 4 × Ø3.4 mounting holes, IM jumpers (SPI / 8080 parallel). Notes: `modules/display-2.8in-st7789.md` | Biggest panel that fits; same outline as the first listing, so no layout change | Check the IM jumpers are set to SPI and that BL drives the on-board transistor. Socketed during development, soldered in the final build |
| Display, fallback | 2.4" IPS ST7789V module from the same listing, 47 × 71 mm | Fits with margin if the 2.8" overhang is a problem | The listing gives 13.5 mm total height: check what sticks out at the back |
| Thumbstick | **Deek-Robot JOYSTICK-02**: PSP-style analog stick on a 25.8 × 25.8 mm breakout, two 10 kΩ pots, 4 pins (GND, X, Y, VCC), no click. Notes: `modules/joystick-psp-deek-robot.md` | Low profile, self-centring, small enough to sit left of the diamond | The header comes right-angle: swap it for straight pins (or wires) through the main board. Mount with M2 screws through its corner holes. Measure its height |
| Buttons, A/B/X/Y | **TS-G010 12 × 12 mm illuminated tactile switches** (white LED, coloured cap; DQLZV listing): 1 red (A), 1 yellow (B), 1 blue (X), 1 green (Y), plus spares. Notes: `modules/button-12mm-illuminated.md` | SNES colours, a light per button for game feedback and pet alerts. 7.3 mm tall with the cap | **Check one LED's wiring before wiring four** (the datasheet doesn't draw it). The tab pins are 9 mm apart: bend onto the diagonal holes. Buy 4 right-angle switches too for the future L/R |
| Buttons, Start/Select | **7.8 × 7.8 mm tactile switch, silicone actuator, 2-pin version** ("Javino" listing), ×2 + spares. Notes: `modules/button-8mm-tact.md` | No LED. 2 legs 5.1 mm apart: exactly two holes on the perfboard. 5.5 mm tall, a little lower than the lit buttons | The 4-pin version also works (legs bent onto 7.62 × 5.08) |
| Button-LED drive | 4 × 270 Ω (through-hole), AO3401 + BS170 for the PWM brightness switch | The LEDs' cathodes go to the second MCP23017 | — |
| Off switch | SPDT mini slide switch (SS12D00 class) + AO3401 P-MOSFET + 100 kΩ | The switch drives the FET's gate; the FET cuts VSYS after the charger and power path (the buck-boost has no EN pad) | The switch carries no load current, only the gate |
| GPIO expanders | **2 × MCP23017 breakout module, 26 × 21 mm** (MCP23017-E/SS pre-soldered, 10 kΩ array and passives on board; 30 holes at 2.54 mm). Notes: `modules/mcp23017-module.md` | 16 I/O each over I2C, internal 100 kΩ pull-ups (no button resistors), open-drain mirrored INT, ~1 µA standby. No SMD work and no extra parts, unlike the DIP-28 chip; no power-wasting LEDs, unlike the TCA9555 modules | GPA7/GPB7 are output-only on current silicon: use them for enables. Second module's A0 to VCC for 0x21. Check how many I2C pull-ups end up in parallel |
| Audio: codec + amp | **ES8311 + NS4150B audio module, 26.1 × 17 mm** (10-pin header: SDA, SCL, MCK, BCK, DI, WS, DO, 3V3, VCC, GND; MIC and SPK sockets), **with its external electret mic on a lead**. Notes: `modules/audio-es8311-ns4150b.md` | One small board for speaker and mic. The mic on a lead goes wherever the case wants it. The same pairing as Espressif's own S3 boards, with `esp_codec_dev` support. Mono, as decided | PA_EN isn't on the header: lift R8 and wire PA_EN to the second expander to switch the amp off. VCC from VSYS. Confirm DI/DO direction. MCK may be droppable (ES8311 clocked from BCK) |
| Speaker | **36 × 25 × 6.5 mm enclosed speaker, 8 Ω 2 W** (the 4 Ω 3 W variant is the fallback if it's too quiet). Notes: `modules/speaker-36x25.md` | Closed box with an adhesive gasket, so it seals to the board. 8 Ω halves the amp's current peaks; ~0.85 W from VSYS is plenty | Check that its 2-pin plug mates with the codec module's speaker connector |
| micro-SD | **"microSD SPI or SDIO" breakout, 3 V only (Adafruit 4682 layout or a clone), 25 × 22 mm**, 9 pins: 3V, GND, CLK, D0, CMD, D3, D1, DAT2, DET. Notes: `modules/micro-sd-sdio-board.md` | Every SD line broken out, with pull-ups and card detect; no LDO, runs from 3V3. **1-bit SDMMC** now (CLK, CMD, D0); 4-bit wired and ready if 3 GPIOs free up | DET goes to the second TCA9555. Check the pull-ups and which way DET switches on arrival |
| Battery | 18650, 3000–3500 mAh, **unprotected** (65 mm) | It fits a standard holder; the charger module protects it | Buy from a known brand (Samsung, LG, Molicel, Sony/Murata); fake-capacity cells are common |
| Battery holder | **Single 18650 holder, pre-wired, 75 × 20 × 18 mm** (spring contacts, centre screw hole). Notes: `modules/battery-holder-18650.md` | On its side between the boards, open face to the bottom edge. Its 20 mm width sits in the 25 mm gap | Glue a side wall to the second board (its screw hole faces up when on its side). Shorten the leads |
| Reverse polarity | AO3401 P-MOSFET on a SOT-23 → SIP3 adapter + 100 kΩ gate resistor | A cell put in backwards must not damage anything | No gate zener needed: the AO3401's gate is rated ±12 V and nothing here exceeds the cell's 4.2 V |
| Charger | **Mini Li-ion charger + protection board, 20 × 15 × 5 mm, USB-C** (TP4056-type SOP-8 + single-chip protector; IN, B and OUT pads). Notes: `modules/charger-tp4056-mini.md` | Small. Separate B+/B− and protected OUT+/OUT− pads. **Its USB-C is the device's charging port** | The S3 mini board's VIN also feeds IN+, through an SS14 (no back-feed between the two ports). CHRG/STDBY are only on the LEDs: wire from the LED cathodes to the expander. Read RPROG (1 A vs 500 mA) |
| Power path | AO3401 P-MOSFET (on a SOT-23 → SIP3 adapter) + 2 × **1N5819** Schottky (through-hole; the stock 1N5711 is too small and the 1N4148 is not a Schottky) + 100 kΩ | Load sharing: runs the system from USB while charging. The second SS14 joins the S3 board's VIN to the charger input | — |
| 3.3 V converter | **TPS63802 buck-boost module, 25.8 × 12.8 mm** (VIN/GND, VOUT/GND pads on the 2.54 grid; 3V3/4V2/5V jumpers; PS pad; 0.47 µH inductor). Notes: `modules/buck-boost-tps63802.md` | Full cell range, ~90 % efficient, ~11 µA idle in power-save mode. Solders straight into the perfboard; proper inductor and output caps for Wi-Fi bursts. Chosen over a 10.4 × 7.9 mm castellated board | Bridge 3V3 only. Make sure PS selects power-save. No EN pad: the off switch becomes an AO3401 on VSYS |
| Battery voltage | 2 × 220 kΩ (1/4 W, through-hole) + 100 nF ceramic | A divider from B+ into an ADC1 pin replaces a fuel-gauge chip: ~10 µA drain, ±1–2 % with the S3's ADC calibration. Chosen over the MAX17048 (too expensive for what it adds) | Read it under light load; the firmware maps voltage to percent |
| Spacers | **In stock, with screws.** M3 hex standoffs, male-female (stackable): 4 × 20 mm + 4 × 5 mm for the 25 mm gap, plus 5/10/15 mm for tuning; **2 sets in nylon** for the top (antenna) corners (check the stock has nylon ones) | Top corners and, moved up above the battery band, two drilled holes at the bottom | Male-female, so they stack |
| Wire, signals | **28 AWG stranded**, thin insulation (PTFE ideal, ~0.9 mm; silicone OK up to ~1.2 mm), a multi-colour kit. In the cart: a 5-colour boxed set, 5 × 10 m | Flexible, thin enough for the half-step lanes (1.27 mm apart) and parallel ribbons, and PTFE doesn't melt back when a neighbouring joint is soldered | Silicone 26 AWG is too fat (~1.5 mm) for half-step lanes. 30 AWG wire-wrap (Kynar, solid) also works, but it is stiff, not flexible |
| Wire, power | **In stock: 22 AWG solid, insulated, red, black and white** | Battery, charger, power path, VSYS and the 3V3/GND trunks: up to ~1 A. Red = VSYS/battery +, black = GND, white = 3V3 | Solid is fine for lanes fixed to the board. The battery holder's own leads cover the cell |
| 2.4 GHz LoRa radio (V1) | **EBYTE E28-2G4M27S** — **bought: 2** (SX1281, 27 dBm, SPI, IPEX) **+ TX2400-JKS-IPX20** rubber-duck antenna (5 dBi, IPEX pigtail, SMA). Notes: `modules/radio-e28-2g4m27s.md` | FH1-to-FH1 links: messages, pet trading, multiplayer, kilometres of range. Replaced the LTE/3G modem (2026-11-02) | Castellated 1.27 mm pads: hand-wired to a header strip. From 3V3 (max 3.6 V), 580 mA TX bursts, 470 µF at the module. Cap TX power to the Anatel limit |
| Bulk caps | **10 V 470 µF, 6.3 × 8 mm** low-ESR on VSYS; **16 V 100 µF, 5 × 7 mm** on 3V3 (2 of each per device). plus one more **10 V 470 µF** at the E28's VCC (3V3) per device | Wi-Fi bursts and amp transients. VSYS reaches ~4.7 V on USB, so 10 V keeps margin (a 6.3 V part would run at ~75 %). Both are short (≤ 8 mm) | Lead spacing ~2.5 mm (6.3 mm cans) and ~2 mm (5 mm cans) |

## To buy, phase 2 (optional)

| Part | Candidate | Why |
|---|---|---|
| RTC | **PCF8563 module, 43 × 17 mm, with coin-cell holder** (INT, COT, SCL, SDA, GND, VCC). **Bought, 5 units.** Notes: `modules/rtc-pcf8563-module.md` | Keeps the pet's clock while the device is off; alarm output wakes the S3 through the expander INT. ~0.25 µA, I2C 0x51 | Fit a coin cell (probably CR1220). On the second board's inner face, beside the board-to-board header |
| Motion sensor | **QMI8658 6-axis module, single 8-pin row** (VCC, GND, SCL, SDA, XDA, XCL, AD0, INT), mounted upright. Notes: `modules/imu-qmi8658-module.md`. Alternative: **BMI160 module (GY-BMI160)**, 18 × 13 mm, pins on two sides | The single row stands upright at 90° and takes one row of holes. I2C 0x6A/0x6B, no clash. INT to MCP #2 GPB2 (through the board-to-board header) | **Buy the QMI8658A** (built-in pedometer per QST's datasheet). If the listing is the QMI8658C or doesn't say, the BMI160's hardware step counter is the safer pick. Remove the module's power LED |
| L/R shoulder buttons | Right-angle tactile switches on the second board's top edge | Planned now, built later; the expander inputs are reserved |
| BT headphone transmitter | KCX_BT_EMITTER-class A2DP transmitter module on the codec output, power-switched | The S3 has no Classic Bluetooth, so it can't stream to headphones itself (see open question 16) |
| Vibration | Coin vibration motor + N-MOSFET + diode | Pet alerts, game rumble |
| Battery monitor (option) | INA219 current/voltage module (I2C), its shunt swapped to 0.01–0.02 Ω | Measures battery current as well as voltage: power profiling during development, and charge counting for a steadier percentage. On I2C, so it would free the divider's ADC pin. Useful as a bench tool even if it never goes into the build. Needs firmware to put it in power-down between reads (~1 mA otherwise) |

## Phase 3: deferred, space and pins reserved (expansions on the second board)

Not bought yet, since the cost (shipping included) is unknown. The second-board header and the GPIO budget keep room for both.

| Part | Candidate | Why | Note |
|---|---|---|---|
| Camera, option (a) | Seeed XIAO ESP32S3 Sense (OV2640 or OV3660, mic, SD) | Own S3, frees the main GPIO budget | Power-gated by a load switch |
| Camera, option (b) | OV2640 module with a 24-pin FPC + FPC breakout | Direct DVP on the main S3 | Needs 12 pins: see the GPIO budget in `architecture.md` |
| Load switches | TPS22918 or an AO3401 + NPN pair | Gate the camera | — |

## Sources

- [Retro-Go fork docs: all systems except SNES full speed on an ESP32-S3 N16R8 at 240 MHz](https://pjcau.github.io/esp32-emu-turbo/docs/software) (a project claim, not a measurement)
- [2.4" ST7789V IPS module mechanical figures (DisplayModule DM-TFT24-311)](https://displaymodule.com/products/dm-tft24-311), [DM-TFT24-333](https://www.displaymodule.com/products/dm-tft24-333), [Tailor Pixels TTS024BVS-01](https://www.directindustry.com/prod/tailor-pixels-technology-co-ltd/product-245050-2493511.html)
- [Waveshare 2" LCD module, 58 × 35 mm](https://waveshare.com/2inch-lcd-module.htm)
- [Adafruit Analog Mini Thumbstick Breakout (3246), 26.6 × 25.4 mm](https://www.adafruit.com/product/3246); [the full thumb joystick kit (512), 38 × 38 × 32 mm](https://adafruit.com/product/512)
- [TPS63802 datasheet (11 µA Iq)](https://ti.com/document-viewer/TPS63802/datasheet), [Micropowercore TPS63802 charger + buck-boost module, 17 µA](https://lectronz.com/products/micropowercore-charger-dc-dc-boost-bu-k-iq-17-a)
- [Elektor: ESP32 board overhead dominates deep-sleep current](https://www.elektormagazine.de/labs/esp32-low-consumption-power-supply-for-lithium-battery-solar-recharge)
- [ES8311 + NS4150 on S3 boards (Espressif esp_codec_dev)](https://components.espressif.com/components/espressif/esp_codec_dev/versions/1.6.2/readme), [ES8311 + NS4150B module listing](https://www.onbuy.com/gb/p/es8311-ns4150b-tft-codec-digital-audio-display-module-with-built-in-audio-amplifier-and-microphone-for-audio-output~p242631270/)
