---
component: EBYTE E28-2G4M27S, 2.4 GHz LoRa / FLRC / GFSK transceiver (Semtech SX1281) with a 27 dBm PA, SPI, IPEX antenna socket; with a TX2400-JKS-IPX20 antenna
image: radio-e28-2g4m27s.png (module, antenna and main parameters), radio-e28-2g4m27s-drawing.png (outline drawing and pin table)
role: device-to-device radio (FH1 to FH1: messages, pet trading, multiplayer), replacing the LTE/3G modem
status: **V1** (2026-11-03): the user bought 2, one per device; read off the seller's images, not in hand yet
---

# E28-2G4M27S radio module (SX1281, 2.4 GHz)

## Read from the images
- **Chip:** SX1281. 2400–2500 MHz ISM band (the listing's "25000 MHz" is a typo).
- **Power:**
  - TX power up to **27 dBm (~500 mW)**.
  - Supply **1.8–3.6 V**, 3.3 V logic; the listing warns that 5 V logic risks burning it.
  - **TX current 580 mA** (instantaneous), **RX current 14.5 mA**.
- **Range:** the seller claims 8 km in clear open sky, with a 5 dBi antenna 2.5 m up, at 1 kbps.
- **Interface:** SPI, 0–10 Mbps.
- **Size:** 26.5 × 15 × 2.85 mm, with a PCB antenna area at one end plus an **IPEX** socket. The "SX" variant (20 × 15 mm) has no PCB antenna.
- **Pads:** **16 castellated half-holes at 1.27 mm pitch**, 8 on each long side. Pad 1 is near the top left; the pitch and the 2.69 mm first-pad offset are from the SX drawing.
- **Pins:**

| Pad | Pin | Notes |
|---|---|---|
| 1 | VCC | 1.8–3.6 V; add a ceramic filter capacitor |
| 2–4 | (cut off in the image) | Most likely GND, MISO, MOSI. **Check the full table** |
| 5 | SCK_RTSN | SPI clock |
| 6 | NSS_CTS | SPI chip select |
| 7 | GND | |
| 8 | RX_EN | LNA control, active high |
| 9 | TX_EN | PA control, active high |
| 10 | GND | |
| 11 | NRESET | Active low, internal 50 kΩ pull-up |
| 12 | BUSY | Output, busy status |
| 13–15 | DIO1–DIO3 | Interrupt pins |
| 16 | GND | |

- **Antenna: TX2400-JKS-IPX20.** A 2.4 GHz rubber-duck antenna, 5 dBi, 50 Ω, SWR ≤ 1.5. It has an IPEX-1 pigtail, probably 20 cm, to an SMA bulkhead.

## Inferred — needs your confirmation
1. **Pads 2–4:** the pin table image is cut off there. Read the full table (or the EBYTE manual) before wiring.
2. **The RF switch is manual.**
   - The MCU must drive **TX_EN high only while transmitting** and **RX_EN high only while receiving**, never both. Both low between packets.
   - The SX1281 doesn't drive them itself.
3. **Hand soldering:** the castellations are 1.27 mm apart. They are hand-solderable, bigger than SMD resistors, but not perfboard-pitch.
   - **Plan:** glue the module flat to the board, solder short 28 AWG wires to the half-holes, and run them to a 2.54 mm header strip beside it.
   - **Alternative:** a 1.27 → 2.54 mm adapter board of the right size, if one turns up.
4. **Power and the law.** 27 dBm + 5 dBi is about **32 dBm EIRP**. That is likely above what Anatel allows for this kind of 2.4 GHz device. Check the current Brazilian limit and set the SX1281's TX power so the EIRP stays within it. The firmware sets the power per packet.
5. **Wi-Fi next door: the main risk.** The radio transmits on the same band as the S3's Wi-Fi, and at up to 500 mW. With the antennas a few cm apart, it can desensitise the Wi-Fi receiver, and at full power it may even stress its front end.
   - Keep the antennas as far apart as the case allows.
   - Never transmit on the radio while Wi-Fi is active. The firmware time-shares them.
   - Use lower TX power for short-range play.

## Use in this project
- **Supply: from 3V3, not VSYS.** VSYS reaches 4.2–4.7 V, above the module's 3.6 V maximum.
  - The TPS63802 (2 A) covers the 580 mA bursts on top of the S3's Wi-Fi peaks, as long as the two don't overlap (the time-sharing rule above).
  - Put a **10 V 470 µF polymer** at the module's VCC pad, plus a 100 nF ceramic.
- **SPI on its own bus (SPI3),** shared later with the camera coprocessor. It runs at 4–8 MHz; the module allows 10 MHz.

| Signal | To |
|---|---|
| SCK | GPIO7 |
| MOSI | GPIO9 |
| MISO | GPIO11 |
| NSS | GPIO13 |
| BUSY | GPIO15 |
| DIO1 | GPIO17 (RTC GPIO: a received packet can wake the S3) |
| NRESET | MCP #2 GPA3 (board-to-board row 15) |
| RX_EN | MCP #2 GPA7 (board-to-board row 23) |
| TX_EN | MCP #2 GPB7 (board-to-board row 22) |

  - **Expander timing:** switching TX_EN and RX_EN through the expander takes about 0.1 ms per change, which is fine for packet radio.
- **Radio header (1 × 9):** main board back, row 0, columns 3–11. It sits right above the S3's top outer pin row, which holds GND, 3V3, GPIO21, 17, 15, 13, 11, 9 and 7 in that order, so every line is a one-hole hop from S3 pin to header pin. It carries GND, 3V3, camera CS (GPIO21, later), DIO1, BUSY, NSS, MISO, MOSI and SCK. The mating half is on the second board, row 0, columns 18–26.
- **Placement:** on the second board's inner face, **top right**, under its header, glued flat with the antenna end to the right, next to the SMA corner. Short 28 AWG leads go from its castellated pads to the header pins above and to the three control lanes below. The 470 µF sits beside it.
- **Antenna:** the JKS rubber duck on an SMA bulkhead through the case's **top-left edge** (seen from the back), as far from the Wi-Fi antenna (top right) as possible. Its pigtail runs from the module's IPEX socket. The duck sticks out above the case.
- **Battery:**
  - **Receiving costs 14.5 mA,** so continuous listening drains the battery. Listen in short windows (duty-cycled RX); the SX1281 can wake the S3 through DIO1 when a packet arrives.
  - **Sleep** is about 1 µA (chip figure).
