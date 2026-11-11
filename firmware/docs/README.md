# Firmware documents and reference pictures

| File | What |
|---|---|
| [`sdk.md`](sdk.md) | Writing apps and games: how the SDK fits together, the build loop, the API by area, the isometric engine, testing, debugging, rules, known issues and limits, changing the SDK |
| [`users.md`](users.md) | Users: identity, avatars, friends. The design and formats |
| [`sound-and-shared-assets.md`](sound-and-shared-assets.md) | Sound effects and a shared asset pool: the sound pack measured, what the FH1 should get, the plan |
| [`themes.md`](themes.md) | Themes: the palettes, looks and voices, how a framed look is drawn and animated, what motion costs, and how to add a theme |
| [`game-engines.md`](game-engines.md) | Game engines for installable games: how packages can share one, PixelRoot32 findings, ESP-IDF engines compared |
| [`avatar-showcase.html`](avatar-showcase.html) | Open in a browser: the avatar style drawn live at the three sizes, with an editor, the 12 expressions and the faces in FH1 screens. The reference for `components/fh1_avatar` |

## Pictures

| File | What |
|---|---|
| `guition-4848s040.jpg` | This project's Guition ESP32-4848S040 dev board, component side: PCB V1.2, Sparkle IoT XH-S3E module (ESP32-S3, N16R8), touch flex dated 2025.03.19. The 2 x 4 `H1` header is right of the panel connector; its silkscreen reads GND, RXD, TXD, 5V on the outer column. The PCM5102A plugs in here (PLAN.md, "Dev-board audio"). The chip by the `Speak` connector (U4) appears to read NS4168, an I2S amplifier. |

`H1`, component side, `H1` label on top, panel connector to the left:

| Left column | Right column |
|---|---|
| GND | GND |
| GPIO40 | RXD (UART0, the console) |
| GPIO2 | TXD (UART0, the console) |
| GPIO1 | 5V |
