#include "world.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "flag.h"

// Blocks of at least 4160 bytes: ESP-IDF serves smaller ones from internal RAM, which Wi-Fi needs.
static void *ps_calloc(size_t n) { return calloc(n < 4160 ? 4160 : n, 1); }

static uint16_t rgb565(uint32_t c) { return (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F)); }

// The view: the world turned a quarter at a time (0..3, clockwise on screen). Cells and points are kept in the
// world's own columns and rows (c, r; u, v); the screen's (sc, sr) are those turned. One world at a time.
static int s_view;
static void to_screen(int c, int r, int *sc, int *sr)
{
    switch (s_view) {
    case 1: *sc = WORLD_H - 1 - r, *sr = c; break;
    case 2: *sc = WORLD_W - 1 - c, *sr = WORLD_H - 1 - r; break;
    case 3: *sc = r, *sr = WORLD_W - 1 - c; break;
    default: *sc = c, *sr = r; break;
    }
}
static void to_world(int sc, int sr, int *c, int *r)
{
    switch (s_view) {
    case 1: *c = sr, *r = WORLD_H - 1 - sc; break;
    case 2: *c = WORLD_W - 1 - sc, *r = WORLD_H - 1 - sr; break;
    case 3: *c = WORLD_W - 1 - sr, *r = sc; break;
    default: *c = sc, *r = sr; break;
    }
}
void world_to_screen(float u, float v, float *su, float *sv) { world_to_screen_v(s_view, u, v, su, sv); }
void world_from_screen(float su, float sv, float *u, float *v) { world_from_screen_v(s_view, su, sv, u, v); }
void world_to_screen_v(int view, float u, float v, float *su, float *sv)
{
    switch (view & 3) {
    case 1: *su = WORLD_H - v, *sv = u; break;
    case 2: *su = WORLD_W - u, *sv = WORLD_H - v; break;
    case 3: *su = v, *sv = WORLD_W - u; break;
    default: *su = u, *sv = v; break;
    }
}
void world_from_screen_v(int view, float su, float sv, float *u, float *v)
{
    switch (view & 3) {
    case 1: *u = sv, *v = WORLD_H - su; break;
    case 2: *u = WORLD_W - su, *v = WORLD_H - sv; break;
    case 3: *u = WORLD_W - sv, *v = su; break;
    default: *u = su, *v = sv; break;
    }
}
void world_px(float u, float v, float *x, float *y)
{
    float su, sv;
    world_to_screen(u, v, &su, &sv);
    *x = (su - sv) * (TW / 2);
    *y = (su + sv) * (TH / 2);
}
// A side's bits (N 1, E 2, S 4, W 8) turned from the world's to the screen's.
static int turn_mask(int m) { return ((m << s_view) | (m >> (4 - s_view))) & 15; }

// The view cache: the screen plus a margin on each side, so the camera moves within it for a while.
#define CACHE_MX 96
#define CACHE_MY 96

bool world_init(world_t *w, fart_t *art)
{
    memset(w, 0, sizeof(*w));
    w->art = art;
    w->sky = rgb565(0x1c2a3c);
    w->cells = ps_calloc(sizeof(cell_t) * WORLD_W * WORLD_H);
    w->edits = ps_calloc(sizeof(edit_t) * EDITS_MAX);
    char n[24];
    for (int s = 0; s < 2; s++)
        for (int i = 0; i < 8; i++) {
            snprintf(n, sizeof(n), "sails.%c.%d", 'a' + s, i);
            const int k = fart_image(art, n);
            w->sails[s][i] = k < 0 ? FART_NONE : (uint16_t)k;
        }
    // The buildings' palettes (fh1model.py): the reference colours and each walls-and-roof's, four pixels each.
    static const char wcs[2] = { 'o', 'w' }, rcs[4] = { 'b', 'g', 'r', 'o' };
    uint8_t mask[8];
    const int pk = fart_image(art, "pal.key");
    w->pal_ok = pk >= 0 && fart_unpack(&art->img[pk], w->pal_key, mask, 4);
    for (int i = 0; i < 8 && w->pal_ok; i++) {
        snprintf(n, sizeof(n), "pal.%c%c", wcs[i >> 2], rcs[i & 3]);
        const int k = fart_image(art, n);
        w->pal_ok = k >= 0 && fart_unpack(&art->img[k], w->pal[i], mask, 4);
    }
    // Flags: the engine's, in the roofs' colours (the towers' blue, green, red, wood).
    flag_load(&w->flag, art, "flag");
    for (int i = 0; i < 4 && w->pal_ok; i++) w->flag_col[i][0] = w->pal[i][2], w->flag_col[i][1] = w->pal[i][3];
    static const char *const col[4] = { "blue", "green", "red", "wood" };
    for (int k = 0; k < 4; k++)
        for (int i = 0; i < 9; i++) {
            snprintf(n, sizeof(n), "tower.%s.%d", col[k], i + 1);
            const int t = fart_image(art, n);
            w->towers[k][i] = t < 0 ? FART_NONE : (uint16_t)t;
        }
    for (int i = 0; i < 3; i++) {
        snprintf(n, sizeof(n), "smoke.%d", i);
        const int k = fart_image(art, n);
        w->smoke[i] = k < 0 ? FART_NONE : (uint16_t)k;
    }
    return w->cells != NULL && w->edits != NULL;
}

void world_free(world_t *w)
{
    free(w->cells);
    free(w->cache);
    free(w->edits);
    w->cells = NULL;
    w->cache = NULL;
    w->edits = NULL;
}

// ── Making the world ──

static uint32_t hash(int c, int r, uint32_t seed)
{
    uint32_t h = (uint32_t)c * 374761393u + (uint32_t)r * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
static float rnd01(int c, int r, uint32_t seed) { return (hash(c, r, seed) & 0xffff) / 65536.0f; }
// Smooth noise: random values on a grid of `step` cells, blended between them.
static float noise(int c, int r, int step, uint32_t seed)
{
    const int gc = c / step, gr = r / step;
    const float fc = (float)(c % step) / step, fr = (float)(r % step) / step;
    const float sc = fc * fc * (3 - 2 * fc), sr = fr * fr * (3 - 2 * fr);
    const float a = rnd01(gc, gr, seed), b = rnd01(gc + 1, gr, seed), d = rnd01(gc, gr + 1, seed), e = rnd01(gc + 1, gr + 1, seed);
    return (a + (b - a) * sc) * (1 - sr) + (d + (e - d) * sc) * sr;
}

static int img(world_t *w, const char *fmt, int k)
{
    char n[24];
    snprintf(n, sizeof(n), fmt, k);
    const int i = fart_image(w->art, n);
    return i < 0 ? FART_NONE : i;
}

static void put_obj(world_t *w, int c, int r, int obj, int image)
{
    if (!world_inside(c, r)) return;
    cell_t *x = world_cell(w, c, r);
    x->obj = (uint8_t)obj;
    x->obj_img = (uint16_t)image;
}

int world_obj_image(world_t *w, int obj, int ovar)
{
    static const char *const roof[4] = { "blue", "green", "red", "wood" };
    char n[32];
    switch (obj) {
    case O_TREE: snprintf(n, sizeof(n), "tree.%d", 1 + ovar % 4); break;
    case O_STONE: snprintf(n, sizeof(n), "stone.%d", 1 + ovar % 4); break;
    case O_TUFT: snprintf(n, sizeof(n), "tuft.%d", 1 + ovar % 9); break;
    case O_HOUSE: case O_SMITH: return FART_NONE;            // drawn from their pieces (house_parts)
    case O_MILL: snprintf(n, sizeof(n), "mill.%d", 1 + (ovar & 3)); break;
    case O_WALL: snprintf(n, sizeof(n), "wall.%c", ((ovar >> 2) ^ s_view) & 1 ? 'v' : 'u'); break;          // alone (joined: resolve)
    case O_TOWER: snprintf(n, sizeof(n), "tower.%s.%d", roof[(ovar >> 4) & 3], 1 + (ovar & 15) % 9); break;
    case O_STAIR: snprintf(n, sizeof(n), "st%d", (ovar + s_view) & 3); break;   // climbing that way on screen
    default: return FART_NONE;
    }
    const int i = fart_image(w->art, n);
    return i < 0 ? FART_NONE : i;
}

// A thing by kind and variant (its image looked up), as world_make places them.
static void put(world_t *w, int c, int r, int obj, int ovar)
{
    if (!world_inside(c, r)) return;
    world_cell(w, c, r)->ovar = (uint8_t)ovar;
    put_obj(w, c, r, obj, world_obj_image(w, obj, ovar));
}

// Roads and rivers: a piece for each set of neighbours (bits N -r 1, E +c 2, S +r 4, W -c 8), painted by the art
// tool ("road.<mask>", "river.<mask>"); a bridge carries a road one way over water the other ("bridge.ew", ".ns").
enum { N_ = 1, E_ = 2, S_ = 4, W_ = 8 };
static bool joins(int ground, bool road)
{
    return road ? ground == G_ROAD || ground == G_BRIDGE : ground == G_RIVER || ground == G_BRIDGE;
}
static int mask_of(world_t *w, int c, int r, bool road)
{
    static const int8_t dc[4] = { 0, 1, 0, -1 }, dr[4] = { -1, 0, 1, 0 };
    int m = 0;
    for (int k = 0; k < 4; k++)
        if (world_inside(c + dc[k], r + dr[k]) && joins(world_cell(w, c + dc[k], r + dr[k])->ground, road)) m |= 1 << k;
    return m;
}

// The ground's image on a cell (a road's or river's piece from its neighbours).
static int ground_image(world_t *w, int c, int r, int ground, int var)
{
    switch (ground) {
    case G_DIRT: return img(w, "g.dirt.%d", 1 + var % 2);
    case G_WHEAT: return img(w, "g.wheat.%d", 1 + var % 2);
    case G_CORN: return img(w, "g.corn.%d", 1 + var % 2);
    case G_ROAD: return img(w, "road.%d", turn_mask(mask_of(w, c, r, true)));
    case G_RIVER: return img(w, "river.%d", turn_mask(mask_of(w, c, r, false)));
    case G_BRIDGE: return img(w, turn_mask(mask_of(w, c, r, true)) & (E_ | W_) ? "bridge.ew" : "bridge.ns", 0);
    default: return img(w, "g.grass", 0);
    }
}

// Castle walls join their neighbours (walls and towers): a straight along rows (u) or columns (v), plain, with a
// window or a gate, or a corner (E+S, N+E, S+W, N+W); three or four joins show the straight their through way
// takes (the pack has no T or cross); alone, the way its variant says. A stacked level joins only what reaches as
// high: walls with as many levels, or the tower on top of one at that level.
enum { WP_U, WP_V, WP_ES, WP_NE, WP_SW, WP_NW };
static bool wall_joins(world_t *w, int c, int r, int level)
{
    if (!world_inside(c, r)) return false;
    const cell_t *x = world_cell(w, c, r);
    if (x->obj == O_TOWER) return level == 0;
    if (x->obj != O_WALL) return false;
    const int walls = 1 + STACK_UP(x);
    return level < walls || (level == walls && STACK_TOWER(x));
}
// The sides a wall's level joins, on screen.
static int wall_mask_at(world_t *w, int c, int r, int level)
{
    return turn_mask((wall_joins(w, c, r - 1, level) ? N_ : 0) | (wall_joins(w, c + 1, r, level) ? E_ : 0) |
                     (wall_joins(w, c, r + 1, level) ? S_ : 0) | (wall_joins(w, c - 1, r, level) ? W_ : 0));
}
static int wall_piece_at(world_t *w, int c, int r, int level)
{
    const int m = wall_mask_at(w, c, r, level);
    switch (m) {
    case E_ | S_: return WP_ES;
    case N_ | E_: return WP_NE;
    case S_ | W_: return WP_SW;
    case N_ | W_: return WP_NW;
    }
    const bool ew = (m & E_) && (m & W_), ns = (m & N_) && (m & S_);
    if (ew && !ns) return WP_U;
    if (ns && !ew) return WP_V;
    if (!ew && !ns && (m & (E_ | W_))) return WP_U;
    if (!ew && !ns && (m & (N_ | S_))) return WP_V;
    return ((world_cell(w, c, r)->ovar >> 2) ^ s_view) & 1 ? WP_V : WP_U;    // alone, or a cross
}
static bool wall_is_gate(world_t *w, int c, int r)
{
    const int p = wall_piece_at(w, c, r, 0);
    return (world_cell(w, c, r)->ovar & 3) == WALL_GATE && (p == WP_U || p == WP_V);
}
// A wall's level: its piece (fh1model.py's castle_set) by the sides it joins on screen, with its parapet when it is
// the top (no wall or tower on it).
static int wall_image_at(world_t *w, int c, int r, int level, int kind)
{
    (void)kind;
    const cell_t *x = world_cell(w, c, r);
    const bool top = level == STACK_UP(x) && !STACK_TOWER(x);
    return img(w, top ? "wl%d.t" : "wl%d.b", wall_mask_at(w, c, r, level));
}

// ── Buildings joined ──

// A house's or smithy's cell the player built keeps the sides it is joined on in its var: bits 0-3 N, E, S, W, and
// JOINED (bit 4: kept this way); UPPER (bit 5) marks a 2 x 2 house's upper storey. A building is the cells joined
// to one another, of any shape and size; a new piece joins what stands beside it and the pieces already there keep
// their joins.
#define SIDES(x) ((x)->var & JOINED ? (x)->var & 15 : 0)
static const int8_t s_dc[4] = { 0, 1, 0, -1 }, s_dr[4] = { -1, 0, 1, 0 };    // N, E, S, W
static bool joinable(const cell_t *x) { return (x->obj == O_HOUSE || x->obj == O_SMITH) && (x->flags & F_BUILT); }
// Can these two join? Any two houses (each keeps its colours); a smithy (its walls wood) with wooden houses only.
static bool join_ok(const cell_t *a, const cell_t *b)
{
    if (a->obj == O_SMITH || b->obj == O_SMITH)
        return !(a->obj == O_HOUSE && (a->ovar & 0x10)) && !(b->obj == O_HOUSE && (b->ovar & 0x10));
    return true;
}

int world_group(world_t *w, int c, int r, int *gc, int *gr)
{
    int n = 1;
    gc[0] = c;
    gr[0] = r;
    for (int i = 0; i < n; i++) {
        const cell_t *x = world_cell(w, gc[i], gr[i]);
        if (!joinable(x)) break;
        for (int k = 0; k < 4; k++) {
            if (!(SIDES(x) & 1 << k)) continue;
            const int cc = gc[i] + s_dc[k], rr = gr[i] + s_dr[k];
            bool seen = false;
            for (int j = 0; j < n; j++) seen |= gc[j] == cc && gr[j] == rr;
            if (!seen && n < GROUP_MAX && world_inside(cc, rr)) gc[n] = cc, gr[n++] = rr;
        }
    }
    return n;
}

// The deepest of some cells on screen (column + row, then the column): the one that draws a building made whole.
static int deepest(const int *gc, const int *gr, int n)
{
    int best = 0, bs = -1, bc = -1;
    for (int i = 0; i < n; i++) {
        int sc, sr;
        to_screen(gc[i], gr[i], &sc, &sr);
        if (sc + sr > bs || (sc + sr == bs && sc > bc)) bs = sc + sr, bc = sc, best = i;
    }
    return best;
}
static bool is_front(world_t *w, int c, int r)
{
    int gc[GROUP_MAX], gr[GROUP_MAX];
    const int n = world_group(w, c, r, gc, gr);
    const int f = deepest(gc, gr, n);
    return gc[f] == c && gr[f] == r;
}

typedef struct { int n, k[4]; } merge_t;                         // the sides of the new piece to join

// How a new piece at (c, r) (alone yet) would join: every neighbour it may join.
static bool merge_plan(world_t *w, int c, int r, merge_t *m)
{
    m->n = 0;
    const cell_t *x = world_cell(w, c, r);
    if (!joinable(x) || SIDES(x)) return false;
    for (int k = 0; k < 4; k++) {
        const int cc = c + s_dc[k], rr = r + s_dr[k];
        if (!world_inside(cc, rr)) continue;
        const cell_t *y = world_cell(w, cc, rr);
        if (joinable(y) && join_ok(x, y)) m->k[m->n++] = k;
    }
    return m->n > 0;
}

// Joins on a side, both ways (in place: the caller records them).
static void link(world_t *w, int c, int r, int k, bool on)
{
    cell_t *x = world_cell(w, c, r), *y = world_cell(w, c + s_dc[k], r + s_dr[k]);
    const int sx = (SIDES(x) & ~(1 << k)) | (on ? 1 << k : 0), sy = (SIDES(y) & ~(1 << ((k + 2) & 3))) | (on ? 1 << ((k + 2) & 3) : 0);
    x->var = (uint8_t)(sx ? JOINED | sx : 0);
    y->var = (uint8_t)(sy ? JOINED | sy : 0);
}

// A 2 x 2 house of one kind (a building of exactly four houses in a square, the same walls and roof): its four cells.
static bool big_house(world_t *w, int c, int r, int *gc, int *gr)
{
    const cell_t *x = world_cell(w, c, r);
    if (x->obj != O_HOUSE || !SIDES(x)) return false;
    if (world_group(w, c, r, gc, gr) != 4) return false;
    int a = gc[0], b = gr[0];
    for (int i = 1; i < 4; i++) {
        if (gc[i] < a) a = gc[i];
        if (gr[i] < b) b = gr[i];
    }
    for (int i = 0; i < 4; i++) {
        const cell_t *y = world_cell(w, gc[i], gr[i]);
        if (gc[i] > a + 1 || gr[i] > b + 1 || y->obj != O_HOUSE || (y->ovar & 0x1c) != (x->ovar & 0x1c)) return false;
    }
    return true;
}

// A castle's 2 x 2 keep: four plain walls (or with windows) whose top is at one level, in a square that is not part of
// a bigger block at that level (a tower on top the same on all four): on the ground, or on the middle of a castle's
// floor (a 4 x 4 block, the keep's walls stacked on its middle 2 x 2). any_tower: the towers not compared (while one
// is being put on). Its level in *level.
static bool wall_top(world_t *w, int c, int r, int level)       // a wall of this square: its top at level
{
    if (!world_inside(c, r)) return false;
    const cell_t *x = world_cell(w, c, r);
    return x->obj == O_WALL && (x->ovar & 3) != WALL_GATE && STACK_UP(x) == level;
}
static bool wall_up(world_t *w, int c, int r, int level)        // a wall reaching that level (a bigger block)
{
    if (!world_inside(c, r)) return false;
    const cell_t *x = world_cell(w, c, r);
    return x->obj == O_WALL && STACK_UP(x) >= level;
}
static bool keep_at(world_t *w, int c, int r, bool any_tower, int *gc, int *gr, int *level)
{
    const cell_t *me = world_cell(w, c, r);
    if (me->obj != O_WALL) return false;
    const int L = STACK_UP(me);
    for (int q = 0; q < 4; q++) {
        const int a = c - (q & 1), b = r - (q >> 1);
        bool ok = true;
        for (int k = 0; k < 4 && ok; k++) ok = wall_top(w, a + (k & 1), b + (k >> 1), L);
        if (!ok) continue;
        const cell_t *x = world_cell(w, a, b);
        for (int k = 1; k < 4 && ok && !any_tower; k++) {
            const cell_t *y = world_cell(w, a + (k & 1), b + (k >> 1));
            ok = (y->var & 0x7c) == (x->var & 0x7c) && (y->ovar & 0x18) == (x->ovar & 0x18);
        }
        // Not in a bigger block at its level: no two walls that reach it side by side beyond any of its sides.
        if (ok && ((wall_up(w, a - 1, b, L) && wall_up(w, a - 1, b + 1, L)) || (wall_up(w, a + 2, b, L) && wall_up(w, a + 2, b + 1, L)) ||
                   (wall_up(w, a, b - 1, L) && wall_up(w, a + 1, b - 1, L)) || (wall_up(w, a, b + 2, L) && wall_up(w, a + 1, b + 2, L))))
            ok = false;
        if (!ok) continue;
        for (int k = 0; k < 4; k++) gc[k] = a + (k & 1), gr[k] = b + (k >> 1);
        if (level) *level = L;
        return true;
    }
    return false;
}

bool world_is_keep(world_t *w, int c, int r)
{
    int gc[4], gr[4];
    return keep_at(w, c, r, false, gc, gr, NULL);
}
bool world_is_big(world_t *w, int c, int r)
{
    int gc[GROUP_MAX], gr[GROUP_MAX];
    return big_house(w, c, r, gc, gr);
}

static int named(world_t *w, const char *fmt, ...)
{
    char name[32];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(name, sizeof(name), fmt, ap);
    va_end(ap);
    const int i = fart_image(w->art, name);
    return i < 0 ? FART_NONE : i;
}

// A house's or smithy's pictures (fh1model.py's): its piece by what is round it on screen (its ridge's way: along
// its world row if it joins that way, else down its column, alone across its door's way; its joined sides; a
// parallel row or a wing across), then a door or windows on its walls seen (the building's front cell has the
// door), a smithy's chimney. A 2 x 2 house of one kind is drawn whole by its front cell.
static int house_parts(world_t *w, int c, int r, uint16_t *ims, int *n)
{
    const cell_t *x = world_cell(w, c, r);
    // Its colours: the pieces are drawn in reference colours, swapped for its walls' and roof's (w->pal).
    const int8_t pal = (int8_t)((x->obj == O_SMITH ? 0 : ((x->ovar >> 4) & 1) * 4) + ((x->ovar >> 2) & 3));
    int gc[GROUP_MAX], gr[GROUP_MAX];
    if (big_house(w, c, r, gc, gr)) {
        const int f = deepest(gc, gr, 4);
        if (gc[f] == c && gr[f] == r) {
            const int i = named(w, "big%d", (x->var & UPPER) != 0);
            if (i != FART_NONE) w->part_pal[*n] = pal, ims[(*n)++] = (uint16_t)i;
        }
        return 0;
    }
    const int sd = SIDES(x), m = turn_mask(sd);
    const bool ku = ((sd & (E_ | W_)) ? true : (sd & (N_ | S_)) ? false : (x->ovar & 1)) ^ (s_view & 1);
    // The neighbour's ridge on screen across a joined side (1: the same way, a parallel row; 2: a wing).
    int across[4] = { 0, 0, 0, 0 };
    for (int d = 0; d < 4; d++) {
        if (!(m & 1 << d)) continue;
        const int k = (d - s_view) & 3;
        const cell_t *y = world_cell(w, c + s_dc[k], r + s_dr[k]);
        const int ys = SIDES(y);
        const bool yu = ((ys & (E_ | W_)) ? true : (ys & (N_ | S_)) ? false : (y->ovar & 1)) ^ (s_view & 1);
        across[d] = yu == ku ? 1 : 2;
    }
    const int along = ku ? ((m & W_) ? 1 : 0) | ((m & E_) ? 2 : 0) : ((m & N_) ? 1 : 0) | ((m & S_) ? 2 : 0);
    const int lo = ku ? across[0] : across[3], hi = ku ? across[2] : across[1];
    const int p = named(w, "c%c%d%d%d", ku ? 'u' : 'v', along, lo, hi);
    if (p != FART_NONE) w->part_pal[*n] = pal, ims[(*n)++] = (uint16_t)p;
    // Doors and windows: on the walls seen (S and E on screen, not joined).
    const bool s_wall = !(m & S_), e_wall = !(m & E_);
    int door = -1;                                               // 2: on S, 1: on E
    if (!sd) {                                                   // alone: by its facing on screen
        const int fs = (x->ovar + s_view) & 3;
        door = fs == 2 ? 2 : fs == 1 ? 1 : -1;
        const int i = named(w, fs == 2 ? "dS" : fs == 1 ? "dE" : fs == 0 ? "wS" : "wE");
        if (i != FART_NONE) ims[(*n)++] = (uint16_t)i;
    } else {
        if (is_front(w, c, r)) door = s_wall ? 2 : e_wall ? 1 : -1;
        if (s_wall) {
            const int i = named(w, door == 2 ? "dS" : "wS");
            if (i != FART_NONE) ims[(*n)++] = (uint16_t)i;
        }
        if (e_wall) {
            const int i = named(w, door == 1 ? "dE" : "wE");
            if (i != FART_NONE) ims[(*n)++] = (uint16_t)i;
        }
    }
    if (x->obj == O_SMITH) {
        const int i = named(w, "chimney");
        if (i != FART_NONE) ims[(*n)++] = (uint16_t)i;
    }
    return 0;
}

static void resolve(world_t *w, int c, int r)
{
    cell_t *x = world_cell(w, c, r);
    x->img = (uint16_t)ground_image(w, c, r, x->ground, x->var);
    if (x->obj != O_NONE) x->obj_img = (uint16_t)world_obj_image(w, x->obj, x->ovar);   // as the view turns it
    if (x->obj == O_WALL) x->obj_img = (uint16_t)wall_image_at(w, c, r, 0, x->ovar & 3);
}

static void lay(world_t *w, int c, int r, int ground)
{
    if (!world_inside(c, r)) return;
    cell_t *x = world_cell(w, c, r);
    if ((ground == G_ROAD && x->ground == G_RIVER) || (ground == G_RIVER && x->ground == G_ROAD)) ground = G_BRIDGE;
    x->ground = (uint8_t)ground;
    x->obj = O_NONE;
    x->obj_img = FART_NONE;
}

// A house beside a road, turned toward it (the tileset draws each house four ways).
static void house(world_t *w, int c, int r, int toward, uint32_t h)
{
    put(w, c, r, O_HOUSE, (toward & 3) | (int)((h >> 1) & 3) << 2 | (int)(h & 1) << 4);
}

#define ROAD_ROW 64                  // the main road's row
// The village's middle and size (cells): an uneven oval, all of it the village's (F_TOWN).
#define VC_C 80
#define VC_R 58
#define VR_C 10.5f
#define VR_R 8.5f

static bool in_village(int c, int r, uint32_t seed)
{
    const float dc = (c - VC_C) / VR_C, dr = (r - VC_R) / VR_R;
    return dc * dc + dr * dr < 1.0f + 0.35f * (noise(c + 200, r + 200, 3, seed + 5) - 0.5f);
}

// A lane: a list of cells, each next to the one before (a bend is a change of direction).
static void lane(world_t *w, const int8_t (*pts)[2], int n)
{
    for (int i = 0; i + 1 < n; i++) {
        int c = VC_C + pts[i][0], r = VC_R + pts[i][1];
        const int c1 = VC_C + pts[i + 1][0], r1 = VC_R + pts[i + 1][1];
        while (c != c1 || r != r1) {
            lay(w, c, r, G_ROAD);
            if (c != c1) c += c1 > c ? 1 : -1;
            else r += r1 > r ? 1 : -1;
        }
        lay(w, c, r, G_ROAD);
    }
}

// A field: a ragged round patch of wheat or corn (rows of it: the variant follows the cell).
static void field(world_t *w, int cc, int cr, float rad, int ground, uint32_t seed)
{
    for (int r = cr - 4; r <= cr + 4; r++)
        for (int c = cc - 4; c <= cc + 4; c++) {
            const float d = sqrtf((float)((c - cc) * (c - cc) + (r - cr) * (r - cr)));
            if (!world_inside(c, r) || d > rad + 0.8f * (rnd01(c, r, seed + 11) - 0.5f)) continue;
            cell_t *x = world_cell(w, c, r);
            if (x->ground != G_GRASS) continue;
            lay(w, c, r, ground);
            x->var = (uint8_t)c;                                     // in rows
        }
}

// The lane cell next to (c, r) (0 N, 1 E, 2 S, 3 W), or -1.
static int lane_side(world_t *w, int c, int r)
{
    static const int8_t dc[4] = { 0, 1, 0, -1 }, dr[4] = { -1, 0, 1, 0 };
    for (int k = 0; k < 4; k++)
        if (world_inside(c + dc[k], r + dr[k]) && joins(world_cell(w, c + dc[k], r + dr[k])->ground, true)) return k;
    return -1;
}

static void village(world_t *w, uint32_t seed)
{
    // Clear it (the forest's trees, stones; tufts stay), then mark it: nobody's to change.
    for (int r = VC_R - 12; r <= VC_R + 12; r++)
        for (int c = VC_C - 14; c <= VC_C + 14; c++) {
            if (!world_inside(c, r) || !in_village(c, r, seed)) continue;
            cell_t *x = world_cell(w, c, r);
            if (x->obj != O_TUFT) put_obj(w, c, r, O_NONE, FART_NONE);
            x->flags |= F_TOWN;
        }
    field(w, VC_C + 7, VC_R + 2, 2.4f, G_WHEAT, seed);
    field(w, VC_C - 7, VC_R + 1, 2.0f, G_CORN, seed);
    field(w, VC_C + 4, VC_R - 6, 1.6f, G_WHEAT, seed + 1);
    // Lanes (cells from the middle): up from the main road with a jog to a little green, west from it and north,
    // east from it to the north road.
    static const int8_t l1[][2] = { { -2, 5 }, { -2, 3 }, { 0, 3 }, { 0, -2 } };
    static const int8_t l2[][2] = { { 0, 0 }, { -5, 0 }, { -5, -3 }, { -6, -3 }, { -6, -5 } };
    static const int8_t l3[][2] = { { 0, -2 }, { 3, -2 }, { 3, -4 } };
    lane(w, l1, 4);
    lane(w, l2, 5);
    lane(w, l3, 3);
    // Houses, scattered (each turned toward its lane, or the way its hash says when none is next to it), the
    // blacksmith by the main road, the mill by the big wheat field, fruit trees between.
    static const int8_t houses[][2] = { { -3, 2 }, { -1, 1 }, { 1, 1 }, { 1, -1 }, { -4, 1 }, { -6, -1 }, { -2, -1 },
                                        { -4, -4 }, { 2, -3 }, { 4, -3 }, { 3, 3 }, { -9, -2 }, { -1, -4 } };
    for (unsigned i = 0; i < sizeof(houses) / sizeof(houses[0]); i++) {
        const int c = VC_C + houses[i][0], r = VC_R + houses[i][1];
        if (world_cell(w, c, r)->ground != G_GRASS) continue;
        const uint32_t h = hash(c, r, seed + 9);
        const int k = lane_side(w, c, r);
        house(w, c, r, k >= 0 ? k : (int)(h >> 5) & 3, h);
    }
    lay(w, VC_C - 4, VC_R + 5, G_GRASS);
    put(w, VC_C - 4, VC_R + 5, O_SMITH, 2 << 2);
    lay(w, VC_C + 7, VC_R + 5, G_GRASS);
    put(w, VC_C + 7, VC_R + 5, O_MILL, 2);
    static const int8_t trees[][2] = { { -3, 0 }, { 2, 0 }, { -7, -4 }, { 2, -5 }, { -3, -6 }, { 5, -1 }, { -8, 3 },
                                       { 0, 5 }, { -5, 3 }, { 6, -4 } };
    for (unsigned i = 0; i < sizeof(trees) / sizeof(trees[0]); i++) {
        const int c = VC_C + trees[i][0], r = VC_R + trees[i][1];
        const cell_t *x = world_cell(w, c, r);
        if (x->ground == G_GRASS && x->obj != O_HOUSE && x->obj != O_SMITH && x->obj != O_MILL)
            put(w, c, r, O_TREE, hash(c, r, seed + 3) % 4);
    }
}

void world_make(world_t *w, float *start_u, float *start_v)
{
    const uint32_t seed = 2026;
    // Grass, its tufts, forests where the noise is high, a stone here and there.
    const int tree0 = img(w, "tree.%d", 1);
    for (int r = 0; r < WORLD_H; r++)
        for (int c = 0; c < WORLD_W; c++) {
            cell_t *x = world_cell(w, c, r);
            *x = (cell_t){ G_GRASS, 0, O_NONE, 0, 0, FART_NONE, 0 };
            const uint32_t h = hash(c, r, seed);
            const float f = noise(c, r, 9, seed) * 0.7f + noise(c, r, 4, seed + 1) * 0.3f;
            if (f > 0.62f && tree0 != FART_NONE) put(w, c, r, O_TREE, (h >> 8) % 4);
            else if (h % 100 < 2) put(w, c, r, O_STONE, (h >> 8) % 4);
            else if (h % 100 < 16) put(w, c, r, O_TUFT, (h >> 8) % 9);
        }
    // A river from north to south, winding (each row joined to the one before, so it stays one channel).
    // It crosses the main road's row straight (a bridge carries water across its deck only): a sideways run that
    // would fall on that row goes on the row above.
    int prev = -1;
    for (int r = 0; r < WORLD_H; r++) {
        int c = 40 + (int)(6 * sinf(r * 0.11f) + 4 * sinf(r * 0.27f + 1));
        if (r == ROAD_ROW && prev >= 0 && c != prev) {
            for (int k = prev; k != c; k += c > prev ? 1 : -1) lay(w, k, r - 1, G_RIVER);
            lay(w, c, r - 1, G_RIVER);
            prev = c;
        }
        if (prev >= 0)
            for (int k = prev; k != c; k += c > prev ? 1 : -1) lay(w, k, r, G_RIVER);
        lay(w, c, r, G_RIVER);
        prev = c;
    }
    // The roads: the main one from the west edge (where he starts) east through the world, one from the north
    // down to the village.
    for (int c = 3; c < 124; c++) lay(w, c, ROAD_ROW, G_ROAD);
    for (int r = 22; r < VC_R - 2; r++) lay(w, 83, r, G_ROAD);
    // Nothing tall right beside a road.
    for (int r = 0; r < WORLD_H; r++)
        for (int c = 0; c < WORLD_W; c++) {
            const cell_t *x = world_cell(w, c, r);
            bool by_road = false;
            for (int dr = -1; dr <= 1; dr++)
                for (int dc = -1; dc <= 1; dc++)
                    if (world_inside(c + dc, r + dr) && joins(world_cell(w, c + dc, r + dr)->ground, true)) by_road = true;
            if (by_road && x->obj != O_NONE && x->obj != O_TUFT) put_obj(w, c, r, O_NONE, FART_NONE);
        }
    // The village: a country one, north of the main road. Its fields first (round patches, a little ragged), then a
    // few short lanes that bend, then houses scattered along them, each turned to its lane, trees between.
    village(w, seed);
    for (int r = 0; r < WORLD_H; r++)
        for (int c = 0; c < WORLD_W; c++) resolve(w, c, r);
    // The player starts at the main road's west end, the village far off to the east.
    *start_u = 6.5f;
    *start_v = 64.5f;
    w->cache_ok = false;
}

// ── The player's changes ──

static void change(world_t *w, int c, int r, int ground, int var, int obj, int ovar, bool built)
{
    cell_t *x = world_cell(w, c, r);
    x->ground = (uint8_t)ground;
    x->var = (uint8_t)var;
    x->ovar = (uint8_t)ovar;
    put_obj(w, c, r, obj, world_obj_image(w, obj, ovar));
    x->flags = (uint8_t)((x->flags & F_TOWN) | (built ? F_BUILT : 0));
}

static bool record(world_t *w, int c, int r)
{
    const cell_t *x = world_cell(w, c, r);
    const edit_t e = { (uint8_t)c, (uint8_t)r, x->ground, x->var, x->obj, x->ovar, x->flags };
    for (int i = 0; i < w->nedits; i++)                           // changed before: the newest state replaces it
        if (w->edits[i].c == c && w->edits[i].r == r) {
            w->edits[i] = e;
            if (w->on_edit) w->on_edit(&e, w->on_edit_user);
            return true;
        }
    if (w->nedits >= EDITS_MAX) return false;
    w->edits[w->nedits++] = e;
    if (w->on_edit) w->on_edit(&e, w->on_edit_user);
    return true;
}

// A cell changed again as it now is (its var: joins), recorded and redrawn.
static void reset(world_t *w, int c, int r)
{
    const cell_t *y = world_cell(w, c, r);
    world_set(w, c, r, y->ground, y->var, y->obj, y->ovar, (y->flags & F_BUILT) != 0);
}

void world_merge(world_t *w, int c, int r)
{
    merge_t m;
    if (!merge_plan(w, c, r, &m)) return;
    for (int i = 0; i < m.n; i++) link(w, c, r, m.k[i], true);
    reset(w, c, r);
    for (int i = 0; i < m.n; i++) reset(w, c + s_dc[m.k[i]], r + s_dr[m.k[i]]);
    int gc[GROUP_MAX], gr[GROUP_MAX];
    const int n = world_group(w, c, r, gc, gr);                   // the whole building drawn again
    for (int i = 0; i < n; i++) world_retile(w, gc[i], gr[i]);
}

void world_unjoin(world_t *w, int c, int r)
{
    const cell_t *x = world_cell(w, c, r);
    if (!joinable(x) || !SIDES(x)) return;
    int gc[GROUP_MAX], gr[GROUP_MAX];
    const int n = world_group(w, c, r, gc, gr);
    const int sd = SIDES(x);
    for (int k = 0; k < 4; k++)
        if (sd & 1 << k) link(w, c, r, k, false);
    for (int k = 0; k < 4; k++)
        if (sd & 1 << k) reset(w, c + s_dc[k], r + s_dr[k]);
    reset(w, c, r);
    for (int i = 0; i < n; i++) world_retile(w, gc[i], gr[i]);
}

bool world_set(world_t *w, int c, int r, int ground, int var, int obj, int ovar, bool built)
{
    if (!world_inside(c, r)) return false;
    bool known = false;
    for (int i = 0; i < w->nedits && !known; i++) known = w->edits[i].c == c && w->edits[i].r == r;
    if (!known && w->nedits >= EDITS_MAX) return false;
    change(w, c, r, ground, var, obj, ovar, built);
    record(w, c, r);
    world_retile(w, c, r);
    return true;
}

void world_apply(world_t *w, const edit_t *e, int n)
{
    for (int i = 0; i < n && i < EDITS_MAX; i++) {
        if (!world_inside(e[i].c, e[i].r)) continue;
        int var = e[i].var;
        if ((e[i].obj == O_HOUSE || e[i].obj == O_SMITH) && (var & 3) && !(var & JOINED)) {
            // Kept by 0.3.5-0.3.6 as a shape (1 along rows, 2 along columns, 3 a square) and its column (bit 2) and
            // row (bit 3) in it: the same joins as sides.
            const int sh = var & 3, pc = (var >> 2) & 1, pr = (var >> 3) & 1;
            int sd = 0;
            if (sh == 1 || sh == 3) sd |= pc ? 8 : 2;
            if (sh == 2 || sh == 3) sd |= pr ? 1 : 4;
            var = JOINED | sd;
        }
        change(w, e[i].c, e[i].r, e[i].ground, var, e[i].obj, e[i].ovar, e[i].flags & F_BUILT);
        record(w, e[i].c, e[i].r);
    }
    for (int i = 0; i < n && i < EDITS_MAX; i++)
        if (world_inside(e[i].c, e[i].r)) world_retile(w, e[i].c, e[i].r);
    w->cache_ok = false;
}

bool world_walkable(world_t *w, float u, float v)
{
    const int c = (int)floorf(u), r = (int)floorf(v);
    if (!world_inside(c, r)) return false;
    const cell_t *x = world_cell(w, c, r);
    if (x->ground == G_RIVER) return false;
    // What stands blocks the middle of its cell: a building leaves a lane round it (he walks between two that stand
    // side by side), a tree or a stone hardly any (a forest stays shut).
    float in;
    switch (x->obj) {
    case O_NONE: case O_TUFT: case O_STAIR: return true;
    case O_HOUSE: case O_SMITH: case O_MILL: in = 0.18f; break;
    case O_WALL: if (wall_is_gate(w, c, r)) return true; in = 0; break;      // through a gate; walls join up
    case O_TOWER: in = 0; break;
    case O_STONE: in = 0.2f; break;
    default: in = 0.1f; break;
    }
    // A joined building has no lane between its cells.
    float lo_u = in, hi_u = in, lo_v = in, hi_v = in;
    if (joinable(x)) {
        const int sd = SIDES(x);
        if (sd & 1) lo_v = 0;
        if (sd & 2) hi_u = 0;
        if (sd & 4) hi_v = 0;
        if (sd & 8) lo_u = 0;
    }
    const float fu = u - c, fv = v - r;
    return fu <= lo_u || fu >= 1 - hi_u || fv <= lo_v || fv >= 1 - hi_v;
}

// ── Drawing ──

// The cell's anchor (its top face's centre), in world pixels.
static void anchor(int c, int r, int *x, int *y)
{
    int sc, sr;
    to_screen(c, r, &sc, &sr);
    *x = (sc - sr) * (TW / 2);
    *y = (sc + sr + 1) * (TH / 2);
}

// A mill's sails now (FART_NONE: not a mill, or no sails in the pack).
static uint16_t sails_of(const world_t *w, const cell_t *x)
{
    return x->obj == O_MILL ? w->sails[((x->ovar >> 2) ^ s_view) & 1][w->sail_frame] : FART_NONE;
}

// What stands on a cell, bottom to top, each image with its rise above the anchor: the thing, a mill's sails, a
// wall's levels on top of it and the tower on top of them.
// (dxs: across; dys: up.) A joined building's front cell gives the whole building's.
// A tower on top of a wall or a keep (dy: its foot's rise), and its flag waving at its top in the tower's colour.
static int tower_on(world_t *w, const cell_t *x, uint16_t *ims, int *dys, int n, int dy)
{
    const uint16_t t = STACK_TOWER(x) ? w->towers[(x->ovar >> 3) & 3][STACK_STYLE(x) % 9] : FART_NONE;
    if (t == FART_NONE || n >= 5) return n;
    ims[n] = t, dys[n++] = dy;
    const int f = flag_frame(&w->flag, w->t);
    if (f != FART_NONE) {
        w->part_pal[n] = (int8_t)(8 + ((x->ovar >> 3) & 3));
        ims[n] = (uint16_t)f, dys[n++] = dy - w->art->img[t].oy - 3;   // at the tower's top
    }
    return n;
}

// (w->part_pal[i]: the palette part i is drawn in, -1 as it is.)
static int thing_parts(world_t *w, int c, int r, uint16_t *ims, int *dxs, int *dys)
{
    for (int i = 0; i < 6; i++) w->part_pal[i] = -1;
    const cell_t *x = world_cell(w, c, r);
    int n = 0;
    for (int i = 0; i < 6; i++) dxs[i] = 0;
    for (int i = 0; i < 6; i++) dys[i] = 0;
    if (x->obj == O_HOUSE || x->obj == O_SMITH) {
        house_parts(w, c, r, ims, &n);
        return n;
    }
    if (x->obj == O_WALL) {                                      // a keep: each cell its piece, the front one its door
        int gc[4], gr[4], L;
        if (keep_at(w, c, r, false, gc, gr, &L)) {
            int ms = 1 << 30, mt = 1 << 30, sc, sr;
            for (int i = 0; i < 4; i++) {
                to_screen(gc[i], gr[i], &sc, &sr);
                if (sc < ms) ms = sc;
                if (sr < mt) mt = sr;
            }
            to_screen(c, r, &sc, &sr);
            const int du = sc - ms, dv = sr - mt;
            for (int l = 0; l < L && n < 4; l++) {               // on a castle's floor: the walls under it
                const int i = wall_image_at(w, c, r, l, WALL_PLAIN);
                if (i != FART_NONE) ims[n] = (uint16_t)i, dys[n++] = l * STACK_DY;
            }
            // Its two outer sides on screen (N or S, W or E): a lower wall against them (at its level)?
            const int da = dv ? 2 : 0, db = du ? 1 : 3;
            const int ka = (da - s_view) & 3, kb = (db - s_view) & 3;
            const int ext = (wall_joins(w, c + s_dc[ka], r + s_dr[ka], L) ? 1 : 0) | (wall_joins(w, c + s_dc[kb], r + s_dr[kb], L) ? 2 : 0);
            const int p = named(w, "kp%d.%d", du + 2 * dv, ext);
            if (p != FART_NONE) ims[n] = (uint16_t)p, dys[n++] = L * STACK_DY;
            if (du && dv) {                                      // the front cell: the door, the tower on the middle
                const int d = L ? FART_NONE : named(w, "kdS");
                if (d != FART_NONE) ims[n] = (uint16_t)d, dys[n++] = 0;
                n = tower_on(w, x, ims, dys, n, L * STACK_DY + TH / 2 + KEEP_H);
            }
            return n;
        }
    }
    if (x->obj_img != FART_NONE) ims[n] = x->obj_img, dys[n++] = 0;
    const uint16_t s = sails_of(w, x);
    if (s != FART_NONE) ims[n] = s, dys[n++] = 0;
    if (x->obj == O_WALL) {
        const int m = wall_mask_at(w, c, r, 0), kind = x->ovar & 3;
        if (wall_is_gate(w, c, r)) {                             // the arch on the face it is seen through
            const int g = named(w, (m & (E_ | W_)) ? "gS" : "gE");
            if (g != FART_NONE) ims[n] = (uint16_t)g, dys[n++] = 0;
        } else if (kind == WALL_WINDOW) {
            if (!(m & S_) && named(w, "lS") != FART_NONE) ims[n] = (uint16_t)named(w, "lS"), dys[n++] = 0;
            if (!(m & E_) && n < 6 && named(w, "lE") != FART_NONE) ims[n] = (uint16_t)named(w, "lE"), dys[n++] = 0;
        }
        for (int l = 1; l <= STACK_UP(x) && n < 5; l++) {
            const int i = wall_image_at(w, c, r, l, STACK_WIN(x) ? WALL_WINDOW : WALL_PLAIN);
            if (i != FART_NONE) ims[n] = (uint16_t)i, dys[n++] = l * STACK_DY;
        }
        n = tower_on(w, x, ims, dys, n, (1 + STACK_UP(x)) * STACK_DY);
    }
    return n;
}

// The palette swap for part i (thing_parts just before), or none: a building's (0..7), a flag's (8 + colour).
static void map_part(world_t *w, int i)
{
    const int p = w->part_pal[i];
    if (p >= 8 && w->flag.n) fart_map(w->flag.key, w->flag_col[p - 8], 2);
    else if (p >= 0 && p < 8 && w->pal_ok) fart_map(w->pal_key, w->pal[p], 4);
    else fart_map(NULL, NULL, 0);
}

static void draw_thing(world_t *w, uint16_t *dst, int stride, const fart_clip_t *clip, int c, int r, int ax, int ay)
{
    uint16_t ims[6];
    int dxs[6], dys[6];
    const int n = thing_parts(w, c, r, ims, dxs, dys);
    for (int i = 0; i < n; i++) {
        map_part(w, i);
        fart_draw(dst, stride, clip, &w->art->img[ims[i]], ax + dxs[i], ay - dys[i], false);
    }
    fart_map(NULL, NULL, 0);
}

// The box a thing covers from its anchor (all its parts).
static fart_clip_t thing_box(world_t *w, int c, int r)
{
    fart_clip_t b = { 0, 0, 0, 0 };
    uint16_t ims[6];
    int dxs[6], dys[6];
    const int n = thing_parts(w, c, r, ims, dxs, dys);
    for (int i = 0; i < n; i++) {
        const fart_img_t *im = &w->art->img[ims[i]];
        const int x0 = im->ox + dxs[i], y0 = im->oy - dys[i], x1 = im->ox + im->w + dxs[i], y1 = im->oy + im->h - dys[i];
        if (b.x0 == b.x1) b = (fart_clip_t){ x0, y0, x1, y1 };
        else {
            if (x0 < b.x0) b.x0 = x0;
            if (y0 < b.y0) b.y0 = y0;
            if (x1 > b.x1) b.x1 = x1;
            if (y1 > b.y1) b.y1 = y1;
        }
    }
    return b;
}

// The world's pixels in [x0, x1) x [y0, y1) (world pixels) into the cache: the sky, the ground, then what stands on
// it, back to front. What stands reaches up to ~160 px above its cell (a castle: its floor, keep, tower and flag) and
// blocks ~36 px below its top face.
static void render(world_t *w, int x0, int y0, int x1, int y1)
{
    if (x0 < w->cx0) x0 = w->cx0;
    if (y0 < w->cy0) y0 = w->cy0;
    if (x1 > w->cx0 + w->cw) x1 = w->cx0 + w->cw;
    if (y1 > w->cy0 + w->ch) y1 = w->cy0 + w->ch;
    if (x0 >= x1 || y0 >= y1) return;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) w->cache[(y - w->cy0) * w->cw + (x - w->cx0)] = w->sky;
    const fart_clip_t clip = { x0 - w->cx0, y0 - w->cy0, x1 - w->cx0, y1 - w->cy0 };
    int k0 = (y0 - 40) / (TH / 2) - 1, k1 = (y1 + 170) / (TH / 2) + 1;
    if (k0 < 0) k0 = 0;
    if (k1 > WORLD_W + WORLD_H - 2) k1 = WORLD_W + WORLD_H - 2;
    for (int pass = 0; pass < 2; pass++)
        for (int k = k0; k <= k1; k++) {
            int c0 = k - (WORLD_H - 1), c1 = k;
            if (c0 < 0) c0 = 0;
            if (c1 > WORLD_W - 1) c1 = WORLD_W - 1;
            for (int sc = c0; sc <= c1; sc++) {                  // screen cells, back to front
                int c, r, ax, ay;
                to_world(sc, k - sc, &c, &r);
                anchor(c, r, &ax, &ay);
                if (ax < x0 - 48 || ax > x1 + 48) continue;
                const cell_t *x = world_cell(w, c, r);
                if (pass) draw_thing(w, w->cache, w->cw, &clip, c, r, ax - w->cx0, ay - w->cy0);
                else if (x->img != FART_NONE) fart_draw(w->cache, w->cw, &clip, &w->art->img[x->img], ax - w->cx0, ay - w->cy0, false);
            }
        }
}

// The cache placed round the camera (camx, camy: the screen's top-left in world pixels): moved when the screen
// nears its edge, the part still in view kept and only the strips coming into view drawn.
static void place(world_t *w, int camx, int camy, int pw, int ph)
{
    if (!w->cache) {
        w->cw = pw + 2 * CACHE_MX;
        w->ch = ph + 2 * CACHE_MY;
        w->cache = ps_calloc((size_t)w->cw * w->ch * 2);
        if (!w->cache) return;
        w->cache_ok = false;
    }
    const int nx = camx - CACHE_MX, ny = camy - CACHE_MY;
    if (!w->cache_ok) {
        w->cx0 = nx;
        w->cy0 = ny;
        render(w, nx, ny, nx + w->cw, ny + w->ch);
        w->cache_ok = true;
        return;
    }
    if (camx >= w->cx0 + 16 && camy >= w->cy0 + 16 && camx + pw <= w->cx0 + w->cw - 16 && camy + ph <= w->cy0 + w->ch - 16)
        return;                                                       // still well inside
    const int dx = nx - w->cx0, dy = ny - w->cy0;
    if (abs(dx) >= w->cw || abs(dy) >= w->ch) {                       // a jump: all of it again
        w->cache_ok = false;
        place(w, camx, camy, pw, ph);
        return;
    }
    // The kept part moved by (-dx, -dy): new pixel (x, y) is old (x + dx, y + dy).
    const int xa = dx > 0 ? 0 : -dx, xb = dx > 0 ? w->cw - dx : w->cw, n = xb - xa;
    if (dy >= 0)
        for (int y = 0; y + dy < w->ch; y++)
            memmove(w->cache + y * w->cw + xa, w->cache + (y + dy) * w->cw + xa + dx, (size_t)n * 2);
    else
        for (int y = w->ch - 1; y + dy >= 0; y--)
            memmove(w->cache + y * w->cw + xa, w->cache + (y + dy) * w->cw + xa + dx, (size_t)n * 2);
    w->cx0 = nx;
    w->cy0 = ny;
    // The strips that came into view.
    if (dx > 0) render(w, nx + w->cw - dx, ny, nx + w->cw, ny + w->ch);
    if (dx < 0) render(w, nx, ny, nx - dx, ny + w->ch);
    if (dy > 0) render(w, nx, ny + w->ch - dy, nx + w->cw, ny + w->ch);
    if (dy < 0) render(w, nx, ny, nx + w->cw, ny - dy);
}

void world_retile(world_t *w, int c, int r)
{
    static const int8_t dc[5] = { 0, 0, 1, 0, -1 }, dr[5] = { 0, -1, 0, 1, 0 };
    for (int k = 0; k < 5; k++)
        if (world_inside(c + dc[k], r + dr[k])) resolve(w, c + dc[k], r + dr[k]);
    if (!w->cache_ok) return;
    int ax, ay;
    anchor(c, r, &ax, &ay);
    render(w, ax - 2 * TW, ay - 2 * TH - 170, ax + 2 * TW, ay + 2 * TH + 40);     // the cell, its neighbours, what stands
}

void world_draw(world_t *w, uint16_t *px, int pw, int ph, int camx, int camy)
{
    place(w, camx, camy, pw, ph);
    if (!w->cache) {
        for (int i = 0; i < pw * ph; i++) px[i] = w->sky;
        return;
    }
    for (int y = 0; y < ph; y++)
        memcpy(px + y * pw, w->cache + (camy - w->cy0 + y) * w->cw + (camx - w->cx0), (size_t)pw * 2);
}

// The cutaway's copy of a unit's box.
#define KEEP_MAX (96 * 96)
static uint16_t s_keep[KEEP_MAX];

void world_cover(world_t *w, uint16_t *px, int pw, int ph, int camx, int camy, float u, float v,
                 const fart_clip_t *box)
{
    fart_clip_t clip = *box;
    if (clip.x0 < 0) clip.x0 = 0;
    if (clip.y0 < 0) clip.y0 = 0;
    if (clip.x1 > pw) clip.x1 = pw;
    if (clip.y1 > ph) clip.y1 = ph;
    const int bw = clip.x1 - clip.x0, bh = clip.y1 - clip.y0;
    if (bw <= 0 || bh <= 0) return;
    const bool keep = bw * bh <= KEEP_MAX;
    bool covered = false;
    float su, sv;
    world_to_screen(u, v, &su, &sv);
    const int uc = (int)floorf(su), vr = (int)floorf(sv);
    // Things in front of it (their cell's centre deeper on screen than its feet) near enough to reach over it, back
    // to front.
    for (int k = uc + vr; k <= uc + vr + 12; k++)
        for (int sc = uc - 3; sc <= uc + 6; sc++) {
            const int sr = k - sc;
            if (sc < 0 || sr < 0 || sc >= WORLD_W || sr >= WORLD_H || sr < vr - 3 || (float)(sc + sr + 1) <= su + sv) continue;
            int c, r;
            to_world(sc, sr, &c, &r);
            const cell_t *x = world_cell(w, c, r);
            if (x->obj == O_NONE || x->obj == O_TUFT) continue;
            const fart_clip_t tb = thing_box(w, c, r);
            if (tb.x0 == tb.x1) continue;                         // a joined building's back cell: drawn by its front
            int ax, ay;
            anchor(c, r, &ax, &ay);
            ax -= camx;
            ay -= camy;
            if (ax + tb.x0 >= clip.x1 || ax + tb.x1 <= clip.x0 || ay + tb.y0 >= clip.y1 || ay + tb.y1 <= clip.y0)
                continue;
            if (keep && !covered)
                for (int y = 0; y < bh; y++) memcpy(s_keep + y * bw, px + (clip.y0 + y) * pw + clip.x0, (size_t)bw * 2);
            covered = true;
            draw_thing(w, px, pw, &clip, c, r, ax, ay);
        }
    if (!covered || !keep) return;
    // The cutaway: inside an ellipse fitted to the unit's box, every other pixel given back (every fourth at its edge).
    const int cx2 = box->x0 + box->x1, cy2 = box->y0 + box->y1, rw = box->x1 - box->x0, rh = box->y1 - box->y0;
    const int64_t r2 = (int64_t)rw * rw * rh * rh;
    for (int y = clip.y0; y < clip.y1; y++)
        for (int x = clip.x0; x < clip.x1; x++) {
            const int64_t dx = (int64_t)(2 * x + 1 - cx2) * rh, dy = (int64_t)(2 * y + 1 - cy2) * rw;
            const int64_t d = dx * dx + dy * dy;
            const bool half = d <= r2 * 7 / 10, quarter = d <= r2;
            if (half ? ((x ^ y) & 1) : quarter ? !(x & 1) && !(y & 1) : false)
                px[y * pw + x] = s_keep[(y - clip.y0) * bw + (x - clip.x0)];
        }
}

// ── What moves ──

// The cells whose anchor falls in [x0, x1) x [y0, y1) (world pixels), each given to fn.
static void each_cell(world_t *w, int x0, int y0, int x1, int y1, void (*fn)(world_t *, int, int, int, int, void *), void *u)
{
    int k0 = y0 / (TH / 2) - 1, k1 = y1 / (TH / 2) + 1;
    if (k0 < 0) k0 = 0;
    if (k1 > WORLD_W + WORLD_H - 2) k1 = WORLD_W + WORLD_H - 2;
    for (int k = k0; k <= k1; k++) {
        int c0 = k - (WORLD_H - 1), c1 = k;
        if (c0 < 0) c0 = 0;
        if (c1 > WORLD_W - 1) c1 = WORLD_W - 1;
        for (int sc = c0; sc <= c1; sc++) {
            int c, r, ax, ay;
            to_world(sc, k - sc, &c, &r);
            anchor(c, r, &ax, &ay);
            if (ax >= x0 && ax < x1 && ay >= y0 && ay < y1) fn(w, c, r, ax, ay, u);
        }
    }
}

static void redraw_mill(world_t *w, int c, int r, int ax, int ay, void *u)
{
    const cell_t *x = world_cell(w, c, r);
    if (u) {                                                     // flags: a tower on a wall or a keep
        if (x->obj != O_WALL || !STACK_TOWER(x)) return;
        const fart_clip_t b = thing_box(w, c, r);
        render(w, ax + b.x0, ay + b.y0, ax + b.x1, ay + b.y1);
        return;
    }
    if (x->obj != O_MILL) return;
    // Every frame's box: the last frame's pixels go as the next one's come.
    fart_clip_t b = thing_box(w, c, r);
    for (int i = 0; i < 8; i++) {
        const uint16_t k = w->sails[((x->ovar >> 2) ^ s_view) & 1][i];
        if (k == FART_NONE) continue;
        const fart_img_t *im = &w->art->img[k];
        if (im->ox < b.x0) b.x0 = im->ox;
        if (im->oy < b.y0) b.y0 = im->oy;
        if (im->ox + im->w > b.x1) b.x1 = im->ox + im->w;
        if (im->oy + im->h > b.y1) b.y1 = im->oy + im->h;
    }
    render(w, ax + b.x0, ay + b.y0, ax + b.x1, ay + b.y1);
}

#define SAIL_MS 150                  // a frame: a quarter turn in 1.2 s

void world_tick(world_t *w, uint32_t dt)
{
    const uint32_t was = w->t;
    w->t += dt;
    const int f = (int)(w->t / SAIL_MS % 8);
    const bool flags = w->flag.n && w->t / FLAG_MS != was / FLAG_MS;
    if (!w->cache_ok || !w->cache) {
        w->sail_frame = f;
        return;
    }
    // The mills (a new frame of their sails) and the flags in the cache (and a margin: a tower's anchor sits below
    // what moves), drawn again there.
    if (f != w->sail_frame) {
        w->sail_frame = f;
        each_cell(w, w->cx0 - 40, w->cy0 - 10, w->cx0 + w->cw + 40, w->cy0 + w->ch + 80, redraw_mill, NULL);
    }
    if (flags) each_cell(w, w->cx0 - 40, w->cy0 - 10, w->cx0 + w->cw + 40, w->cy0 + w->ch + 200, redraw_mill, w);
}

// Smoke: puffs rising from a smithy's chimney (its two facings: the chimney's top from the anchor), growing and
// thinning out (blended), a few at a time in turn.
typedef struct { uint16_t *px; int pw, ph, camx, camy; } smoke_ctx_t;
#define PUFFS 3
#define PUFF_MS 2700

static void smoke_at(world_t *w, int c, int r, int ax, int ay, void *u)
{
    const smoke_ctx_t *s = u;
    const cell_t *x = world_cell(w, c, r);
    if (x->obj != O_SMITH || w->smoke[0] == FART_NONE) return;
    const int chx = 0, chy = -CHIMNEY_TOP;                      // the chimney stands on the cell's middle
    const fart_clip_t clip = { 0, 0, s->pw, s->ph };
    const uint32_t off = (uint32_t)(c * 977 + r * 131);              // smithies not in step
    for (int i = 0; i < PUFFS; i++) {
        const uint32_t ms = (w->t + off + (uint32_t)i * PUFF_MS / PUFFS) % PUFF_MS;
        const int p = (int)(ms * 256 / PUFF_MS);                     // 0..255 through its rise
        const int px = ax + chx + p * 7 / 256 + ((p >> 5) & 1) - s->camx;
        const int py = ay + chy - p * 30 / 256 - s->camy;
        const int alpha = p < 32 ? p * 6 : 192 - (p - 32) * 192 / 224;   // in, then thinning out
        fart_draw_alpha(s->px, s->pw, &clip, &w->art->img[w->smoke[p * 3 / 256]], px, py, false, alpha);
    }
}

void world_smoke(world_t *w, uint16_t *px, int pw, int ph, int camx, int camy)
{
    smoke_ctx_t s = { px, pw, ph, camx, camy };
    each_cell(w, camx - 40, camy - 10, camx + pw + 40, camy + ph + 80, smoke_at, &s);
}

// ── A ghost of a change ──

#define MERGE_TINT 0x5e7f                // a joined building's ghost: a little toward sky blue

// Some cells' things, see-through, back to front on screen.
static void ghost_cells(world_t *w, uint16_t *px, int pw, int ph, int camx, int camy, int *dc, int *dr, int k, int alpha,
                        uint16_t tint, int tint_a)
{
    const fart_clip_t clip = { 0, 0, pw, ph };
    for (int a = 0; a < k; a++)
        for (int b = a + 1; b < k; b++) {
            int sa, ta, sb, tb;
            to_screen(dc[a], dr[a], &sa, &ta);
            to_screen(dc[b], dr[b], &sb, &tb);
            if (sb + tb < sa + ta) {
                int t = dc[a]; dc[a] = dc[b]; dc[b] = t;
                t = dr[a]; dr[a] = dr[b]; dr[b] = t;
            }
        }
    for (int a = 0; a < k; a++) {
        uint16_t ims[6];
        int dxs[6], dys[6], ax, ay;
        const int n = thing_parts(w, dc[a], dr[a], ims, dxs, dys);
        anchor(dc[a], dr[a], &ax, &ay);
        for (int i = 0; i < n; i++) {
            map_part(w, i);
            fart_draw_ghost(px, pw, &clip, &w->art->img[ims[i]], ax - camx + dxs[i], ay - camy - dys[i], false, alpha,
                            tint, tint_a);
        }
        fart_map(NULL, NULL, 0);
    }
}

void world_ghost(world_t *w, uint16_t *px, int pw, int ph, int camx, int camy, int c, int r, int ground, int var,
                 int obj, int ovar, int alpha, uint16_t tint, int tint_a)
{
    if (!world_inside(c, r)) return;
    int ax, ay;
    anchor(c, r, &ax, &ay);
    ax -= camx;
    ay -= camy;
    const fart_clip_t clip = { 0, 0, pw, ph };
    cell_t *x = world_cell(w, c, r);
    if (ground != x->ground) {                                   // a new ground: its piece as it would join
        const uint8_t was = x->ground;
        x->ground = (uint8_t)ground;
        const int i = ground_image(w, c, r, ground, var);
        x->ground = was;
        if (i != FART_NONE)                                      // its top face only (row by row, the diamond)
            for (int y = ay - TH / 2; y < ay + TH / 2; y++) {
                const int hw = (TH / 2 - abs(y - ay + (y < ay ? 0 : 1))) * TW / TH;
                fart_clip_t row = { ax - hw, y, ax + hw, y + 1 };
                if (row.x0 < 0) row.x0 = 0;
                if (row.x1 > pw) row.x1 = pw;
                if (y < 0 || y >= ph || row.x0 >= row.x1) continue;
                fart_draw_ghost(px, pw, &row, &w->art->img[i], ax, ay, false, alpha, tint, tint_a);
            }
    }
    if (obj == O_HOUSE || obj == O_SMITH) {                     // would it join? the whole building, tinted
        const cell_t keep = *x;
        x->obj = (uint8_t)obj;
        x->ovar = (uint8_t)ovar;
        x->var = 0;
        x->flags |= F_BUILT;
        merge_t m;
        if (merge_plan(w, c, r, &m)) {
            // Joined for a moment: it and the neighbours it joins (their pieces change), back to front.
            cell_t near[4];
            int dc[5], dr[5], k = 0;
            for (int i = 0; i < m.n; i++) near[i] = *world_cell(w, c + s_dc[m.k[i]], r + s_dr[m.k[i]]);
            for (int i = 0; i < m.n; i++) link(w, c, r, m.k[i], true);
            int gc[GROUP_MAX], gr[GROUP_MAX];
            if (big_house(w, c, r, gc, gr)) {
                const int f = deepest(gc, gr, 4);
                dc[k] = gc[f], dr[k++] = gr[f];
            } else {
                dc[k] = c, dr[k++] = r;
                for (int i = 0; i < m.n; i++) dc[k] = c + s_dc[m.k[i]], dr[k++] = r + s_dr[m.k[i]];
            }
            ghost_cells(w, px, pw, ph, camx, camy, dc, dr, k, alpha, tint_a ? tint : MERGE_TINT, tint_a ? tint_a : 70);
            for (int i = 0; i < m.n; i++) *world_cell(w, c + s_dc[m.k[i]], r + s_dr[m.k[i]]) = near[i];
            *x = keep;
            return;
        }
        *x = keep;
    }
    // The cell as it would be (a wall joined to its neighbours, its stack), drawn whole: what is there already shows
    // through solid under it, the new part faint. On a keep or a 2 x 2 house (a tower, an upper storey), on all four
    // for a moment, drawn by its front cell.
    const cell_t keep = *x;
    x->obj = (uint8_t)obj;
    x->ovar = (uint8_t)ovar;
    if (obj == O_WALL || obj == O_HOUSE) x->var = (uint8_t)var;
    x->obj_img = obj == O_WALL ? (uint16_t)wall_image_at(w, c, r, 0, ovar & 3) : (uint16_t)world_obj_image(w, obj, ovar);
    int gc[GROUP_MAX], gr[GROUP_MAX];
    const bool four = (obj == O_WALL && keep_at(w, c, r, true, gc, gr, NULL)) || (obj == O_HOUSE && big_house(w, c, r, gc, gr));
    if (four) {
        cell_t mates[4];
        for (int i = 0; i < 4; i++) {
            cell_t *y = world_cell(w, gc[i], gr[i]);
            mates[i] = *y;
            if (y == x) continue;
            if (obj == O_WALL) y->var = (uint8_t)((y->var & ~0x7c) | (x->var & 0x7c)), y->ovar = (uint8_t)((y->ovar & ~0x18) | (x->ovar & 0x18));
            else y->var = (uint8_t)((y->var & ~UPPER) | (x->var & UPPER));
        }
        const int f = deepest(gc, gr, 4);
        int dc = gc[f], dr = gr[f];
        ghost_cells(w, px, pw, ph, camx, camy, &dc, &dr, 1, alpha, tint, tint_a);
        for (int i = 0; i < 4; i++)
            if (world_cell(w, gc[i], gr[i]) != x) *world_cell(w, gc[i], gr[i]) = mates[i];
        *x = keep;
        return;
    }
    uint16_t ims[6];
    int dxs[6], dys[6];
    const int n = thing_parts(w, c, r, ims, dxs, dys);
    *x = keep;
    for (int i = 0; i < n; i++) {
        map_part(w, i);
        fart_draw_ghost(px, pw, &clip, &w->art->img[ims[i]], ax + dxs[i], ay - dys[i], false, alpha, tint, tint_a);
    }
    fart_map(NULL, NULL, 0);
}

int world_gate_way(world_t *w, int c, int r)
{
    if (!world_inside(c, r)) return -1;
    const int m = mask_of(w, c, r, true);
    const int way = m == 0 ? -1 : !(m & (E_ | W_)) ? 0 : !(m & (N_ | S_)) ? 1 : -1;   // the road N-S: a wall E-W
    if (way < 0) return -1;
    const bool across = way == 0 ? wall_joins(w, c, r - 1, 0) || wall_joins(w, c, r + 1, 0)
                                 : wall_joins(w, c - 1, r, 0) || wall_joins(w, c + 1, r, 0);
    return across ? -1 : way;
}

void world_view(world_t *w, int view)
{
    s_view = w->view = view & 3;
    for (int r = 0; r < WORLD_H; r++)
        for (int c = 0; c < WORLD_W; c++) resolve(w, c, r);
    w->cache_ok = false;
}

void world_spread(world_t *w, int c, int r)
{
    const cell_t *x = world_cell(w, c, r);
    int gc[GROUP_MAX], gr[GROUP_MAX];
    if (x->obj == O_WALL && keep_at(w, c, r, true, gc, gr, NULL)) {   // a tower on a keep: on all four of its cells
        for (int i = 0; i < 4; i++) {
            const cell_t *y = world_cell(w, gc[i], gr[i]);
            if (y == x) continue;
            world_set(w, gc[i], gr[i], y->ground, (y->var & ~0x7c) | (x->var & 0x7c), y->obj,
                      (y->ovar & ~0x18) | (x->ovar & 0x18), true);
        }
    } else if (x->obj == O_HOUSE && big_house(w, c, r, gc, gr)) {   // a 2 x 2 house's upper storey: all four
        for (int i = 0; i < 4; i++) {
            const cell_t *y = world_cell(w, gc[i], gr[i]);
            if (y != x) world_set(w, gc[i], gr[i], y->ground, (y->var & ~UPPER) | (x->var & UPPER), y->obj, y->ovar, true);
        }
    }
}

float world_height(world_t *w, float u, float v, float z)
{
    const int c = (int)floorf(u), r = (int)floorf(v);
    if (!world_inside(c, r)) return -1;
    const cell_t *x = world_cell(w, c, r);
    const float fu = u - c, fv = v - r;
    if (x->obj == O_STAIR) {
        const int d = x->ovar & 3;
        const float t = d == 0 ? 1 - fv : d == 1 ? fu : d == 2 ? fv : 1 - fu;
        const float across = (d & 1) ? fv : fu;                 // only between the steps' sides, once off the ground
        if ((across < 0.18f || across > 0.82f) && t * WALK_H > 3) return -1;
        return t * WALK_H;
    }
    if (x->obj == O_WALL) {
        // A walkway: a wall of one level, nothing on it, not part of a keep; a gate is a way through at the foot too.
        const bool walkway = STACK_UP(x) == 0 && !STACK_TOWER(x) && !world_is_keep(w, c, r);
        if (z < WALK_H / 2) return wall_is_gate(w, c, r) ? 0 : -1;
        return walkway ? WALK_H : -1;
    }
    return world_walkable(w, u, v) ? 0 : -1;
}

bool world_building(world_t *w, int c, int r, building_t *b)
{
    if (!world_inside(c, r)) return false;
    const cell_t *x = world_cell(w, c, r);
    int gc[GROUP_MAX], gr[GROUP_MAX], n = 1;
    memset(b, 0, sizeof(*b));
    gc[0] = c, gr[0] = r;
    b->floors = 1;
    b->walls = (x->ovar >> 4) & 1;
    b->roof = (x->ovar >> 2) & 3;
    if (x->obj == O_WALL) {
        int L;
        if (!keep_at(w, c, r, false, gc, gr, &L) || L != 0) return false;
        b->kind = B_KEEP;
        b->open = true;
        n = 4;
        b->floors = STACK_TOWER(x) ? 2 : 1;
        b->roof = (x->ovar >> 3) & 3;
    } else if (x->obj == O_HOUSE && big_house(w, c, r, gc, gr)) {
        b->kind = B_BIG;
        b->open = true;
        n = 4;
        b->floors = (x->var & UPPER) ? 2 : 1;
    } else if (x->obj == O_HOUSE || x->obj == O_SMITH) {
        b->kind = x->obj == O_SMITH ? B_SMITHY : B_HOUSE;
        if (x->obj == O_SMITH) b->walls = 0;
        if (SIDES(x)) n = world_group(w, c, r, gc, gr);
    } else
        return false;
    b->n = n;
    for (int i = 0; i < n; i++) {
        b->c[i] = (int16_t)gc[i];
        b->r[i] = (int16_t)gr[i];
        b->joins[i] = b->open ? 15 : (uint8_t)SIDES(world_cell(w, gc[i], gr[i]));
    }
    return true;
}

void world_reset(world_t *w, float *start_u, float *start_v)
{
    w->nedits = 0;
    world_make(w, start_u, start_v);
    world_view(w, w->view);
    w->cache_ok = false;
}
