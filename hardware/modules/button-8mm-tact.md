---
component: 7.8 × 7.8 mm tactile switch with a silicone (grey) round actuator, 2-pin or 4-pin, no LED ("Javino" listing)
image: button-8mm-tact.png (seller's photos and drawings for both versions)
role: Start and Select (no LEDs)
status: read off the seller's drawings
---

# 8 mm tactile switch (Start, Select)

## Read from the drawings
- **Body:** 7.8 × 7.8 mm, 3 mm tall. The **Ø3.5 mm** silicone actuator brings it to **5.5 mm** above the board.
- **4-pin version:**
  - Legs at **8 × 4.5 mm**, recommended holes 4 × Ø1. The legs are bent outward (9.4 mm over the legs).
  - A–B and C–D are the two sides of the switch.
- **2-pin version:**
  - Two straight legs **5.1 mm** apart, 6.7 mm long, recommended holes 2 × Ø1.
  - A–B is the switch.

## Use in this project
- **Pick the 2-pin version.** 5.1 mm is two holes (5.08 mm) on the perfboard, so it drops straight in. The 4-pin's 8 × 4.5 mm would need bending onto 7.62 × 5.08.
- **Placement:** Start and Select on the front, below the stick, centred on a hole column. The legs go one column either side, on one row.
- **Wiring:**
  - One leg goes to its MCP #1 input (GPA4 Start, GPA5 Select, internal pull-up).
  - The other leg goes to GND.
- **Height:** 5.5 mm, against the lit buttons' 7.3 mm. Start and Select sit 1.8 mm lower, which also makes them harder to hit by accident.
- **No LEDs:** MCP #2's GPA4/GPA5 (Start/Select LEDs) become spare. Four lit buttons remain: A, B, X, Y.
