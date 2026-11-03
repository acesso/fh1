---
component: TPS63802 buck-boost module, 25.8 × 12.8 mm, VIN/GND and VOUT/GND pad pairs, output jumpers 3V3/4V2/5V, PS pad
image: buck-boost-tps63802.png (two boards, top), buck-boost-tps63802-drawing.png (outline drawing). Also kept, the option not picked: buck-boost-tps63802-tiny-rejected.png (10.4 × 7.9 mm castellated board)
role: makes the 3.3 V rail from VSYS (3.0–4.2 V)
status: recommended 2026-10-31 over the tiny 10.4 × 7.9 mm board; read off the seller's images, not in hand yet
---

# TPS63802 buck-boost module

## Read from the images
- **Size:** 25.84 × 12.85 mm. The inner edges of the pads are 20.59 mm apart.
- **Chip and parts:** a TPS63802 (marked `63802`), an **R47 (0.47 µH) inductor**, 2 input caps and 4 output caps.
- **Pads:**
  - **VIN** and **GND** on the left end, **VOUT** and **GND** on the right end.
  - Each pad has **two holes 2.53 mm apart**, so it lands on the perfboard grid.
- **Output jumpers:** **3V3**, **4V2** and **5V**, solder bridges.
- **PS pad:** bottom left. The TPS63802's MODE pin: power-save (auto PFM/PWM) or forced PWM.
- **No EN pad is visible.** EN is probably tied to VIN on the board.

## Why this one over the tiny board (10.4 × 7.9 mm)
- **Mounting:** its pads sit on the 2.54 mm grid with holes, so it solders straight into the perfboard. The tiny board has four castellated corner pads with no labels.
- **Parts:** this one has the 0.47 µH inductor TI specifies and four output caps, so it rides through the S3's Wi-Fi current bursts with less ripple. The tiny board's inductor and caps are smaller.
- **Settings:** the output is selected by a jumper, and the PS pad gives control over the power-save mode. The tiny board's output voltage and pins aren't marked.
- **Size cost:** 25.8 × 12.8 mm fits easily on the back.

## Inferred — needs your confirmation
1. **Jumpers:** bridge **3V3** only, and check it reads 3.30 V before connecting anything.
2. **PS / MODE:** it must select **power-save mode** (MODE low, auto PFM/PWM). That gives the datasheet's ~11 µA idle, which the standby budget relies on. Forced PWM would idle at several mA. Check where the PS pad goes and what the board does by default.
3. **EN:** if no EN pad exists, the planned "slide switch on the buck-boost EN" moves to a **P-MOSFET switch on VSYS** (see below).

## Use in this project
- **VIN** from VSYS, the power path's output, through the **off switch**. **VOUT** to the 3V3 rail: the S3 mini board's 3V3 pins and every 3.3 V module.
- **Off switch without EN:** the slide switch drives the gate of an AO3401 P-MOSFET in the VSYS line (gate pulled to VSYS by 100 kΩ; the switch pulls it to GND to turn on).
  - The P-MOSFET sits after the charger and the power path, so charging still works with the device off.
  - It also cuts the amp and the button LEDs (the radio runs from 3V3, after the buck-boost, so it goes off too). That makes it a true off for everything.
- **Placement:** on the main board's back, upright, top left under the S3 mini board (seen from the back).
