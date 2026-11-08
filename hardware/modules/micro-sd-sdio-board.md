---
component: micro-SD breakout, "microSD SPI or SDIO", 3 V only (Adafruit 4682 layout or a clone of it), 25 × 22 mm
image: micro-sd-sdio-board.png (seller's photo with dimensions)
role: ROM and data storage
status: read off the seller's photo
---

# micro-SD SPI/SDIO breakout

## Read from the image
- **Size:** 25 × 22 mm. The pin row runs along the 25 mm edge.
- **Socket:** a push-push micro-SD socket. The card goes in from the edge opposite the pins.
- **Header:** 9 pins at 2.54 mm. The silkscreen gives the SDIO name and the SPI name.

| Pin | SDIO | SPI |
|---|---|---|
| 3V | power | power |
| GND | ground | ground |
| CLK | CLK | CLK |
| D0 | DAT0 | SO |
| CMD | CMD | SI |
| D3 | DAT3 | CS |
| D1 | DAT1 | — |
| DAT2 | DAT2 | — |
| DET | card detect | card detect |

- **Parts:** two small resistor networks (the pull-ups) and a few passives. No regulator.
- **Marking:** "3V" only; the Adafruit flower logo is printed.
- **Mounting:** two holes.

## Inferred — needs your confirmation
1. **Pull-ups:** every data line and CMD is pulled up to 3V on the board, as on Adafruit's original. Check with a meter: about 10–47 kΩ from each pin to 3V.
2. **DET:** a switch in the socket that closes to GND when a card is in (Adafruit's design). Check which way it goes.
3. **Clone or original:** the photo looks like a clone of Adafruit's 4682. The pinout should be the same either way.

## Use in this project
- **1-bit SDMMC:** CLK, CMD and D0 go to three GPIOs. D3 is left unconnected; its pull-up keeps it high. D1 and DAT2 are unconnected too, pulled up.
- **4-bit** would need 3 more GPIOs (D1, DAT2, D3); the GPIO budget has 1 spare.
- **DET** goes to MCP #1 GPA6 (its internal pull-up holds it high). Inserting or removing a card raises the shared expander INT, so the firmware sees it and it can wake the device.
- **Placement:** on the main board's back, top right (seen from the back). The pin row faces into the board and the card slot faces the side edge.
