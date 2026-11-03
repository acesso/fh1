# Architecture

## Block diagram

```
      USB-C on the S3 mini board (flash, console)     USB-C on the charger board (charge)
                 │ VIN 5 V ──► 1N5819 ──┐                    │ 5 V
                                     └──────────►──────────┤ charger IN+
                    │ VBUS 5 V
                    ▼
 18650 ──► charger + protection (TP4056 + DW01) ──► power path ──► VSYS (3.0–4.2 V)
                                                                     │
               ┌──── off switch (P-FET on VSYS) ─────────────────────┤
               ▼                                                     │
        buck-boost 3.3 V (TPS63802 module, ~11 µA Iq in power-save)              │
               │ 3V3                                                 │
               ▼                                                     └──► speaker amp (NS4150B on the audio module, from VSYS for more volume)
   ┌──────────────────── ESP32-S3 N16R8 ───────────────────────┐
   │ SPI2 ─────────► IPS display 320×240 (ST7789 / ILI9341)    │
   │ LEDC PWM ─────► backlight                                 │
   │ SDMMC ────────► micro-SD                                  │
   │ I2S0 ─ BCLK/WS shared ─► amp (DOUT) + MEMS mic (DIN)      │
   │ ADC1 ─────────► thumbstick X / Y                          │
   │ I2C ──────────► MCP23017 #1 (buttons, status, enables)     │
   │            └──► MCP23017 #2 (amp, charger, radio control)  │
   │ LEDC PWM ─────► button-LED brightness (VSYS high-side FET) │
   │            └──► RTC (optional)                            │
   │ ADC1 ─────────► battery voltage (divider from B+)          │
   │ RTC GPIO ─────► Menu/BOOT button (deep-sleep wake)        │
   │ RTC GPIO ─────► expander INT (any button wakes)           │
   │ SPI3 ─────────► E28 2.4 GHz LoRa radio (V1)               │
   │ SPI3 ─────────► camera coprocessor (deferred, reserved)   │
   └───────────────────────────────────────────────────────────┘
```

## Buses

- **Display on SPI2 (FSPI), its own bus.** MOSI, SCK, CS, DC; reset from the expander or tied to the S3's EN through an RC. No MISO.
  - **Speed:** a 1.5× scaled Game Boy frame (240 × 216, RGB565) is 0.83 Mbit. At 40 MHz that is about 21 ms, so about 47 fps worst case before partial updates; at 80 MHz about 10 ms. Retro-Go only pushes changed lines, so 60 fps is realistic.
  - **Why not 8-bit parallel (i80):** it is faster, but takes 11+ pins and clashes with a camera on the GPIO budget. Keep SPI unless the SPI build misses frames.
- **micro-SD on SDMMC, 1-bit.** CLK, CMD and D0 on the SPI/SDIO breakout (`modules/micro-sd-sdio-board.md`). D1, DAT2 and D3 stay unconnected, held high by its pull-ups. It runs alongside the display without sharing a bus.
  - **4-bit later:** 3 more GPIOs (D1, DAT2, D3).
  - **Card detect (DET):** an input on the second expander.
- **I2S0 full duplex.** One BCLK and WS feed both the amp and the mic, so audio in and out run at one sample rate (16–32 kHz). 4 pins in total.
- **I2C, 400 kHz.** The control plane: the GPIO expanders, the codec, an optional RTC, the camera's SCCB.
- **Battery voltage (no fuel-gauge chip):** a 220 kΩ / 220 kΩ divider from the cell's B+ (after the reverse FET), with 100 nF across the lower resistor, into an ADC1 pin.
  - **Drain:** about 10 µA, always, which is negligible for an 18650.
  - **Reading:** the S3's calibrated ADC reads it to about ±1–2 %. The firmware turns it into a percentage with a Li-ion discharge curve, read when the load is light (sag under load lowers it), and averaged.
  - **Uses:** the battery icon and low-battery shutdown, and keeping the button-LED brightness even.
  - **Possible upgrade:** an INA219 module on I2C, with a 0.01–0.02 Ω shunt in the battery line, measures current as well. That allows charge counting and power profiling, and frees the ADC pin. Not in phase 1.
- **GPIO expanders: two MCP23017 modules** (26 × 21 mm, chip pre-soldered; 16 I/O each, internal 100 kΩ pull-ups, ~1 µA standby; `modules/mcp23017-module.md`).
  - **Why two:** the bus could take many more devices; the limit is pins. The plan needs about 22, and one chip has 16.
  - **Shared interrupt:** both chips' INTA/INTB are mirrored and set open-drain, then tied together to one RTC GPIO with a pull-up.
  - **Output-only pins:** on current silicon, **GPA7 and GPB7 are output-only**, so they carry enables.
  - **Expander #1 (address 0x20), next to the buttons:** the six button inputs, SD card detect and stick power on GPA; the four button-LED cathodes on GPB.
  - **Expander #2 (address 0x21), next to the charger and the board-to-board header:** amp enable, charger CHRG/STDBY the radio's NRESET (GPA3) and RX_EN (GPA7), later L/R on GPA; the radio's TX_EN on GPB7; the RTC and IMU interrupts and later the camera and BT lines on GPB. The full table is in `wiring.md`.
  - **Parts around each chip:** already on the module (decoupling, RESET and address resistors). Only the second module's A0 is tied to 3V3, for 0x21.
  - **Latency:** reading 2 bytes at 400 kHz takes about 60 µs, so polling every frame is free. INT on an RTC GPIO lets any button wake the S3 from light or deep sleep.

## Power tree

- **Cell:** one 18650, 3.0–4.2 V, 2500–3500 mAh. A protected cell, or a charger module with DW01 + 8205 protection.
- **Charging:** the **charger board's own USB-C is the charging port** (power only). The S3 mini board's USB-C also charges, through an 1N5819 Schottky from its VIN into the charger's IN+. The diode stops a charger on one port pushing 5 V back out of the other. About 1 A either way.
- **Power path:** a P-MOSFET (AO3401) plus Schottky (1N5819) load-sharing circuit. With USB in, the system runs from USB and the cell only charges, so the TP4056 terminates properly.
- **3.3 V:** a buck-boost (TPS63802 or similar, about 2 A).
  - It holds 3.3 V down to a 3.0 V cell, so the whole capacity is usable.
  - An LDO would drop out near 3.5 V and leave about 15 % of the cell unused.
  - It feeds the S3 mini board's 3V3 pins directly. With USB in, the board's own regulator also drives 3V3 from VIN; measure that the two share the rail peacefully, or lift that regulator's output pin.
- **Off switch:** a slide switch driving an AO3401 P-MOSFET in the VSYS line, after the charger and the power path. The chosen buck-boost module has no EN pad. When off, only the charger, the protection and the FET's leakage remain; charging still works, and the amp, the button LEDs and (through the buck-boost) the radio are cut too.
- **Speaker amp from VSYS:** the audio module's NS4150B (its VCC pin) takes about 3.0–5.5 V, and a higher rail gives more volume.
- **Radio from 3V3 (V1):** the E28 takes 1.8–3.6 V, so never VSYS. Its 580 mA TX bursts come from the TPS63802, with a 470 µF polymer at the module. The firmware never transmits on it while Wi-Fi is busy.

## GPIO budget (ESP32-S3 mini board, ESP32-S3R8)

**Off-limits:**
- 26–32: flash and the octal PSRAM's chip select. Only 26 reaches the headers; leave it unconnected.
- 33–37: octal PSRAM. They reach the headers too; leave them unconnected.
- 19/20: native USB, wired to the board's USB-C (not on the headers).

**Usable:**
- **Clean (26):** 1–18, 21, 38–42, 47, 48. On this board 48 probably drives the RGB LED: remove the LED before using it.
- **More after giving up UART0 (2):** 43/44. The console goes over native USB instead.
- **Strapping:** 0, 3, 45, 46. 0 is the BOOT button: wiring it as the Menu button doubles as the recovery combo (hold Menu + tap the board's RST = download mode).

| Function | Pins | Notes |
|---|---|---|
| Display SPI (MOSI, SCK, CS, DC) + RST | 5 | Any GPIO via the matrix; use the IO_MUX pins for 80 MHz (FSPI: 11 MOSI, 12 SCK, 10 CS) |
| Backlight PWM | 1 | Drives a small N-MOSFET (BS170 on hand, or AO3400) |
| SDMMC 1-bit | 3 | |
| I2S (BCLK, WS, DOUT, DIN) | 4 | |
| I2C (SDA, SCL) | 2 | |
| Expander INT | 1 | RTC GPIO (0–21) |
| Thumbstick X, Y | 2 | ADC1 only (1–10); ADC2 is unusable with Wi-Fi on |
| Menu (BOOT) | GPIO0 | Strapping, the same button as BOOT; RTC GPIO, so it wakes from deep sleep (ext0) |
| Codec MCLK (ES8311) | 1 | Possibly droppable: the ES8311 can clock itself from BCLK. Its amp enable (PA_EN) goes on the second expander |
| Button-LED brightness (PWM) | 1 | LEDC PWM to the shared high-side switch on the LEDs' VSYS feed |
| Battery voltage | 1 | ADC1 only (1–10), from the divider on B+ |
| **Core total** | **21** | 5 clean pins + 43/44 left |
| E28 radio (SPI3: SCK 7, MOSI 9, MISO 11, NSS 13; BUSY 15; DIO1 17), V1 | 6 | The camera coprocessor later shares the SPI3 bus with its own CS (GPIO21). NRESET, RX_EN, TX_EN on MCP #2 |
| Reserved: camera coprocessor link (SPI: SCK, MOSI, MISO, CS) | 4 | Goes to the second-board header |
| **Core + reservations** | **28** | No clean pin spare; the strapping pins 3, 45, 46 remain, with care. Dropping MCK (ES8311 clocked from BCLK) would free one |

The camera is deferred (decided 2026-10-31). The reservation above fits route (a). A direct DVP camera (12 pins) would not fit next to the radio, so routes for later:

- **(a) Camera coprocessor (preferred).** A tiny ESP32-S3 camera board, such as the Seeed XIAO ESP32S3 Sense (21 × 17.5 mm, camera + mic + SD), on the second board.
  - It talks to the main S3 over SPI or UART: 2–4 pins.
  - It is power-gated from the expander, so it costs nothing when the camera app is closed.
- **(b) SD moves to the display SPI bus,** as the ODROID-GO does. That frees 2 pins; with the strapping pins it then fits, but nothing is left.
- **(c) Leave the camera out for good.**

## Power budget (estimates, to be measured)

| Block | Gaming, Wi-Fi off | Wi-Fi on | Pet idle (deep sleep) |
|---|---|---|---|
| S3 @ 240 MHz | 50–70 mA | 100–130 mA avg, 350–500 mA peaks | ~8 µA |
| Display logic + backlight (mid) | 25–40 mA | 25–40 mA | 0 (backlight off, panel in sleep: ~10 µA) |
| Amp + speaker (moderate) | 10–60 mA | — | ~2 µA (shutdown) |
| Mic | 1.4 mA (only when recording) | — | 0 (BCLK stopped) |
| SD | 1–30 mA | — | 0 (power-gated) |
| Thumbstick (2 × 10 kΩ) | 0.7 mA | — | 0 (gated) |
| Button LEDs (4 white, from VSYS) | 0–17 mA (as lit) | — | 0 (off) |
| Expanders, battery divider, buck-boost Iq | <0.1 mA | — | ~20–30 µA |
| **Total at 3.3 V** | **~110–200 mA** | **~250 mA avg** | **~40–60 µA** |

- **Gaming:** a 3000 mAh cell at 90 % converter efficiency gives roughly **12–20 h**.
- **Wi-Fi on:** about **10 h**.
- **Standby:** the bare-chip figure gives months. With a bare module there are no dev-board extras (LEDs, regulator, USB-UART chip) to subtract; measure the real figure on the built board.

### Virtual pet timekeeping

The S3's internal RC slow clock drifts by several percent, which is minutes per day.

- **Phase 1:** NTP over Wi-Fi at each wake that already uses Wi-Fi, plus drift correction.
- **Phase 2:** a PCF8563 RTC (~0.25 µA, alarm output to an RTC GPIO or to the expander INT line). It keeps time with the S3 fully off and wakes it on schedule.
