---
component: mini Li-ion charger + protection board, USB-C, 20 × 15 × 5 mm ("extreme mini form factor" listing)
image: charger-tp4056-mini.png (seller's photo with dimensions)
role: charges the 18650 and protects it
status: chosen by the user 2026-10-31; read off the seller's photo, not in hand yet
---

# Mini charger + protection board (USB-C)

## Read from the image
- **Size:** 20 × 15 mm, 5 mm tall at the USB-C socket. The board itself is thin.
- **Charger IC:** an SOP-8 chip with an exposed pad, the usual TP4056 footprint. The marking isn't readable.
- **Protection:** a 6-pin SOT-23 chip, most likely a single-chip protector with the MOSFETs inside (the DW01 + 8205 job in one part).
- **LEDs:** two status LEDs (charging and done). A few passives.
- **Pads, as far as the silkscreen shows:**
  - **IN+ / IN−** beside the USB-C.
  - **B+ / B−** for the cell.
  - **OUT+ / OUT−** for the load.
  - The B and OUT pads sit along the edge opposite the socket.
- **USB-C:** power only (no data lines), like every TP4056 board.

## Inferred — needs your confirmation
1. **OUT− is the protected negative.** It sits behind the protection switch, so the load goes on **OUT+/OUT−**, never straight on B+/B−. The protection then covers both over-discharge and short circuits.
2. **Charge current:** 1 A is typical for these boards, set by the RPROG resistor (1.2 kΩ ≈ 1 A, 2.4 kΩ ≈ 500 mA). Read the resistor on arrival. If VIN through the S3 mini board's diode can't carry 1 A, swap RPROG for 2.4 kΩ.
3. **CHRG and STDBY aren't on pads.** To read them on the expander, solder thin wires to the two LED cathodes (or to the IC's pins 7 and 6). The pins are open-drain, so the expander inputs need pull-ups.

## Use in this project
- **Its USB-C is the device's charging port** (power only), at the side edge. The S3 mini board's USB-C also charges: its VIN feeds IN+ through an **1N5819 Schottky**, so neither port can back-feed the other. IN+ is also where the power path takes its USB 5 V.
- **B+/B−:** from the battery holder, through the **reverse-polarity P-MOSFET**.
- **OUT+/OUT−:** into the **power path**. The AO3401 load-sharing switch (with an 1N5819 from USB 5 V) makes VSYS. VSYS feeds the buck-boost and the amp.
- **Status:** the CHRG and STDBY wires go to MCP #2 GPA6/GPA5; its internal pull-ups serve the open-drain outputs.
- **Placement:** on the main board's back, at the bottom right (seen from the back), its USB-C at the side edge (the front view's left edge), right above the battery band, so the cell wires stay short.
