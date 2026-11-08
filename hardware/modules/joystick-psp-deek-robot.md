---
component: PSP-style analog thumbstick on a breakout, Deek-Robot "JOYSTICK-02" (ZC Technology listing), 25.8 × 25.8 mm
image: joystick-psp-deek-robot.png (seller's photos and description)
role: the 2-axis stick (D-pad replacement)
status: read off the seller's photos
---

# PSP-style thumbstick (Deek-Robot JOYSTICK-02)

## Read from the images
- **Board:** 25.8 × 25.8 mm, blue, with four corner mounting holes.
- **Stick:** a PSP-type self-centring analog stick in a square metal frame, with a black rubber cap.
- **Electrical:** two 10 kΩ potentiometers, X and Y. The seller says to power VCC from 3 or 5 V and read X and Y on analog pins.
- **Header:** 4 pins, silkscreen **GND, X, Y, VCC**. The pins are right-angle and come already soldered, pointing out along the board's plane.
- **No push switch:** there is no SEL/click pin. This stick does not press in.

## Inferred — needs your confirmation
1. **Height:** not given. PSP sticks stand roughly 5–6 mm above their mount, and the board adds 1.6 mm. Measure on arrival.
2. **Swing:** the travel uses only the middle of the range. On a real PSP stick that is roughly 15–85 % of VCC. The firmware calibrates the centre, the limits and a dead zone.

## Use in this project
- **Mounting:** on the main board's front face, left of the diamond, on four M2 screws or spacers through its corner holes.
  - **The right-angle header has to go:** desolder it and fit **straight** pins pointing down through the main board, or solder 4 short wires.
- **Power:** VCC from 3V3 through the stick-power switch on the first expander, so the pots (0.33 mA each) draw nothing when the device sleeps.
  - Read X and Y on two **ADC1** pins (1–10) with 12 dB attenuation, which covers about 0–3.1 V. The few % at the top end clip, which doesn't matter with this stick's range.
- **No click:** the expander input reserved for "stick click" is now spare. The six buttons and Menu cover everything the emulators need.
