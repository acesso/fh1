---
component: MCP23017 breakout module, 26 × 21 mm (MCP23017-E/SS, SSOP-28, pre-soldered), board marked "280"
image: mcp23017-module-dimensions.png (top, with dimensions), mcp23017-module-pinout.png (pin labels; cropped from the seller's images)
role: the two GPIO expanders (0x20 and 0x21)
status: chosen by the user 2026-10-31 over bare DIP-28 chips; read off the seller's photos, not in hand yet
---

# MCP23017 module (26 × 21 mm)

## Read from the images
- **Size:** 26 × 21 mm.
- **Chip:** an MCP23017-E/SS (SSOP-28) already soldered. It is the I2C part; the "SO/CS/SI/SCK" labels are for the SPI twin, the MCP23S17.
- **Parts:**
  - A 4 × 10 kΩ resistor array (`103`).
  - Two single resistors (`103` and `101`).
  - One capacitor.
- **Holes:** 30 at 2.54 mm, in three rows of 10.
  - **Single row** (one long edge): **A2, A1, A0, RESET, NC/SO, NC/CS, SDA/SI, SCL/SCK, GND, VCC**.
  - **Double row** (the other long edge): the two halves of each label.

| Double-row label | Outer row | Inner row |
|---|---|---|
| VCC/GND | VCC | GND |
| ITB/ITA | INTB | INTA |
| B0/A0 … B7/A7 | GPB0–GPB7 | GPA0–GPA7 |

## Inferred — needs your confirmation
1. **What the resistors do:**
   - The 4 × 10 kΩ array most likely pulls A0–A2 and RESET to a default: the address pins to GND and RESET to VCC. That gives address 0x20 with nothing connected.
   - The `103` single is probably an I2C or interrupt pull-up, the `101` a series resistor. Check with a meter before relying on any of them.
2. **The second module's address:** tie its A0 to VCC to make it 0x21. If the array pulls A0 to GND, that wire then leaks about 0.33 mA through the 10 kΩ, all the time. Find which array resistor serves A0 and lift it, or cut its trace, before tying A0 high.
3. **Extra I2C pull-ups:** if both modules carry SDA/SCL pull-ups, together with the other modules' there may be too many in parallel. Aim for about 2.2–4.7 kΩ total on each line, and remove extras.

## Use in this project
- **Two modules:** #1 at 0x20 (inputs, charger and SD status, stick and SD power), #2 at 0x21 (amp enable, charger status, RTC/IMU interrupts, later the radio's control lines). The allocation is in `architecture.md`.
- **GPA7 and GPB7 are output-only** on current MCP23017 silicon. They carry enables.
- **Interrupts:** INTA and INTB are mirrored and set open-drain. Both modules' INT pins join on one RTC GPIO with a pull-up.
- **Placement:** on the main board's back, upright (21 wide, 26 tall), side by side under the codec and the micro-SD board. Sockets for the 30 pins, or solder the modules straight in.
