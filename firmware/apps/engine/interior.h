// Inside a building, for every game of the engine: its floors seen from above at a slant, each laid out like the
// building: a room for each of its cells where the game gives it, in the same arrangement, a doorway where two of
// its cells join (or one open hall for a building of one piece), back walls with windows, the door out on the
// ground floor's entry room, stairs up and down in its back corners; furniture the game places, each with a
// footprint the walker cannot cross, drawn with the walker back to front; the view following the walker. Pictures
// from the game's pack (interior_art_t, any of them -1: drawn with plain shapes in the colours given).
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "fart.h"

#define IN_TILE 16                   // a tile on screen
#define IN_WALL_ROWS 3               // a back wall's height in tiles
#define IN_ROOM_W 9                  // a cell's room: tiles across (with the wall on its right)
#define IN_ROOM_H 9                  // tiles deep (with its back wall)
// Along a room's back wall (tiles from its left): the door out (and the stairs down above the ground) at 1, a
// doorway from the room behind at IN_PASS, the stairs up at IN_ROOM_W - 3; doorways to the right at rows 1 and 2.
#define IN_PASS 4
#define IN_GRID 4                    // cells across and deep, at most
#define IN_COLS (IN_GRID * IN_ROOM_W + 2)
#define IN_ROWS (IN_GRID * IN_ROOM_H + 2)
#define IN_ITEMS 40
enum { IN_NONE, IN_EXIT, IN_UP, IN_DOWN };
enum { IN_FLOOR_LIGHT, IN_FLOOR_DARK, IN_WALL, IN_WALL_DARK, IN_TRIM, IN_COLOURS };
enum { IT_VOID, IT_FLOOR, IT_WALL, IT_EDGE, IT_DOOR, IT_UP, IT_DOWN };   // the tiles

typedef struct {
    int floor;                       // a floor tile (16 x 16)
    int wall;                        // a slice of back wall (16 wide, the wall's height), tiled across
    int door;                        // the back wall with the door out (its middle on the door's tiles)
    int window[2];                   // windows hung on back walls (either may be -1)
} interior_art_t;

typedef struct {
    int16_t img;
    float x, y;                      // its top-left (tiles)
    float depth;                     // how deep its footprint is (tiles up from its bottom), 0: walked over (a rug)
} interior_item_t;

// A floor's plan: its cells (bit gx + IN_GRID * gy), which joins between them (a doorway; a bit per cell in join_r
// to its right, in join_d below), the entry room's cell (the door out; -1: none, upstairs; in an open hall moved
// back to the cell on its back wall), and whether it is one open hall.
typedef struct {
    uint16_t cells, join_r, join_d;
    int entry;
    bool open;
} interior_plan_t;

typedef struct {
    int cols, rows;                  // this floor in tiles
    uint8_t tile[IN_ROWS * IN_COLS];
    interior_plan_t plan;
    int floors, floor;
    float x, y;                      // the walker, in tiles (its feet)
    uint16_t col[IN_COLOURS];        // RGB565, for what has no picture
    bool on_stairs;
    int door_x, door_y;              // the door out (tiles; its left), stairs (their left, on the floor's top row)
    int up_x, up_y, down_x, down_y;
    interior_item_t item[IN_ITEMS];
    int nitems;
} interior_t;

// Into a building of `floors` floors: plan for the floor shown (the ground: its door, the walker just inside it).
void interior_enter(interior_t *in, int floors, const interior_plan_t *plan, const uint16_t colours[IN_COLOURS]);
// Another floor, its plan (after IN_UP / IN_DOWN): the walker beside its stairs down (coming up) or up (down).
void interior_floor(interior_t *in, int floor, const interior_plan_t *plan);
// A cell's room on this floor: its top-left tile and size (floor only: below its back wall), false if none.
bool interior_room(const interior_t *in, int gx, int gy, int *x, int *y, int *w, int *h);
// Furniture: an image with its top-left at (x, y) tiles; its footprint the bottom `depth` tiles of it.
void interior_add(interior_t *in, const fart_t *a, int img, float x, float y, float depth);
// A step (dx, dy: tiles) kept off walls and furniture: IN_EXIT into the door, IN_UP / IN_DOWN onto stairs.
int interior_move(interior_t *in, const fart_t *a, float dx, float dy);
// The floor into px (w x h), the view on the walker; the walker drawn by draw_walker (its feet at x, y) in its place
// among the furniture.
void interior_draw(const interior_t *in, const fart_t *a, const interior_art_t *art, uint16_t *px, int w, int h,
                   void (*draw_walker)(uint16_t *px, int w, int h, int x, int y, void *user), void *user);
