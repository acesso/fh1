---
component: 12 × 12 mm illuminated tactile switch with a round coloured cap and a built-in LED, part TS-G010 (DQLZV listing)
image: button-12mm-illuminated.png (seller's photo with dimensions).
role: the four lit game buttons (A, B, X, Y). Start and Select use plain 8 mm switches (`button-8mm-tact.md`)
status: read off the seller's image
---

# 12 mm illuminated tactile switch

## Read from the image
- **Body:** 12 × 12 mm, black, with a white square carrier on top. Four black dots mark the push-pin corners.
- **Width over the side tabs:** 15 mm. Two white plastic tabs stick out on opposite sides, each carrying a pin.
- **Cap:** round, translucent: about 8.2–8.3 mm across the white collar, Ø6.9 mm for the coloured cap itself (datasheet). It comes in red, yellow, green, blue and white.
- **The LED inside is white**, seen under the clear cap in the small photo. So the colour comes from the cap, and every switch has the same LED, as the user expected.
- **Pins:** the four switch legs of a 12 × 12 tactile switch, plus the two LED pins in the side tabs. That makes six through-hole pins.
- **Height:** see the datasheet below (7.3 mm).

## Read from the datasheet drawing (TS-G010)
- **Rating:** 50 mA, 12 V DC (the switch contacts).
- **Cap:** Ø6.9 mm (the photo's 8.2–8.3 mm includes the white collar).
- **Height:** 7.3 mm from the board to the top of the cap, of which the cap is 3.3 mm. The legs stand 3.5 mm below the body.
- **Body:** 12 ± 0.1 mm square; 15 mm over the tabs; 13 mm over the legs in the front view.
- **Land pattern:**
  - **4 × Ø1.2 mm holes** for the switch legs, 6.5 ± 0.1 × 5 ± 0.1 mm apart. Both are close to the 2.54 mm grid (6.5 ≈ 2.5 × 2.54 = 6.35, 5 ≈ 2 × 2.54 = 5.08), so the legs bend slightly into perfboard holes.
  - **2 × Ø1.6 mm holes** for the tab pins, 9 ± 0.2 mm apart on the centre line. 9 mm is not on the grid (3.5 holes), so these two need a drilled hole or bent pins.
- **Circuit diagram:**
  - Pins 1–2 are joined, pins 3–4 are joined, and the switch sits between the two pairs.
  - **Pin 5 is drawn to ground.** The LED and the sixth pin are **not drawn**.

## Inferred — needs your confirmation
1. **Switch legs:** like any 12 × 12 tactile switch, the legs are in two pairs, each pair already connected inside. Only one leg from each pair is needed electrically, but solder all four for strength.
2. **LED wiring: the main risk.** The likely reading is that the two tab pins are the LED, with pin 5 as its cathode (ground) and the other tab as its anode, independent of the switch.
   - The diagram doesn't show it. Some parts of this kind wire the LED to one side of the switch instead, so it only lights while pressed.
   - **Check one on arrival before wiring six:** a meter in diode mode across the two tab pins, then across each tab and the switch legs.
   - If the LED turns out to be tied to the switch, independent control needs the switch side driven differently. The fallback is to light it only on press.
   - There is no resistor inside: add one per LED.
3. **The LED is white,** so its forward voltage is about 2.8–3.1 V at a few mA. That is too close to 3.3 V to run well from the 3V3 rail; see "Driving the LEDs".

## Use in this project
- **Four of them:** the SNES colours. Start and Select are plain 8 mm switches without LEDs (`button-8mm-tact.md`).

| Button | Cap |
|---|---|
| A | red |
| B | yellow |
| X | blue |
| Y | green |

- **Menu (BOOT)** stays a small plain switch.
- **Switches:** read through the first MCP23017, which pulls each switch up internally; the switch pulls it to GND.
- **Driving the LEDs:**
  - **Supply:** each anode gets a resistor (about 270 Ω) from **VSYS** (3.0–4.2 V), through one shared high-side P-MOSFET (AO3401). A spare GPIO switches that FET with PWM, through a BS170 or NPN, as a **global brightness** control.
  - **Switching:** each cathode goes to an output on **MCP #1's GPB pins (address 0x20)**, so every LED turns on and off on its own. The expander sinks the current.
    - When its output is high, at 3.3 V, the LED sees at most 0.9 V across it and stays dark.
  - **Current:** about 1.5–5 mA per LED, varying with the battery voltage. The battery-voltage divider (an ADC pin) reads it, so firmware can raise the PWM duty as the cell drains to keep the brightness even.
  - **All four lit:** about 10–17 mA. Off when the device sleeps.
  - **Per-LED fading:** if the pet app ever needs each LED to fade on its own, a PCA9685 (16-channel PWM over I2C) can take the second expander's place.
- **Layout:**
  - Each switch needs a 12 × 12 mm body, or 15 mm across its tabs. So the diamond spreads to ±14 mm between centres, and Start/Select move below the stick (see `layout.md`).
  - Through the perfboard, each switch uses 4 + 2 holes.
