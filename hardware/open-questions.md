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
