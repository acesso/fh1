# Requirements

Each feature is turned into what the hardware must provide. Phases say what is built first; later phases still get pins and space reserved now.

| # | Feature | Hardware requirement | Phase |
|---|---|---|---|
| R1 | Emulators (GBC, NES, later SNES-class if the S3 copes) | ESP32-S3 at 240 MHz with octal PSRAM (N16R8 or N8R8). 2.8" IPS 320 × 240 colour panel on SPI at 40–80 MHz with DMA. 16 MB flash for the launcher, emulators and OTA slots | 1 |
| R2 | Virtual pet app | Deep sleep with timer and button wake. A clock that stays right across days (NTP over Wi-Fi, or an RTC chip). Speaker for alerts | 1 |
| R3 | Controls | 2-axis analog thumbstick (2 ADC1 pins), no click (Deek-Robot JOYSTICK-02). SNES/PlayStation layout: A, B, X, Y diamond (lit) plus Start and Select (plain) (NES apps use 2 of the 4). A small Menu button (= BOOT). Later: L and R on the back, on the second board | 1 (L/R: 3) |
| R4 | Audio out | Mono speaker, about 1 W, through an I2S class-D amp with a shutdown pin | 1 |
| R5 | Audio in | I2S MEMS microphone, voice quality (16 kHz is enough) | 1 |
| R6 | Battery operation | One 18650 cell, USB-C charging while playing (power path), protection, a battery-voltage reading (divider into an ADC pin), a real off switch | 1 |
| R7 | Low power | Efficient 3.3 V conversion over the whole 3.0–4.2 V cell range. Every power-hungry block can be switched off: backlight, amp, thumbstick, SD, radio, camera | 1 |
| R8 | Storage | micro-SD on SDMMC, 1-bit to start; the board is wired for 4-bit if 3 GPIOs free up. Card detect | 1 |
| R9 | OTA | Wi-Fi OTA with A/B slots and rollback (firmware). A reachable BOOT + RESET for USB recovery (hardware) | 1 |
| R10 | Wi-Fi / Bluetooth | Native on the S3 (BLE only, no Classic). External flex antenna (through the S3 mini board's IPEX socket) on the second board's outer face, away from the display and the cell | 1 |
| R11 | Camera | Deferred: pins and space reserved, nothing bought. Route decided when it's built | 3 |
| R12 | Device-to-device radio | 2.4 GHz LoRa (EBYTE E28-2G4M27S, SX1281), SPI, external antenna. Replaced the LTE/3G modem (2026-11-02); in V1 (2026-11-03) | 1 |
| R13 | Modularity | Every module on round-pin sockets. Board-to-board link through a header, so the second board can be left off | 1 |

## Not in scope (for now)

- Touch screen.
- Hi-fi audio or stereo speakers.
- Hardware 3D or video decoding beyond what the S3 does in software.

## Phases

1. **Core handheld:** S3 mini board (with its USB-C), the E28 LoRa radio, display, buttons, thumbstick, audio, SD, battery and power. Enough for the emulator and the pet.
2. **Final build:** solder the display (and optionally the S3 mini board), and add an RTC chip, a vibration motor and (optionally) a BT headphone transmitter.
3. **Expansions on the second board:** L/R buttons, camera.
