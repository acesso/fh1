// The FH1's isometric engine: a map of cells with heights, built from 32 x 32 block tiles (a 32 x 16 diamond on
// top, 8 pixels of side below it), drawn once into a static layer in PSRAM; units with animations that walk
// from cell to cell; a camera, a cursor, depth order and the scenery in front of a unit drawn again over it.
//
// World pixels: the top-face centre of cell (c, r) at level h is x = (c - r) * 16, y = (c + r) * 8 - h * 8.
// Columns grow toward the screen's bottom right (facing se), rows toward the bottom left (facing sw).
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "fart.h"

#define ISO_HW 16                    // half a tile's width
#define ISO_HH 8                     // half its top face's height
#define ISO_STEP 8                   // one level of height
#define ISO_NONE 0xFFFF

typedef struct {
    uint8_t h;                       // the top's level (0: the lowest block only)
    uint16_t top, fill;              // block images: the top block, the ones under it
    uint16_t obj;                    // an object standing on it (drawn in the slot one level up), or ISO_NONE
} iso_cell_t;

#define ISO_PATH 32

typedef struct {
    const fart_anim_t *walk, *idle;
    int c, r;                        // the cell it stands on (the one it walks to, while walking)
    int fc, fr;                      // the cell it walks from
    int dir;                         // FART_SE...
    uint32_t t;                      // its animation clock
    uint32_t step;                   // ms into the current step; 0 when standing
    uint8_t path[ISO_PATH];          // facings still to walk
    uint8_t npath, ipath;
    const fart_anim_t *once;         // a one-shot animation playing (an attack), or NULL
    uint32_t once_t;
    bool hold;                       // the one-shot stays on its last frame (knocked out)
    bool dim;                        // drawn darker (done for this turn)
    bool still;                      // its idle stopped on its first frame (knocked out: it lies still)
    uint8_t fade;                    // knocked out: its colour draining (0..230, rising while still)
    bool hidden;
    uint32_t flash;                  // ms of white flash left (hit)
    // What it wears and holds, and its pose (set by the game; the engine draws them).
    int16_t wpn;                     // the weapon's first rotation image (16 in a row, k pointing at k * 22.5 degrees), -1
    int16_t wpn_rest;                // the weapon's angle at rest, facing right (degrees; mirrored facing left)
    int16_t wpn_angle;               // ISO_POSE_REST, or the angle now (an attack, a cast), already facing the right way
    int8_t hand_dx, hand_dy;         // the hand moved from where the frame has it (raised to cast or throw)
    int8_t lunge_x, lunge_y;         // the whole unit moved a little (an attack)
    uint8_t cover;                   // armour: 1 << FART_PART_* it covers
    const uint16_t *ramp;            // the armour's 4 shades
    const uint16_t *shade;           // the whole unit in one colour (fart_style_t), or NULL
    const uint16_t *wpn_shade;       // the weapon in one colour (an elemental weapon), or NULL
    uint8_t zoom;                    // 2: drawn twice the size (a boss)
    bool falling;                    // knocked out without an animation for it: it falls over and stays down
    int16_t tilt;                    // how far over it is (degrees, 90: lying)
    bool counter;                    // ready to strike back (drawn with a mark)
} iso_unit_t;
#define ISO_POSE_REST (-1000)
// The rotation image for an angle (degrees).
static inline int iso_rot(int base, int deg) { return base + (((deg % 360 + 360) * 16 + 180) / 360) % 16; }

#define ISO_UNITS 16

typedef struct iso_map {
    int cols, rows;
    iso_cell_t *cells;
    const fart_t *art;
    uint16_t *layer;                 // the static scenery (PSRAM), lw x lh, its top-left at world (lx, ly)
    int lw, lh, lx, ly;
    uint16_t sky;                    // outside the map
    iso_unit_t units[ISO_UNITS];
    int nunits;
    int camx, camy;                  // the screen's top-left in world pixels
    uint8_t *mark;                   // per cell: ISO_MARK_*, drawn over the ground
    // Told when a unit starts a step (from_h, to_h: the levels it leaves and reaches): footsteps.
    void (*on_step)(const struct iso_map *m, const iso_unit_t *u, int from_h, int to_h);
} iso_map_t;

enum { ISO_MARK_NONE, ISO_MARK_MOVE, ISO_MARK_ACT, ISO_MARK_AREA };

bool iso_init(iso_map_t *m, const fart_t *art, int cols, int rows);
void iso_free(iso_map_t *m);
static inline iso_cell_t *iso_cell(iso_map_t *m, int c, int r) { return &m->cells[r * m->cols + c]; }
static inline bool iso_inside(const iso_map_t *m, int c, int r) { return c >= 0 && r >= 0 && c < m->cols && r < m->rows; }
// Draws the static layer again (after changing cells).
bool iso_build_layer(iso_map_t *m);

iso_unit_t *iso_add_unit(iso_map_t *m, const fart_anim_t *walk, const fart_anim_t *idle, int c, int r, int dir);
// Starts a step to a neighbouring cell (one of the four facings); false if one is under way or it is outside.
bool iso_step(iso_map_t *m, iso_unit_t *u, int dir);
void iso_update(iso_map_t *m, uint32_t dt_ms);
// Walks a list of facings, one cell each.
void iso_walk(iso_unit_t *u, const uint8_t *dirs, int n);
static inline bool iso_busy(const iso_unit_t *u)
{
    return u->step || u->ipath < u->npath || (u->once && !u->hold) || (u->falling && u->tilt != 90 && u->tilt != -90);
}
// Falls over (once, about 0.4 s) and stays lying; iso_rise stands it up again (its colour back too).
void iso_fall(iso_unit_t *u);
void iso_rise(iso_unit_t *u);
// Plays an animation once (hold: stay on its last frame).
void iso_play(iso_unit_t *u, const fart_anim_t *a, bool hold);
// Cells a unit can walk to: at most range steps, climbing or dropping at most jump levels a step, not into
// water (level 0) or through other units. dist[cell] = steps (255: no); from[cell] = the facing that came in.
void iso_reach(iso_map_t *m, const iso_unit_t *u, int range, int jump, uint8_t *dist, uint8_t *from);
// The facings from the unit to (c, r) found by iso_reach; returns how many (0: unreachable).
int iso_path_to(iso_map_t *m, const iso_unit_t *u, const uint8_t *dist, const uint8_t *from, int c, int r, uint8_t *out);
iso_unit_t *iso_unit_at(iso_map_t *m, int c, int r);

// Where a unit's hand is now (world pixels), for a weapon's tip, a thrown item or a projectile's start.
void iso_hand(iso_map_t *m, const iso_unit_t *u, int *x, int *y);
// Where a unit's feet are now (world pixels).
void iso_feet(iso_map_t *m, const iso_unit_t *u, int *x, int *y);
// The world position of a cell's top-face centre.
void iso_world(const iso_map_t *m, int c, int r, int *x, int *y);
// Moves the camera smoothly so (wx, wy) is near the screen's middle.
void iso_follow(iso_map_t *m, int wx, int wy, int w, int h, uint32_t dt_ms);
// Draws the scene: the static layer, the cursor cell (cc, cr; -1: none), the units in depth order.
void iso_draw(iso_map_t *m, uint16_t *px, int w, int h, int cc, int cr, uint32_t now_ms);
