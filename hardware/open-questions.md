# Decisions and open questions

Numbered so answers can refer to them. The user decides; each open question has a recommendation.

## Decisions

1. **Display:** the 2.8" IPS ST7789 SPI module (54.74 × 81.28 × 4.4 mm, 9-pin header, `modules/display-2.8in-st7789.md`), its top holes on the board's top corner holes. Socketed during development, soldered in the final build.
2. **Orientation:** upright, Game Boy style.
3. **MCU:** an **ESP32-S3 mini board**, N16R8 (chip-down S3R8, 16 MB flash, 8 MB octal PSRAM, USB-C, RST/BOOT, IPEX; `modules/esp32-s3-mini.md`). IPEX antenna on the second board.
4. **Device-to-device radio:** an EBYTE E28-2G4M27S 2.4 GHz LoRa radio (SX1281) with a TX2400-JKS-IPX20 antenna, on its own 1 × 9 radio header above the S3; control lines through the board-to-board header. See `modules/radio-e28-2g4m27s.md`.
5. **Buttons:** SNES/PlayStation layout. A, B, X, Y are TS-G010 12 mm illuminated switches in the SNES colours (A red, B yellow, X blue, Y green), every LED under its own control. Start and Select are plain 7.8 mm 2-pin switches. Menu is wired to IO0 with BOOT. L and R are PB86 push buttons on the second board's outer face, one on each side of the centred speaker, their tops level with its top, pressed from behind; their LEDs are not wired.
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
22. **Display at 80 MHz SPI:** SCK and MOSI are wired to SPI2's IO_MUX pins (GPIO12, GPIO11), so the bus can run past the GPIO matrix's 40 MHz; the firmware runs 80 MHz. The chosen module ran clean at 80 MHz on a test board even through the GPIO matrix (16.8 ms a full frame, no wrong colours or shifted lines on full-screen motion; `firmware/docs/display-bench.md`), though the ST7789's serial write clock is rated to about 62 MHz. To check on the built board: the test card and full-screen motion of the Teste de tela package. SCK and MOSI run partly on the front face (`wiring.md`): if the edges ring, shorten those lanes or add a 22–33 Ω series resistor at the S3 end of SCK.
23. **A third GPIO expander on the second board, and a parallel display:** an MCP23017 #3 (0x22) on the second board for the camera coprocessor and later modules. Their power, reset, chip-select and interrupt lines go there instead of on S3 pins, which leaves room for more modules and frees pins for a faster display.
    - **What it frees:** the camera's CS and its reserved link pins, with the camera's data on the shared SPI3 bus; GPIO39, the one clean pin left, is kept for that CS until then. The display's RST (GPIO48) can also move to an expander.
    - **What a parallel display needs:** the module's 8-bit 8080 mode (IM jumpers 1, 0, 0) uses its 8080 FPC socket instead of the 9-pin SPI header, so the display connection changes. It needs D0–D7, WR, DC and CS (RD tied high, RST on an expander): 11 S3 pins against the SPI's 4, or 10 with CS tied low.
    - **Pin sources:** GPIO39, GPIO18 (codec MCLK, if the ES8311 clocks itself from BCLK), the strapping pins 45 and 46 (no pull-ups at boot), and the radio's BUSY (GPIO15) onto an expander if more are needed. That is just enough.
    - **Speed:** the ST7789's 8080 write cycle is 66 ns, about 15 MB/s. A full screen takes about 10 ms, three times faster than SPI at 40 MHz. The S3's LCD_CAM peripheral drives it (ESP-IDF's i80 driver), with DMA from PSRAM.
    - **Recommendation:** build V1 as wired (SPI). Measure full-screen frame rates first, then try 80 MHz (question 22). Plan the third expander for the second board either way, and take the 8080 route only if 80 MHz SPI is not enough.
24. **L and R on the back (PB86):** each stands about 15.7 mm off the second board's outer face (`modules/button-pb86.md`), so the back cover needs two openings and the device is thicker there. Check on arrival: the real height, where the two Ø1.1 pegs go (drill or clip them), and pin 3 (normally open) seen from the solder side. Recommendation: mount one on a scrap of perfboard and hold it behind the main board in the hand before soldering, to confirm the position suits the fingers.

25. **Tearing on the ST7789:** the module brings no TE line out (not on the 9-pin header nor the 8080 sockets; only on the panel's own 40-pin flex), and full-screen motion tears: a frame being written while the panel redraws (top to bottom, in landscape) shows as steps in moving shapes. Tried on the test board, none of which helps: reading the panel's line through SDA-O or back on SDA itself (3-wire) (the controller's ID read, 04h, came back all zeros both ways: this module gives no reads in its serial wiring); sending frames in column bands (worse: half-drawn shapes); a faster panel redraw (FRCTRL2 at about 119 Hz: the panel no longer holds the picture). Recommendation: a display module that brings the ST7789's TE pin out (labelled TE or FMARK on its header), on one GPIO (GPIO39, moving the camera's CS, or whatever the third expander frees), and frames sent when TE marks the start of a redraw, as the Guition's RGB panel does with its v-sync. To check a new module's TE (or find it on a test pad): after the panel's set-up, send TEON (35h, parameter 00) and count rising edges on the candidate through a 10 kΩ resistor: TE gives one a redraw, about 60 a second (the flex also carries the gate supplies, about +15 and −10 V: never probe it directly). On this module both 8080 sockets are fully used (24 and 16 contacts, all labelled) and the flex's pads are too fine to wire..