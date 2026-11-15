---
component: PB86 push button, 12.35 × 17.4 mm, SPDT momentary, with an LED in the cap, straight PCB mount
image: button-pb86.png (the seller's drawing: views, PCB layout, circuit diagram, specifications)
role: the L and R buttons, on the second board's outer face, pressed from behind
status: read off the seller's drawing
---

# PB86 push button (L and R)

## Read from the drawing
- **Body:** 12.35 × 17.4 mm footprint. The cap face is 12.35 × 17 mm, with a round LED window near one end.
- **Height:** 11.1 + 4.6 = about 15.7 mm from the board to the cap's face, on the side view.
- **Travel:** 2.0 ± 0.4 mm; operating force 170 ± 50 gf.
- **Switch:** SPDT, momentary. Two pins 1 (the common, joined inside), pin 2 normally closed, pin 3 normally open.
- **LED:** pins 5 (+) and 6 (−); pins 4 and 7 are drawn as open circles (no connection).
- **Rating:** 12 mA, 24 V DC; contact resistance 50 mΩ max; 50,000 cycles mechanical.
- **PCB layout** (as drawn, from the component side):
  - Top row: pins 4, 5, 6, 7, 2.54 mm apart (7.62 mm across), Ø0.7 holes.
  - 5.08 mm below: the two pins 1, under pins 4 and 7, Ø0.9 holes.
  - 5.08 mm below that: pin 2 under the left pin 1, pin 3 under the right one, Ø0.9 holes.
  - Two Ø1.1 holes for plastic pegs, near the centre line.
- **Materials:** PBT base, ABS cap, gold-plated phosphor bronze terminals.

## Inferred — needs your confirmation
1. **Every pin is on the 2.54 mm grid** (7.62 = 3 steps, 5.08 = 2 steps), so it sits on the perfboard as it is. The pegs (Ø1.1) need their own holes: measure where they are and drill, or clip them off.
2. **The height:** read as 15.7 mm off the board; measure one.
3. **Which way the pins read from the solder side:** the layout is taken as drawn from the component side. Check pin 3 with a meter before soldering: pins 1–3 close when pressed.

## Use in this project
- **Two of them, L and R**, on the second board's **outer face** (the device's back), one on each side of the centred speaker, their tops level with its top, pressed from behind by the fingers holding the device. Positions and wiring: `../wiring.md` ("Second board wiring") and `../layout/board-render.html`.
- **Wiring:** pin 3 (normally open) to the expander input, pull-up on; one pin 1 to GND. Pressed reads low. Pin 2 stays unconnected.
- **The LED is not wired:** a light on the back of the device isn't seen. Pins 4–7 are soldered for strength only and left floating.
