// Portals for every game of the engine: a vortex whirling (frames made by fh1art.py's "whirl" on a part, in its
// own reference colours), drawn in any colour (its shades keep their brightness), opening by growing from its
// middle and closing back into it, with a few motes of light drifting round it in its colours. The frame round it
// (pillars, an arch) is the game's scenery; the game draws the portal where it stands, at its depth.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "fart.h"

#define PORTAL_KEYS 16
#define PORTAL_FRAMES 16
#define PORTAL_MOTES 6
enum { PORTAL_CLOSED, PORTAL_OPENING, PORTAL_OPEN, PORTAL_CLOSING };

typedef struct {
    float x, y, vx, vy;              // from the vortex's middle (pixels), drifting
    uint16_t life, age;              // ms
    uint8_t shade;                   // which of the colours
} portal_mote_t;

typedef struct {
    uint16_t frame[PORTAL_FRAMES];   // the vortex's frames (the pack's)
    int n;
    uint16_t ms;                     // a frame
    uint16_t key[PORTAL_KEYS], col[PORTAL_KEYS];   // its reference colours (dark to light) and the ones drawn
    int nkey;
    int state;
    uint32_t t, st_t;                // its clock; ms into opening or closing
    uint16_t open_ms;                // how long opening (and closing) takes
    portal_mote_t mote[PORTAL_MOTES];
    uint32_t seed;
    int glow;                        // the light it casts on its frame (an image, in its colours), or -1
} portal_t;

// The portal whose vortex is the pack's animation `anim` (fh1art.py "whirl" frames): false if the pack has none.
// It starts closed, in its own colours.
bool portal_load(portal_t *p, const fart_t *a, const char *anim);
// Its colour (0xRRGGBB): the vortex's shades, dark to light, spread from a dark tone of it through it to a pale one.
void portal_colour(portal_t *p, uint32_t rgb);
// The light it casts on its frame (fh1art.py "cut" "glow": in the vortex's colours), drawn while it is open.
void portal_glow(portal_t *p, int img);
void portal_open(portal_t *p);       // grows from its middle
void portal_close(portal_t *p);      // shrinks into it
bool portal_shown(const portal_t *p);
void portal_update(portal_t *p, uint32_t dt);
// Draws it with the frame's anchor at (x, y) (the vortex image is anchored like the whole portal picture), mirrored
// if asked (the view turned); the motes round it.
void portal_draw(const portal_t *p, const fart_t *a, uint16_t *dst, int stride, const fart_clip_t *clip, int x, int y,
                 bool mirror);
