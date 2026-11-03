# Open questions

Numbered so answers can refer to them. Each has a recommendation; the user decides.

## Decided

1. **Display:** the 2.8" IPS ST7789 SPI module (54.74 × 81.28 × 4.4 mm, `modules/display-2.8in-st7789.md`), upright device, top holes on the board's top corner holes. Socketed during development, soldered in the final build. (2026-10-30/31)
2. **Orientation:** upright, Game Boy style. (2026-10-31)
3. **MCU:** an **ESP32-S3 mini board** (chip-down S3R8, USB-C, RST/BOOT, IPEX; `modules/esp32-s3-mini.md`), no dev board, no bare module. IPEX antenna on the second board. The old ESP32 WROOM board is only a bench fixture for modules. (2026-10-31)
4. **Camera:** deferred; pins (coprocessor link) and second-board space reserved. (2026-10-31)
5. **Cellular:** dropped (2026-11-02). Replaced by an EBYTE E28-2G4M27S 2.4 GHz LoRa radio (SX1281) with a TX2400-JKS-IPX20 antenna, for device-to-device links. See `modules/radio-e28-2g4m27s.md`.
6. **Buttons:** SNES/PlayStation layout. A, B, X, Y plus Start and Select; L and R later, on the second board. (2026-10-31)
7. **Battery position:** at the bottom, between the two boards, behind the controls, held on the second board's inner face. (2026-10-31)
8. **Speaker supply:** VSYS. (2026-10-31)
9. **Power control:** slide switch for hard off, Menu or any button to wake. Good for now. (2026-10-31)
10. **USB:** two ports. The charger board's USB-C is the charging port (side edge, bottom); the S3 mini board's USB-C is for flashing and console and also charges, through a Schottky into the charger. (2026-10-31)
11. **Board spacing:** metal corner spacers, 5–20 mm in 5 mm steps; the gap is tuned while prototyping. (2026-10-31)
12. **Audio:** start with the ES8311 + NS4150B codec module (mono). (2026-10-31)
13. **Speaker:** on the second board's outer face; exact spot set during prototyping. (2026-10-31)
14. **Battery:** a regular plastic spring holder on its side between the boards, open face toward the bottom edge, so the cell loads from the device's bottom; unprotected cell. (2026-10-31)
15. **Audio:** mono is enough; no ES8388 (poor quality in the user's experience); no 3.5 mm jack. (2026-10-31)
16. **Headphones:** none in phase 1. Later, if wanted: a BT A2DP transmitter module on the codec output (the S3 has no Classic Bluetooth). (2026-10-31)
17. **Board gap:** 25 mm (20 + 5 spacers) for now, maybe slimmer during the build. The bottom pair of spacers sits just above the battery band. (2026-10-31)
18. **Between the boards:** the speaker and the radio (was the modem); the camera coprocessor at the top centre. On the outer face: the antennas (top corners) and the camera window. (2026-10-31)
19. **Antennas:** top corners of the second board's outer face, either side of the camera. Fine for now. (2026-10-31)
20. **S3 mini board variant:** N16R8, 16 MB flash and 8 MB octal PSRAM. (2026-10-31)
21. **Buttons:** TS-G010 12 mm illuminated switches in the SNES colours (A red, B yellow, X blue, Y green, Start/Select white); every LED under its own control. (2026-10-31)
22. **micro-SD:** the SPI/SDIO breakout (Adafruit 4682 layout), 1-bit SDMMC with card detect. (2026-10-31)
23. **Thumbstick:** Deek-Robot JOYSTICK-02, PSP-style, no click. (2026-10-31)
24. **Speaker:** 36 × 25 × 6.5 mm, 8 Ω 2 W (4 Ω 3 W as the fallback). (2026-10-31)
25. **Charger:** the mini 20 × 15 mm charger + protection board; its USB-C unused, fed from the S3 mini board's VIN. (2026-10-31)
26. **Battery holder:** pre-wired 75 × 20 × 18 mm, on its side; the 25 mm gap stays. (2026-10-31)
27. **Audio module:** ES8311 + NS4150B, 26.1 × 17 mm, with the external electret mic on its lead. (2026-10-31)
28. **Buck-boost:** the 25.8 × 12.8 mm TPS63802 module (3V3 jumper); the off switch becomes a P-FET on VSYS. (2026-10-31)
29. **GPIO expanders:** two MCP23017 breakout modules (26 × 21 mm, chip pre-soldered), not TCA9555 modules, DIP-28 chips or bare SSOPs. (2026-10-31)
30. **Battery level:** a resistor divider into an ADC pin, no MAX17048. An INA219 module stays a possible future option (and a bench tool). (2026-10-31)
31. **Display listing:** the updated 2.8" module (same outline, 9-pin header with the pinout printed). (2026-10-31)
32. **Start/Select:** plain 7.8 mm 2-pin switches, no LEDs; only A/B/X/Y are lit. (2026-10-31)
33. **LoRa radio in V1:** E28-2G4M27S, 2 bought. Own 1 × 9 radio header above the S3; control lines through the board-to-board header. (2026-11-03)
33a. **Third MCP23017 on the second board:** not for V1 (2026-11-03). For V2, an MCP #3 (0x22) on the second board takes the radio control, the RTC/IMU interrupts, L/R and the camera lines locally. The board-to-board link drops from 9 lines to 5 (3V3, GND, SDA, SCL, shared INT). Needs a sixth module.

## Open

34. **The switch's LED wiring:** the TS-G010 datasheet draws pin 5 to ground and doesn't show the LED. Check one switch with a meter on arrival: two independent LED pins (the plan) or an LED tied to the switch.
35. **The TS-G010 datasheet image** only exists in chat; save it as `hardware/modules/button-12mm-illuminated-datasheet.png`.
36. **Mic position:** where should the capsule's hole go? Suggested: the front face, between the display and the controls, or the top edge, so it faces the player and stays clear of the speaker on the back.
37. **Radio pads 2–4:** the E28's pin table image is cut off there. Read the full table before wiring.
38. **Radio TX power limit:** check Anatel's current limit for 2.4 GHz digital-modulation devices; 27 dBm + 5 dBi ≈ 32 dBm EIRP is likely above it. The firmware caps the power.
