---
component: ES8311 + NS4150B audio module, 26.1 × 17 mm, MIC and SPK connectors, 10-pin header
image: audio-es8311-ns4150b.png (board with dimensions), audio-es8311-ns4150b-kit.png (kit: board, header, electret mic on a lead)
role: codec (speaker out, mic in) and speaker amp
status: chosen by the user 2026-10-31, with the external electret mic; read off the seller's photos, not in hand yet
---

# ES8311 + NS4150B audio module

## Read from the images
- **Size:** 26.1 × 17 mm, black, with one mounting hole (top right).
- **Chips:** an **ES8311** codec (QFN, labelled) and an **NS4150B** class-D amp (MSOP-8, labelled "NS4150B").
- **Connectors:**
  - **MIC:** a 2-pin socket on the left edge, with D1/D2 and R3/R6 beside it (the mic bias and input network).
  - **SPK:** a 2-pin socket on the right edge.
- **Header:** 10 pins at 2.54 mm: **SDA, SCL, MCK, BCK, DI, WS, DO, 3V3, VCC, GND**.
- **PA_EN:** printed beside R8 near the top edge. It is **not on the header**.
- **Kit:** the board, a straight 10-pin header, and an **electret microphone capsule on a twisted lead** with a plug for the MIC socket.

## Inferred — needs your confirmation
1. **DI and DO direction:**
   - **DI** = data into the codec (the DAC path) = the S3's I2S data **out**.
   - **DO** = data out of the codec (the ADC/mic path) = the S3's I2S data **in**.
   - Check against the silkscreen or the schematic before wiring.
2. **Supplies:** **3V3** feeds the ES8311, and **VCC** the NS4150B amp. The amp runs from about 3.0 to 5.5 V, so VSYS (3.0–4.2 V) works. Confirm that VCC isn't tied to 3V3 on the board.
3. **PA_EN:** most likely the amp's enable, pulled on by R8, so the amp is always on. The NS4150B idles at a few mA. To switch it off between sounds, lift R8 (or cut its trace) and run a wire from the PA_EN point to the second expander's amp-enable output.
4. **I2C address:** 0x18 (the ES8311's CE pin low), the common setting. Check it with an I2C scan.
5. **The "on-board mic option"** the listing mentions isn't visible in the photo. Probably a footprint left unpopulated. Not needed: the external capsule is the plan.

## Use in this project
- **I2S** (shared BCLK and WS):
  - BCK = BCLK.
  - WS = word select.
  - DI = from the S3's DOUT.
  - DO = to the S3's DIN.
  - MCK = MCLK from the S3, on its own GPIO.
  - **Pin saver:** the ES8311 can also make its internal clock from BCK (its MCLK-from-SCLK setting). If that works cleanly, MCK can stay unconnected and the GPIO goes back to the pool. Keep the pin budgeted until it is tested.
- **I2C:** SDA/SCL on the shared bus.
- **Power:**
  - 3V3 from the buck-boost.
  - **VCC from VSYS**, the battery side, as decided: louder, and the amp's current stays off the 3.3 V rail.
- **Speaker:** the 36 × 25 mm 8 Ω speaker plugs into SPK. Check that the plugs mate.
- **Mic:** the external **electret capsule** on its lead, placed wherever the case wants it, with a small hole in front of it.
  - It is analog. Route the lead away from the display's SPI clock, the buck-boost and the antenna cables, and keep it twisted.
  - Seal the capsule against the case with a foam or rubber gasket, so it hears the room, not the inside of the device.
- **Placement:** on the main board's back, upright, between the S3 mini board and the micro-SD board (seen from the back).
