// The FH1's isometric engine: a map of cells with heights, built from 32 x 32 block tiles (a 32 x 16 diamond on
// top, 8 pixels of side below it), drawn once into a static layer in PSRAM; units with animations that walk
// from cell to cell; a camera, a cursor, depth order and the scenery in front of a unit drawn again over it.
//
// World pixels: the top-face centre of cell (c, r) at level h is x = (c - r) * 16, y = (c + r) * 8 - h * 8, in the
// view's cell coordinates. In view 0 columns grow toward the screen's bottom right (facing se), rows toward the
// bottom left (facing sw); each view turns the map a quarter further (iso_turn), and everything the engine gives in
// world pixels (iso_world, iso_feet, iso_hand) is in the view shown. Cells, facings and paths stay in map
// coordinates: only the drawing turns. A unit facing d on the map faces (d + view) % 4 on screen.
//
// Scenery animations: a tile's frames (water's ripple), redrawn into the static layer only where a frame changes
// and only on screen. A grade (iso_grade) recolours the scenery's images themselves, so every redraw has it.
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
    uint8_t base;                    // the lowest block's level: above 0 the column floats, nothing under it
    uint8_t bob;                     // 0: still; 1..: bobs up and down with the other cells of its group
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

// How a tile's animation is timed across the map: all cells together, each at its own point (a hash of the
// cell: shimmer), or a wave rolling across the map (half a frame later per cell, down and right).
enum { ISO_TA_SYNC, ISO_TA_SCATTER, ISO_TA_WAVE };
#define ISO_TANIMS 8
typedef struct {
    uint16_t base;                   // the still image a cell names (the animation's first frame)
    uint8_t mode;
    const fart_anim_t *anim;         // its frames: the se facing's
} iso_tanim_t;
// How the view turns: the map spinning a quarter round the cursor (the cached layer drawn turned, units upright,
// moving with their cells), or the picture narrowing to a line and widening in the new view.
enum { ISO_TURN_SPIN, ISO_TURN_NARROW };
#define ISO_SPIN_MS 480

// Changing the scenery while the map is shown (iso_set_obj, iso_set_height, iso_set_top), with a transition:
//   ISO_FX_NONE   at once;
//   ISO_FX_BREAK  what goes shatters into pieces that fly out and fall; what comes is there at once;
//   ISO_FX_DROP   what comes falls from above, lands with a bounce and dust; what goes shatters;
//   ISO_FX_RISE   what comes rises out of the ground, what goes sinks into it (a column rising or sinking whole);
//   ISO_FX_FADE   what goes dissolves away, what comes dissolves in (an ordered dither);
//   ISO_FX_TELEPORT  what goes breaks into ever bigger blocks of colour and vanishes, what comes condenses from
//                 big blocks to its pixels, with sparks rising (a teleport).
// The cell holds its new state at once (paths, reach and units see it); the layer shows it when the transition ends.
enum { ISO_FX_NONE, ISO_FX_BREAK, ISO_FX_DROP, ISO_FX_RISE, ISO_FX_FADE, ISO_FX_TELEPORT };
enum { ISO_EV_START, ISO_EV_LAND, ISO_EV_DONE };        // told to on_change: begun, landed (a drop), finished
#define ISO_CHANGES 8                // at once
#define ISO_PIECES 40                // a change's flying pieces
#define ISO_PARTS 10                 // images a fade or a teleport draws (a column's blocks, objects)
#define ISO_BOBS 32                  // bobbing cells drawn
typedef struct {
    int16_t img, x, y;               // the image and its anchor (world)
    uint8_t mirror, coming;          // coming: it appears (else it goes)
} iso_part_t;
typedef struct {
    int16_t img;                     // the image the piece is cut from
    int8_t x0, y0, w, h;             // its part of the image (from the anchor)
    int16_t ax, ay;                  // the image's anchor at the start (world)
    int16_t vx, vy;                  // pixels a second
} iso_piece_t;
typedef struct {
    int16_t c, r;
    uint8_t fx, what;                // what: 0 the object, 1 the height, 2 the top; 3 a crack, 4 a drop (looks only)
    bool landed;
    uint16_t t, len;                 // ms into it, how long
    uint16_t start;                  // ms before it shows (a crack's, a drop's: staggered)
    uint16_t pieces_at;              // ms when its pieces start flying
    int16_t img;                     // a drop's image
    iso_cell_t from, to, shown;      // before, after, what the layer shows meanwhile
    uint8_t npieces, nparts;
    iso_piece_t pieces[ISO_PIECES];
    iso_part_t parts[ISO_PARTS];
} iso_change_t;
#define ISO_TURN_MS 320              // the narrowing

typedef struct iso_map {
    int cols, rows;
    iso_cell_t *cells;
    fart_t *art;                     // its scenery images may be recoloured (iso_grade)
    uint16_t *layer;                 // the static scenery (PSRAM), lw x lh, its top-left at world (lx, ly)
    int lw, lh, lx, ly;
    uint16_t sky;                    // outside the map
    iso_unit_t units[ISO_UNITS];
    int nunits;
    int camx, camy;                  // the screen's top-left in world pixels
    uint8_t *mark;                   // per cell: ISO_MARK_*, drawn over the ground
    // Told when a unit starts a step (from_h, to_h: the levels it leaves and reaches): footsteps.
    void (*on_step)(const struct iso_map *m, const iso_unit_t *u, int from_h, int to_h);
    uint8_t view;                    // 0..3: quarter turns of the view
    // Scenery animations (iso_tile_anim) and what the layer shows of them.
    iso_tanim_t tanim[ISO_TANIMS];
    int ntanim;
    uint8_t *tanim_of;               // per image: 1 + its animation, 0: none (NULL: no animations)
    uint16_t *acells;                // the cells with an animated part (index r * cols + c)
    uint8_t *aframe;                 // per such cell, 3 bytes: the frame the layer shows of its top, fill and object
    int nacells;
    uint32_t now;                    // the scenery animations' clock (iso_draw's now_ms)
    int16_t col_l, col_r, col_up, col_dn;   // how far scenery images reach from their anchor (layer redraws)
    // Scenery changes under way, and per cell 1 + the one changing it (0: none).
    iso_change_t chg[ISO_CHANGES];
    uint8_t *chg_of;
    int maxh;                        // the highest level the layer has room for
    uint16_t *bcells;                // the bobbing cells (index r * cols + c), drawn each frame, not in the layer
    int nbcells;
    uint16_t *scratch;               // an image unpacked (teleports), ISO_SCRATCH x ISO_SCRATCH, and its mask after it
    uint16_t (*grade_fn)(uint16_t v, void *user);   // the grade, for images a change brings in
    void *grade_user;
    // Told when a scenery change starts, lands and ends (sounds).
    void (*on_change)(const struct iso_map *m, int c, int r, int fx, int event);
    // A turn under way (iso_turn): ms into it (0: none), by how much, the cell kept in place.
    uint16_t turn_ms, turn_len;
    uint8_t turn_style;              // ISO_TURN_SPIN (the default) or ISO_TURN_NARROW
    int8_t turn_by;
    bool turned;
    int16_t turn_c, turn_r;
} iso_map_t;

enum { ISO_MARK_NONE, ISO_MARK_MOVE, ISO_MARK_ACT, ISO_MARK_AREA };

bool iso_init(iso_map_t *m, fart_t *art, int cols, int rows);
void iso_free(iso_map_t *m);
static inline iso_cell_t *iso_cell(iso_map_t *m, int c, int r) { return &m->cells[r * m->cols + c]; }
static inline bool iso_inside(const iso_map_t *m, int c, int r) { return c >= 0 && r >= 0 && c < m->cols && r < m->rows; }
// Draws the static layer again (after changing cells).
bool iso_build_layer(iso_map_t *m);
// Animates a tile: cells naming image base show a's frames (its se facing), timed by mode (ISO_TA_*). Before
// iso_build_layer. false: no room (ISO_TANIMS) or memory.
bool iso_tile_anim(iso_map_t *m, int base, const fart_anim_t *a, int mode);
// Recolours the images the map's cells show (and their animations' frames) through fn, from their original
// colours; fn NULL puts them back (iso_free does). Before iso_build_layer; the art is shared, so a map that grades
// it undoes that when it goes.
void iso_grade(iso_map_t *m, uint16_t (*fn)(uint16_t v, void *user), void *user);
// The view: at once (rebuilding the layer), or turning by +1 or -1 quarter turns round cell (c, r), which stays
// where it is on screen, in the map's turn_style: spinning (ISO_SPIN_MS) or narrowing (ISO_TURN_MS). The layer is
// rebuilt once, half way. While spinning, marks, the cursor and the scenery drawn over units wait.
bool iso_set_view(iso_map_t *m, int view);
void iso_turn(iso_map_t *m, int by, int c, int r);
static inline bool iso_turning(const iso_map_t *m) { return m->turn_ms != 0; }
// Checks the layer under the camera (w x h) against one built from scratch: how many pixels differ (0: as a full
// build would have it), and their box in screen pixels. For tests: what changes left in the layer.
int iso_check_layer(iso_map_t *m, int w, int h, int box[4]);
// Draws every animated cell again into the layer (each frame change does this for the cells on screen); for
// measuring the worst case.
void iso_redraw_animated(iso_map_t *m);
// Scenery changes (see ISO_FX_*). The object on a cell (ISO_NONE: none), the height of its column (its top block
// and object ride up or down with it), its top block's image. false: the cell is changing already, no room
// (ISO_CHANGES), the view is turning, or nothing would change. A turn waits until no change runs.
bool iso_set_obj(iso_map_t *m, int c, int r, int img, int fx);
bool iso_set_height(iso_map_t *m, int c, int r, int h, int fx);
bool iso_set_top(iso_map_t *m, int c, int r, int img, int fx);
static inline bool iso_changing(const iso_map_t *m, int c, int r) { return m->chg_of && m->chg_of[r * m->cols + c]; }
// A cell floating (its lowest block at level base; 0: on the ground) and bobbing with group bob (0: still), at once.
// Units standing on a bobbing cell bob with it.
bool iso_set_float(iso_map_t *m, int c, int r, int base, int bob);
// Looks only, the map unchanged: a cell's top cracking (its pieces fly, the tile stays; power 1..3 throws them
// further), and an image dropped onto a cell's top from above, shattering there (delay: ms before it falls). Both
// tell on_change ISO_EV_LAND when they hit (a spell's damage), ISO_EV_DONE at the end.
bool iso_crack(iso_map_t *m, int c, int r, int power, int delay_ms);
bool iso_drop_on(iso_map_t *m, int c, int r, int img, int delay_ms);
// A facing on screen (the D-pad's: FART_NE is Up) as a facing on the map, and the other way round.
static inline int iso_map_dir(const iso_map_t *m, int screen_dir) { return (screen_dir + 4 - m->view) % 4; }
static inline int iso_screen_dir(const iso_map_t *m, int dir) { return (dir + m->view) % 4; }

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
