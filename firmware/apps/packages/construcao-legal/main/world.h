// The world of Construção Legal: a fixed map of cells (ground, a thing on it, who may change it), made the same
// every time, drawn isometric from the art pack through a view cache that scrolls with the camera.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "fart.h"
#include "flag.h"

#define WORLD_W 128
#define WORLD_H 128
// A tile on screen: its top face 48 x 28 (the tileset's own proportion, about 1.7 : 1, at half its size).
#define TW 48
#define TH 28

// Ground kinds; roads and rivers pick their piece from their neighbours (autotile), a bridge is a road over a river.
enum { G_GRASS, G_DIRT, G_WHEAT, G_CORN, G_ROAD, G_RIVER, G_BRIDGE };
// Things on a cell (one at most).
enum { O_NONE, O_TUFT, O_TREE, O_STONE, O_HOUSE, O_SMITH, O_MILL, O_WALL, O_TOWER, O_STAIR };
// Cell flags.
enum {
    F_TOWN = 1,                      // part of the village: nobody may change it (the player builds elsewhere)
    F_BUILT = 2,                     // built by the player (theirs to change)
};

typedef struct {
    uint8_t ground, var;             // its kind and variant (the image's choice)
    uint8_t obj, flags;              // O_*, F_*
    uint16_t img, obj_img;           // the images drawn (the ground's resolved piece; FART_NONE: nothing)
    uint8_t ovar;                    // the thing's variant: a tree's kind, a house's facing (bits 0-1), roof (2-3), walls (4);
                                     // a smithy's facing (0), roof (2-3); a mill's roof (0-1), sails' side (2);
                                     // a wall's kind (0-1: WALL_*), its way when alone (2: along rows / columns);
                                     // a tower's style (0-3: 0..8), colour (4-5); stairs: the side they climb
                                     // toward (0 N, 1 E, 2 S, 3 W), a wall there
} cell_t;

enum { WALL_PLAIN, WALL_WINDOW, WALL_GATE };
// A castle wall's stack (its var): bits 0-1 walls on top of the first (0..2), 2 a tower on top, 3-6 the tower's
// style (0..8; its colour in the ovar's bits 3-4), 7 windows in the walls on top. Three levels at most.
#define STACK_UP(x) ((x)->var & 3)
#define STACK_TOWER(x) (((x)->var >> 2) & 1)
#define STACK_STYLE(x) (((x)->var >> 3) & 15)
#define STACK_WIN(x) ((x)->var >> 7)
#define STACK_LEVELS(x) (1 + STACK_UP(x) + STACK_TOWER(x))
#define STACK_MAX 3
#define STACK_DY 25                  // a level's height on screen: a wall's ("castle_h"; the levels under the top
                                     // have no parapet)
// Buildings joined into one: a house's or smithy's cell keeps the sides it is joined on in its var: bits 0-3 N, E, S,
// W, JOINED (bit 4: kept this way); UPPER (bit 5) a 2 x 2 house's upper storey.
#define JOINED 0x10
#define UPPER 0x20
#define GROUP_MAX 64                 // the cells of one building, at most
#define KEEP_H 34                    // a keep's height (fh1model.py's "keep_h"), where a tower stands on it
#define CHIMNEY_TOP 38               // a smithy's chimney top above its cell's middle (fh1model.py's "chimney")

// A change to the world (the player's), kept by kind and variant, not by image: a save survives new art.
typedef struct { uint8_t c, r, ground, var, obj, ovar, flags; } edit_t;
#define EDITS_MAX 2048

typedef struct {
    fart_t *art;
    cell_t *cells;                   // WORLD_W x WORLD_H, row by row
    uint16_t sky;
    // The view cache: the world's picture round the camera (CW x CH pixels at world pixel cx0, cy0).
    uint16_t *cache;
    int cw, ch, cx0, cy0;
    bool cache_ok;
    edit_t *edits;                   // the player's changes, one per cell changed (the save)
    int nedits;
    // Moving things: a mill's sails (8 frames a quarter turn, each side), the smithy's smoke (3 sizes).
    uint16_t sails[2][8], smoke[3];
    uint16_t towers[4][9];           // by colour and style (stacked on walls)
    int view;                        // 0..3 (world_view)
    uint16_t keep;                   // a castle's 2 x 2 keep
    uint16_t pal_key[4], pal[8][4];  // buildings' colours: the reference (walls light, dark; roof light, dark), and
    bool pal_ok;                     // each walls (wood, white) x roof (blue, green, red, wood)
    int8_t part_pal[6];              // the palette of each part thing_parts gave last (-1: none)
    flag_t flag;                     // the flag on towers that stand on walls (the engine's flag.h)
    uint16_t flag_col[4][2];         // in the towers' colours
    uint32_t t;
    int sail_frame;
    // Each change as it is kept (the newest state of its cell), as it happens: a network host passes them on.
    void (*on_edit)(const edit_t *e, void *user);
    void *on_edit_user;
} world_t;

bool world_init(world_t *w, fart_t *art);
void world_free(world_t *w);
// The fixed world: grass and forests, a river, roads, a small village; the player's start.
void world_make(world_t *w, float *start_u, float *start_v);
// Back to the fixed world, no changes (in the view it has).
void world_reset(world_t *w, float *start_u, float *start_v);
static inline cell_t *world_cell(world_t *w, int c, int r) { return &w->cells[r * WORLD_W + c]; }
static inline bool world_inside(int c, int r) { return c >= 0 && r >= 0 && c < WORLD_W && r < WORLD_H; }
// The ground's piece again for a cell and its neighbours (after a road or river changes), and the cache there.
void world_retile(world_t *w, int c, int r);
// Can one stand at (u, v) (cell units, continuous)? Water blocks; a bridge carries; trees and stones block nearly
// all their cell, buildings its middle (a lane round each).
bool world_walkable(world_t *w, float u, float v);
// How high one stands at (u, v) (pixels; z: where one is now, which decides between a gate's passage and the
// walkway over it): the ground 0; on stairs, part of a wall's height by how far up them; on a castle wall of one
// level, its walkway (reached only from stairs or another wall's top); -1 where one cannot stand.
float world_height(world_t *w, float u, float v, float z);
#define WALK_H STACK_DY              // a wall's walkway above the ground
// The image of a thing by kind and variant (FART_NONE: none).
int world_obj_image(world_t *w, int obj, int ovar);
// Changes a cell (its ground, the thing on it; built: the player's now, or back to nobody's), redraws round it and
// keeps the change for the save; false when the change list is full.
bool world_set(world_t *w, int c, int r, int ground, int var, int obj, int ovar, bool built);
// Joins the new building at (c, r) with what stands beside it, if it can (houses of any colours, a smithy with
// wooden houses), of any shape and size: those already joined keep their joins. Each cell changed is recorded as
// world_set does.
void world_merge(world_t *w, int c, int r);
// A 2 x 2 keep of castle walls / a 2 x 2 house of one kind there?
bool world_is_keep(world_t *w, int c, int r);
bool world_is_big(world_t *w, int c, int r);
// A building one can go into: what it is, its floors, its colours (a house's walls and roof), its cells and how they
// join (each one's joined sides: N 1, E 2, S 4, W 8), and whether it is of one piece (a 2 x 2 house, a keep: one
// hall inside).
enum { B_HOUSE, B_SMITHY, B_BIG, B_KEEP };
typedef struct {
    int kind, floors, walls, roof, n;
    int16_t c[GROUP_MAX], r[GROUP_MAX];
    uint8_t joins[GROUP_MAX];
    bool open;
} building_t;
// The building (c, r) is part of, any of its cells, on the ground: false if none to go into there.
bool world_building(world_t *w, int c, int r, building_t *b);
// After a tower is put on a keep's cell or an upper storey on a 2 x 2 house's: the same on its other three cells.
void world_spread(world_t *w, int c, int r);
// Undoes (c, r)'s joins (before it is removed): the rest of its building keeps theirs.
void world_unjoin(world_t *w, int c, int r);
// A gate on the road at (c, r): the way its wall must run (0 along rows, 1 along columns) for the road to go
// straight through it (and the walls beside it to join that way); -1: none fits.
int world_gate_way(world_t *w, int c, int r);
// The cells of the building (c, r) is part of (itself first; 1 when alone): its pieces' places.
int world_group(world_t *w, int c, int r, int *gc, int *gr);
// The changes again, from a save: applied without being drawn (before the first frame).
void world_apply(world_t *w, const edit_t *e, int n);

// A point of the ground (cell units) on screen, relative to the world's origin (world pixels), as the view turns it.
void world_px(float u, float v, float *x, float *y);
// A point turned to the screen's columns and rows (the view), and back.
void world_to_screen(float u, float v, float *su, float *sv);
void world_from_screen(float su, float sv, float *u, float *v);
// The same in a view given (another player's, who sees the world turned their own way).
void world_to_screen_v(int view, float u, float v, float *su, float *sv);
void world_from_screen_v(int view, float su, float sv, float *u, float *v);
// Turns the view (0..3: a quarter each, clockwise): every piece chosen again, the cache drawn again.
void world_view(world_t *w, int view);

// Draws the world's view at camera (world pixels of the screen's top-left) into px (w x h): the cache, scrolled
// and filled as needed, copied out.
void world_draw(world_t *w, uint16_t *px, int pw, int ph, int camx, int camy);
// The things on cells in front of a point of depth d (u + v), redrawn over a box of the frame (a unit behind them),
// then the cutaway: the box's earlier pixels given back through them in an ellipse, every other pixel.
void world_cover(world_t *w, uint16_t *px, int pw, int ph, int camx, int camy, float u, float v,
                 const fart_clip_t *box);
// Time for what moves: the mills' sails turn (their part of the cache drawn again when the frame changes).
void world_tick(world_t *w, uint32_t dt);
// A change not made yet, see-through over the frame (alpha 0..255; its colours moved toward tint by tint_a): the
// ground's piece as it would join its neighbours, the thing (a mill with its sails).
void world_ghost(world_t *w, uint16_t *px, int pw, int ph, int camx, int camy, int c, int r, int ground, int var,
                 int obj, int ovar, int alpha, uint16_t tint, int tint_a);
// Smoke from the smithies' chimneys in view, over the frame.
void world_smoke(world_t *w, uint16_t *px, int pw, int ph, int camx, int camy);
