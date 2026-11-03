---
component: rectangular enclosed speaker, 36 × 25 × 6.5 mm, sold as 4 Ω 3 W or 8 Ω 2 W, twisted leads with a small 2-pin plug
image: speaker-36x25.png (seller's photo with dimensions)
role: the handheld's speaker
status: chosen by the user 2026-10-31, **8 Ω 2 W** variant (recommended)
---

# 36 × 25 mm speaker

## Read from the image and listing
- **Size:** 36 × 25 × 6.5 mm.
- **Build:** a closed box with a black oval cone, a white front face (adhesive gasket) and a black back.
- **Leads:** twisted red/black wires, about 15–20 cm, ending in a small white 2-pin plug (JST-type, probably 1.25 mm or PH 2.0 pitch).
- **Variants:** 4 Ω 3 W or 8 Ω 2 W.

## Why 8 Ω
- **Loudness:** the NS4150B runs from VSYS (3.0–4.2 V). Its bridged output gives at most about V²/(2R):
  - 8 Ω: about **0.85 W** at a 3.7 V cell.
  - 4 Ω: about **1.7 W** at a 3.7 V cell.
  - 0.85 W from a speaker this size is plenty for a handheld.
- **Current:** the 4 Ω load pulls twice the current, about 1 A peaks against 0.5 A. That costs battery, and it can sag VSYS on loud passages while Wi-Fi is transmitting.
- **Headroom:** the 8 Ω part is rated 2 W, more than twice what the amp can give it, so it never runs near its limit.
- **Fallback:** if it turns out too quiet in the case, the 4 Ω part is a direct swap.

## Inferred — needs your confirmation
1. **The plug:** check that it mates with the ES8311 + NS4150B module's speaker connector. If it doesn't, cut it off and solder the leads, or crimp the module's plug on.

## Use in this project
- **Placement:** between the boards, on the second board's inner face, hanging into the 25 mm gap. 6.5 mm thick leaves plenty of room.
- **Sound path:** it fires out through a field of drilled holes in the second board to a grille in the back cover.
- **Mounting:** stick its adhesive front gasket to the board, so it seals around the holes. The closed back box means it doesn't need the gap as back volume.
