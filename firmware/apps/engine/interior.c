#include "interior.h"
#include <string.h>
#include <math.h>

#define T(in, x, y) ((in)->tile[(y) * IN_COLS + (x)])

static bool has(const interior_plan_t *p, int gx, int gy)
{
    return gx >= 0 && gy >= 0 && gx < IN_GRID && gy < IN_GRID && (p->cells >> (gx + IN_GRID * gy) & 1);
}

// The floor's tiles from its plan: each cell's room (its back wall, its floor), the walls round them, doorways
// where cells join (or none at all between them: one open hall), the door out and the stairs.
static void lay_out(interior_t *in)
{
    const interior_plan_t *p = &in->plan;
    memset(in->tile, IT_VOID, sizeof(in->tile));
    int gw = 0, gh = 0;
    for (int g = 0; g < IN_GRID * IN_GRID; g++)
        if (p->cells >> g & 1) {
            if (g % IN_GRID + 1 > gw) gw = g % IN_GRID + 1;
            if (g / IN_GRID + 1 > gh) gh = g / IN_GRID + 1;
        }
    in->cols = gw * IN_ROOM_W + 2;
    in->rows = gh * IN_ROOM_H + 2;
    for (int gy = 0; gy < gh; gy++)
        for (int gx = 0; gx < gw; gx++) {
            if (!has(p, gx, gy)) continue;
            const int x0 = 1 + gx * IN_ROOM_W, y0 = 1 + gy * IN_ROOM_H, g = gx + IN_GRID * gy;
            // Open hall: a back wall only along the top of the building, no walls between its cells.
            const bool back = !(p->open && has(p, gx, gy - 1));
            const bool right = !(p->open && has(p, gx + 1, gy));
            for (int y = y0; y < y0 + IN_ROOM_H; y++)
                for (int x = x0; x < x0 + IN_ROOM_W; x++) {
                    int t = (back && y < y0 + IN_WALL_ROWS) ? IT_WALL : IT_FLOOR;
                    if (x == x0 + IN_ROOM_W - 1 && right) t = IT_EDGE;
                    if (y == y0 + IN_ROOM_H - 1 && !has(p, gx, gy + 1)) t = IT_EDGE;
                    T(in, x, y) = (uint8_t)t;
                }
            // Doorways: two tiles through the wall to its right, through its back wall into the room above.
            if (!p->open && (p->join_r >> g & 1) && has(p, gx + 1, gy))
                for (int y = y0 + IN_WALL_ROWS + 1; y < y0 + IN_WALL_ROWS + 3; y++) T(in, x0 + IN_ROOM_W - 1, y) = IT_FLOOR;
            if (!p->open && has(p, gx, gy - 1) && (p->join_d >> (g - IN_GRID) & 1)) {
                const int dx = x0 + IN_PASS;
                for (int y = y0 - 1; y < y0 + IN_WALL_ROWS; y++)
                    for (int x = dx; x < dx + 2; x++) T(in, x, y) = IT_FLOOR;
            }
        }
    // The walls round it all: wherever a tile meets nothing.
    for (int y = 0; y < in->rows; y++)
        for (int x = 0; x < in->cols; x++) {
            if (T(in, x, y) != IT_VOID) continue;
            bool near = false;
            for (int dy = -1; dy <= 1 && !near; dy++)
                for (int dx = -1; dx <= 1 && !near; dx++) {
                    const int xx = x + dx, yy = y + dy;
                    near = xx >= 0 && yy >= 0 && xx < IN_COLS && yy < IN_ROWS && T(in, xx, yy) != IT_VOID && T(in, xx, yy) != IT_EDGE;
                }
            if (near) T(in, x, y) = IT_EDGE;
        }
    // The door out (the entry room's back wall, its left part) and the stairs (its back corners).
    in->door_x = in->up_x = in->down_x = -1;
    // (In an open hall: on its back wall, behind the cell gone in by.)
    if (p->open && p->entry >= 0)
        while (has(p, p->entry % IN_GRID, p->entry / IN_GRID - 1)) in->plan.entry -= IN_GRID;
    const int e = p->entry >= 0 ? p->entry : 0;
    if (has(p, e % IN_GRID, e / IN_GRID)) {
        const int x0 = 1 + (e % IN_GRID) * IN_ROOM_W, y0 = 1 + (e / IN_GRID) * IN_ROOM_H;
        const int top = y0 + IN_WALL_ROWS;                     // the floor's first row
        if (p->entry >= 0 && in->floor == 0) {
            in->door_x = x0 + 1, in->door_y = top - 1;
            T(in, in->door_x, in->door_y) = T(in, in->door_x + 1, in->door_y) = IT_DOOR;
        }
        if (in->floor < in->floors - 1) {
            in->up_x = x0 + IN_ROOM_W - 3, in->up_y = top;
            T(in, in->up_x, top) = T(in, in->up_x + 1, top) = IT_UP;
        }
        if (in->floor > 0) {
            in->down_x = x0 + 1, in->down_y = top;
            T(in, in->down_x, top) = T(in, in->down_x + 1, top) = IT_DOWN;
        }
    }
}

static void walker_at_entry(interior_t *in, int which)       // 0: the door, 1: stairs down, 2: stairs up
{
    if (which == 0 && in->door_x >= 0) in->x = in->door_x + 1.0f, in->y = in->door_y + 1.6f;
    else if (which == 1 && in->down_x >= 0) in->x = in->down_x + 1.0f, in->y = in->down_y + 1.7f;
    else if (which == 2 && in->up_x >= 0) in->x = in->up_x + 1.0f, in->y = in->up_y + 1.7f;
    else in->x = in->cols / 2.0f, in->y = in->rows / 2.0f;
}

void interior_enter(interior_t *in, int floors, const interior_plan_t *plan, const uint16_t colours[IN_COLOURS])
{
    memset(in, 0, sizeof(*in));
    in->floors = floors < 1 ? 1 : floors;
    in->plan = *plan;
    memcpy(in->col, colours, sizeof(in->col));
    lay_out(in);
    walker_at_entry(in, 0);
}

void interior_floor(interior_t *in, int floor, const interior_plan_t *plan)
{
    const bool up = floor > in->floor;
    in->floor = floor;
    in->plan = *plan;
    in->nitems = 0;
    lay_out(in);
    walker_at_entry(in, up ? 1 : 2);
    in->on_stairs = true;                                       // a step off them before they act
}

bool interior_room(const interior_t *in, int gx, int gy, int *x, int *y, int *w, int *h)
{
    if (!has(&in->plan, gx, gy)) return false;
    *x = 1 + gx * IN_ROOM_W;
    *y = 1 + gy * IN_ROOM_H + IN_WALL_ROWS;
    *w = IN_ROOM_W - 1;
    *h = IN_ROOM_H - IN_WALL_ROWS - 1;
    return true;
}

void interior_add(interior_t *in, const fart_t *a, int img, float x, float y, float depth)
{
    if (in->nitems >= IN_ITEMS || img < 0 || img >= a->nimg) return;
    in->item[in->nitems++] = (interior_item_t){ (int16_t)img, x, y, depth };
}

static float item_bottom(const interior_item_t *it, const fart_t *a) { return it->y + (float)a->img[it->img].h / IN_TILE; }

static int tile_at(const interior_t *in, float x, float y)
{
    const int tx = (int)floorf(x), ty = (int)floorf(y);
    if (tx < 0 || ty < 0 || tx >= in->cols || ty >= in->rows) return IT_VOID;
    return T(in, tx, ty);
}
static bool walkable(int t) { return t == IT_FLOOR || t == IT_DOOR || t == IT_UP || t == IT_DOWN; }

static bool blocked(const interior_t *in, const fart_t *a, float x, float y)
{
    const float r = 0.3f;
    // The feet: their middle and either side, on floor (a little room in front of a back wall's foot).
    if (!walkable(tile_at(in, x, y)) || !walkable(tile_at(in, x - r, y)) || !walkable(tile_at(in, x + r, y)) ||
        !walkable(tile_at(in, x, y - 0.25f)))
        return true;
    for (int i = 0; i < in->nitems; i++) {
        const interior_item_t *it = &in->item[i];
        if (it->depth <= 0) continue;
        const float x1 = it->x + (float)a->img[it->img].w / IN_TILE, y1 = item_bottom(it, a), y0 = y1 - it->depth;
        if (x + r > it->x && x - r < x1 && y > y0 && y - 0.2f < y1) return true;
    }
    return false;
}

int interior_move(interior_t *in, const fart_t *a, float dx, float dy)
{
    float nx = in->x + dx, ny = in->y + dy;
    if (blocked(in, a, nx, ny)) {                               // slide along what is in the way
        if (!blocked(in, a, nx, in->y)) ny = in->y;
        else if (!blocked(in, a, in->x, ny)) nx = in->x;
        else return IN_NONE;
    }
    in->x = nx;
    in->y = ny;
    const int t = tile_at(in, nx, ny - 0.25f);
    if (t == IT_DOOR) return IN_EXIT;
    if (t != IT_UP && t != IT_DOWN) {
        in->on_stairs = false;
        return IN_NONE;
    }
    if (in->on_stairs) return IN_NONE;
    in->on_stairs = true;
    return t == IT_UP ? IN_UP : IN_DOWN;
}

static void rect(uint16_t *px, int w, int h, int x, int y, int rw, int rh, uint16_t c)
{
    for (int j = y < 0 ? 0 : y; j < y + rh && j < h; j++)
        for (int i = x < 0 ? 0 : x; i < x + rw && i < w; i++) px[j * w + i] = c;
}

static void put(uint16_t *px, int w, int h, const fart_t *a, int img, int x, int y)
{
    if (img < 0 || img >= a->nimg) return;
    const fart_clip_t clip = { 0, 0, w, h };
    const fart_img_t *im = &a->img[img];
    fart_draw(px, w, &clip, im, x - im->ox, y - im->oy, false);   // its picture's top-left at (x, y)
}

void interior_draw(const interior_t *in, const fart_t *a, const interior_art_t *art, uint16_t *px, int w, int h,
                   void (*draw_walker)(uint16_t *px, int w, int h, int x, int y, void *user), void *user)
{
    const uint16_t dark = 0x1082, line = 0x2124;
    for (int i = 0; i < w * h; i++) px[i] = dark;
    // The view: on the walker, kept on the floor when it fits.
    const int fw = in->cols * IN_TILE, fh = in->rows * IN_TILE;
    int ox = fw <= w ? (w - fw) / 2 : w / 2 - (int)(in->x * IN_TILE);
    int oy = fh <= h ? (h - fh) / 2 : h / 2 - (int)(in->y * IN_TILE);
    if (fw > w) ox = ox > 0 ? 0 : ox < w - fw ? w - fw : ox;
    if (fh > h) oy = oy > 0 ? 0 : oy < h - fh ? h - fh : oy;
    // Floors, walls round, back walls (a wall's slice where its run starts), the door, windows, stairs.
    for (int y = 0; y < in->rows; y++)
        for (int x = 0; x < in->cols; x++) {
            const int t = T(in, x, y), sx = ox + x * IN_TILE, sy = oy + y * IN_TILE;
            if (sx <= -IN_TILE || sy <= -IN_TILE * IN_WALL_ROWS || sx >= w || sy >= h) continue;
            if (t == IT_FLOOR || t == IT_UP || t == IT_DOWN) {
                if (art && art->floor >= 0) put(px, w, h, a, art->floor, sx, sy);
                else rect(px, w, h, sx, sy, IN_TILE, IN_TILE, (x + y / 2) & 1 ? in->col[IN_FLOOR_DARK] : in->col[IN_FLOOR_LIGHT]);
            } else if (t == IT_WALL || t == IT_DOOR) {
                const bool start = y == 0 || (T(in, x, y - 1) != IT_WALL && T(in, x, y - 1) != IT_DOOR);
                if (!start) continue;
                if (art && art->wall >= 0) put(px, w, h, a, art->wall, sx, sy);
                else rect(px, w, h, sx, sy, IN_TILE, IN_WALL_ROWS * IN_TILE, in->col[IN_WALL]);
            } else if (t == IT_EDGE) {
                rect(px, w, h, sx, sy, IN_TILE, IN_TILE, in->col[IN_WALL_DARK]);
                rect(px, w, h, sx, sy, IN_TILE, 1, line);
            }
        }
    // Windows: one on each room's back wall, clear of its door, doorway and stairs.
    for (int g = 0; g < IN_GRID * IN_GRID && art; g++) {
        const int gx = g % IN_GRID, gy = g / IN_GRID, x0 = 1 + gx * IN_ROOM_W, y0 = 1 + gy * IN_ROOM_H;
        if (!has(&in->plan, gx, gy)) continue;
        const int win = art->window[g & 1] >= 0 ? art->window[g & 1] : art->window[1 - (g & 1)];
        static const int8_t at[3] = { IN_ROOM_W - 3, 1, IN_PASS - 1 };
        for (int k = 0; k < 3 && win >= 0; k++) {
            const int x = x0 + at[k];
            const bool by_door = in->door_x >= 0 && x + 1 >= in->door_x - 1 && x <= in->door_x + 2;
            if (by_door || T(in, x, y0) != IT_WALL || T(in, x + 1, y0) != IT_WALL) continue;
            put(px, w, h, a, win, ox + x * IN_TILE + 1, oy + y0 * IN_TILE + 6);
            break;
        }
    }
    if (in->door_x >= 0) {
        const int dx = ox + in->door_x * IN_TILE + IN_TILE, dy = oy + (in->door_y + 1) * IN_TILE;   // its foot's middle
        if (art && art->door >= 0) put(px, w, h, a, art->door, dx - a->img[art->door].w / 2, dy - a->img[art->door].h);
        else rect(px, w, h, dx - IN_TILE, dy - IN_TILE * 2, IN_TILE * 2, IN_TILE * 2, 0x4a49);
    }
    for (int s = 0; s < 2; s++) {
        const int sx = s == 0 ? in->up_x : in->down_x, syy = s == 0 ? in->up_y : in->down_y;
        if (sx < 0) continue;
        const int x = ox + sx * IN_TILE + 2, y = oy + syy * IN_TILE - IN_TILE;
        for (int k = 0; k < 8; k++) {
            const int py = y + k * 4, shade = s == 0 ? 8 - k : k + 1;
            const uint16_t c = (uint16_t)(((shade * 2 + 6) << 11) | ((shade * 4 + 12) << 5) | (shade * 2 + 5));
            rect(px, w, h, x, py, IN_TILE * 2 - 4, 4, c);
            rect(px, w, h, x, py + 3, IN_TILE * 2 - 4, 1, line);
        }
    }
    // Furniture and the walker, back to front (by their bottoms; rugs first).
    int order[IN_ITEMS], n = 0;
    for (int i = 0; i < in->nitems; i++) order[n++] = i;
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0; j--) {
            const interior_item_t *p = &in->item[order[j - 1]], *q = &in->item[order[j]];
            const float bp = p->depth <= 0 ? -1 : item_bottom(p, a), bq = q->depth <= 0 ? -1 : item_bottom(q, a);
            if (bp <= bq) break;
            const int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
        }
    bool walker = false;
    for (int k = 0; k <= n; k++) {
        const interior_item_t *it = k < n ? &in->item[order[k]] : NULL;
        if (!walker && (!it || (it->depth > 0 && item_bottom(it, a) > in->y))) {
            if (draw_walker) draw_walker(px, w, h, ox + (int)(in->x * IN_TILE), oy + (int)(in->y * IN_TILE), user);
            walker = true;
        }
        if (it) put(px, w, h, a, it->img, ox + (int)(it->x * IN_TILE), oy + (int)(it->y * IN_TILE));
    }
}
