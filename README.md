# FH1

A modular, pocket-sized handheld built on an ESP32-S3, hand-wired on two 12 × 8 cm perfboards. It runs "apps": Game Boy Color and NES emulators, a hand-made virtual pet, and later anything that fits (network tools, a camera, an LTE/3G link).

It borrows its building style from the ESP32-S3 Wi-Fi bridge in `../Signal-Decoder/hardware/bridge-s3`: modules on 2.54 mm round-pin sockets, so each one can be swapped or reworked.

## Goals

- **Pocket size:** two 120 × 80 mm perfboards in a sandwich, one 18650 cell.
- **Colour screen:** 2.8" IPS, 320 × 240, fast enough for 60 fps Game Boy Color emulation.
- **Controls:** a PSP-style thumbstick and six lit buttons in the SNES layout (A, B, X, Y, Start, Select), plus Menu. L/R later. No touch screen.
- **Audio:** a mono speaker and a microphone, voice quality.
- **Battery life first:** hours of play, days of virtual-pet standby.
- **Storage and updates:** micro-SD for ROMs and data; firmware and app updates over the air.
- **Later:** a camera, and LTE/3G as an app and as a network layer.

## Status

Hardware planning, parts being ordered for **two devices**. Parts chosen and placement drawn (tag `hw-plan-1`); wiring in progress. Nothing soldered yet, no firmware.

## Layout

| Path | What |
|---|---|
| `hardware/requirements.md` | Features turned into hardware requirements, with priorities and phases |
| `hardware/architecture.md` | Block diagram, buses, power tree, GPIO budget, power budget |
| `hardware/parts.md` | Parts list: what is reused from the bridge stock, what to buy, candidates |
| `hardware/layout.md` | Mechanical plan: the two boards, what goes on each face |
| `hardware/wiring.md` | GPIO map, MCP23017 pin allocation, power rails, routing rules |
| `hardware/layout/board-render.html` | The layout drawn: both faces of both boards, the side section, parts as placed (open in a browser) |
| `hardware/layout/concept-v1-*.png` | Conceptual 3D renders of the V1 build (front, laid open, back), made by `concept-render.html` from the layout's positions and wires (open it in a browser; buttons switch the view) |
| `hardware/modules/` | Module notes: dimensions and pinouts read off the sellers' drawings |
| `hardware/open-questions.md` | Decisions waiting for the user |
