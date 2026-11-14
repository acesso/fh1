# Decisions and open questions

Numbered so answers can refer to them. The user decides; each open question has a recommendation.

## Decisions

1. **Display:** the 2.8" IPS ST7789 SPI module (54.74 × 81.28 × 4.4 mm, 9-pin header, `modules/display-2.8in-st7789.md`), its top holes on the board's top corner holes. Socketed during development, soldered in the final build.
2. **Orientation:** upright, Game Boy style.
3. **MCU:** an **ESP32-S3 mini board**, N16R8 (chip-down S3R8, 16 MB flash, 8 MB octal PSRAM, USB-C, RST/BOOT, IPEX; `modules/esp32-s3-mini.md`). IPEX antenna on the second board.
4. **Device-to-device radio:** an EBYTE E28-2G4M27S 2.4 GHz LoRa radio (SX1281) with a TX2400-JKS-IPX20 antenna, on its own 1 × 9 radio header above the S3; control lines through the board-to-board header. See `modules/radio-e28-2g4m27s.md`.
5. **Buttons:** SNES/PlayStation layout. A, B, X, Y are TS-G010 12 mm illuminated switches in the SNES colours (A red, B yellow, X blue, Y green), every LED under its own control. Start and Select are plain 7.8 mm 2-pin switches. Menu is wired to IO0 with BOOT.
6. **Thumbstick:** Deek-Robot JOYSTICK-02, PSP-style, no click.
7. **Battery:** one unprotected 18650 at the bottom, between the two boards, in a pre-wired 75 × 20 × 18 mm holder on its side, open face toward the bottom edge, loaded from the device's bottom.
8. **Board spacing:** metal M3 corner spacers, 25 mm (20 + 5); nylon in the antenna corners.
9. **Power:** a slide switch drives a P-FET on VSYS for hard off; Menu or any button wakes. The mini 20 × 15 mm charger + protection board; the 25.8 × 12.8 mm TPS63802 buck-boost module set to 3V3.
10. **USB:** two ports. The charger board's USB-C is the charging port (side edge, bottom); the S3 mini board's USB-C is for flashing and console and also charges, through a Schottky into the charger.
11. **Audio:** the ES8311 + NS4150B codec module (mono, 26.1 × 17 mm), with the external electret mic on its lead. The speaker (36 × 25 × 6.5 mm, 8 Ω 2 W) on VSYS, on the second board's inner face.
12. **Between the boards:** the speaker and the radio; on the outer face, the antennas in the top corners.
13. **micro-SD:** the SPI/SDIO breakout (Adafruit 4682 layout), 1-bit SDMMC with card detect.
14. **GPIO expanders:** two MCP23017 breakout modules (26 × 21 mm, chip pre-soldered), at 0x20 and 0x21.
15. **Battery level:** a resistor divider into an ADC pin.
16. **RTC and motion:** a PCF8563 module and a QMI8658A module, on the second board's inner face.
17. **Bluetooth controllers:** through a separate bridge board over BLE or Wi-Fi (`bridge/`), not on the FH1's own hardware.

## Open

18. **The switch's LED wiring:** the TS-G010 datasheet draws pin 5 to ground and doesn't show the LED. Check one switch with a meter on arrival: two independent LED pins (the plan) or an LED tied to the switch.
19. **Mic position:** where should the capsule's hole go? Suggested: the front face, between the display and the controls, or the top edge, so it faces the player and stays clear of the speaker on the back.
20. **Radio pads 2–4:** the E28's pin table image is cut off there. Read the full table before wiring.
21. **Radio TX power limit:** check Anatel's current limit for 2.4 GHz digital-modulation devices; 27 dBm + 5 dBi ≈ 32 dBm EIRP is likely above it. The firmware caps the power.
22. **Display at 80 MHz SPI:** the display runs at 40 MHz on GPIO-matrix pins (SCK 18, MOSI 39, CS 38), about 30 fps for full-screen motion. ESP-IDF guarantees matrix routing only up to 40 MHz; write-only SPI may still run faster on it. The ST7789's serial write clock is rated to about 62 MHz (16 ns cycle), and many modules take 80 MHz. Recommendation, on the built board:
    - Raise the clock in steps (60, 80 MHz) on the current pins, and watch for wrong colours or shifted lines on full-screen motion. 80 MHz halves a full screen to about 15 ms (about 60 fps).
    - If the matrix pins don't hold the higher clock, move the display to SPI2's IO_MUX pins: SCK 12, MOSI 11, CS 10. Those carry I2S DOUT, the radio's SPI3 MISO and I2S WS, which run at audio rates and 4–8 MHz and can move to 18, 39 and 38 through the matrix. This changes routes in `wiring.md` that are now crossing-free.
23. **A third GPIO expander on the second board, and a parallel display:** an MCP23017 #3 (0x22) on the second board for the camera coprocessor and later modules. Their power, reset, chip-select and interrupt lines go there instead of on S3 pins, which leaves room for more modules and frees pins for a faster display.
    - **What it frees:** the camera's CS (GPIO21) and its reserved link pins, with the camera's data on the shared SPI3 bus. The display's RST (GPIO48) can also move to an expander.
    - **What a parallel display needs:** the module's 8-bit 8080 mode (IM jumpers 1, 0, 0) uses its 8080 FPC socket instead of the 9-pin SPI header, so the display connection changes. It needs D0–D7, WR, DC and CS (RD tied high, RST on an expander): 11 S3 pins against the SPI's 4, or 10 with CS tied low.
    - **Pin sources:** GPIO21, GPIO16 (codec MCLK, if the ES8311 clocks itself from BCLK), the strapping pins 45 and 46 (no pull-ups at boot), and the radio's BUSY (GPIO15) onto an expander if more are needed. That is just enough.
    - **Speed:** the ST7789's 8080 write cycle is 66 ns, about 15 MB/s. A full screen takes about 10 ms, three times faster than SPI at 40 MHz. The S3's LCD_CAM peripheral drives it (ESP-IDF's i80 driver), with DMA from PSRAM.
    - **Recommendation:** build V1 as wired (SPI). Measure full-screen frame rates first, then try 80 MHz (question 22). Plan the third expander for the second board either way, and take the 8080 route only if 80 MHz SPI is not enough.
