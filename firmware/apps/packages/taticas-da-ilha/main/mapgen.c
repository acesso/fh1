// Battle maps: made from a seed (an island of hills, its tiles and objects by theme; on a small island the heroes
// start in one corner, the enemies in the far one; on a big one near its middle, a fixed way apart; a walkable way
// between them is checked), or drawn by hand (on a big map, set into an island). Then the
// realm's colours over the scenery: lava for water in the fire realm, the sea's blue, the forest's green, the
// heights' pale air.
#include "game.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static uint32_t s_rng;
static uint32_t rnd(void)                       // xorshift: the same map from the same seed on every FH1
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}
static float rndf(void) { return (rnd() & 0xffff) / 65536.0f; }

static int tile(int n) { char name[24]; snprintf(name, sizeof(name), "tile.%d", n); return fart_image(&g_art, name); }
static int prop(const char *name) { const int i = fart_image(&g_art, name); return i < 0 ? ISO_NONE : i; }

typedef struct { int water, fill, tops[4], ntops, high, objs[6], nobjs, obj_chance; } theme_t;

static void theme_of(int t, theme_t *th)
{
    memset(th, 0, sizeof(*th));
    th->water = tile(90);
    th->fill = tile(0);
    if (t == THEME_FOREST) {
        const int tops[4] = { tile(40), tile(22), tile(23), tile(40) }, objs[6] = { tile(43), tile(44), tile(52), tile(51), tile(42), tile(53) };
        memcpy(th->tops, tops, sizeof(tops));
        memcpy(th->objs, objs, sizeof(objs));
        th->ntops = 4, th->nobjs = 6, th->obj_chance = 6, th->high = tile(17);
    } else if (t == THEME_ROCKS) {
        const int tops[3] = { tile(11), tile(12), tile(17) }, objs[4] = { tile(54), tile(55), tile(65), tile(58) };
        memcpy(th->tops, tops, sizeof(tops));
        memcpy(th->objs, objs, sizeof(objs));
        th->ntops = 3, th->nobjs = 4, th->obj_chance = 10, th->high = tile(61);
    } else {
        const int tops[3] = { tile(22), tile(23), tile(24) }, objs[5] = { tile(41), tile(44), tile(46), tile(48), tile(42) };
        memcpy(th->tops, tops, sizeof(tops));
        memcpy(th->objs, objs, sizeof(objs));
        th->ntops = 3, th->nobjs = 5, th->obj_chance = 11, th->high = tile(17);
    }
}

static void paint(iso_map_t *m, const theme_t *th, int c, int r, int h, bool obj)
{
    iso_cell_t *cell = iso_cell(m, c, r);
    if (h <= 0) {
        *cell = (iso_cell_t){ 0, th->water, th->water, ISO_NONE, 0, 0 };
        return;
    }
    cell->h = h;
    cell->top = h >= 4 ? th->high : th->tops[rnd() % th->ntops];
    cell->fill = th->fill;
    cell->obj = obj ? th->objs[rnd() % th->nobjs] : ISO_NONE;
}

// Cells reachable from (c, r) climbing at most one level a step (the wolves' limit), objects and water excluded.
static int reach_count(iso_map_t *m, int c, int r, uint8_t *seen)
{
    static uint16_t q[36 * 36];                    // (the biggest islands)
    int head = 0, tail = 0, n = 0;
    memset(seen, 0, m->cols * m->rows);
    q[tail++] = r * m->cols + c;
    seen[r * m->cols + c] = 1;
    while (head < tail) {
        const int i = q[head++], ic = i % m->cols, ir = i / m->cols;
        n++;
        static const int8_t dc[4] = { 1, 0, -1, 0 }, dr[4] = { 0, 1, 0, -1 };
        for (int d = 0; d < 4; d++) {
            const int nc = ic + dc[d], nr = ir + dr[d], j = nr * m->cols + nc;
            if (!iso_inside(m, nc, nr) || seen[j]) continue;
            const iso_cell_t *a = &m->cells[i], *b = &m->cells[j];
            if (!b->h || b->obj != ISO_NONE || abs((int)a->h - (int)b->h) > 1) continue;
            seen[j] = 1;
            if (tail < (int)(sizeof(q) / sizeof(q[0]))) q[tail++] = j;
        }
    }
    return n;
}

static bool free_cell(iso_map_t *m, int c, int r) { return iso_inside(m, c, r) && iso_cell(m, c, r)->h && iso_cell(m, c, r)->obj == ISO_NONE; }

// The free cell nearest (c, r) that is reachable (seen), not already taken.
static bool spot_near(iso_map_t *m, const uint8_t *seen, int c, int r, uint8_t taken[][2], int ntaken, uint8_t out[2])
{
    for (int d = 0; d < m->cols + m->rows; d++)
        for (int dc = -d; dc <= d; dc++) {
            const int dr = d - abs(dc);
            for (int s = -1; s <= 1; s += 2) {
                const int cc = c + dc, rr = r + dr * s;
                if (!free_cell(m, cc, rr) || !seen[rr * m->cols + cc]) continue;
                bool used = false;
                for (int k = 0; k < ntaken; k++) used |= taken[k][0] == cc && taken[k][1] == rr;
                if (used) continue;
                out[0] = cc;
                out[1] = rr;
                return true;
            }
        }
    return false;
}

// The realm's own ground on a big island (none on a small one): the earth's high hills crowned with rocks, the
// water's lakes, the fire's lava pits (its water is lava), the air's winding chasms (cells with nothing in them: the
// sky far below). None can be walked on or through. Away from the island's middle, where the first wave fights.
static void features(iso_map_t *m, const battle_def_t *b, const theme_t *th)
{
    const int el = g_realms[b->realm].element, n = m->cols / 6 - 1;          // 24: 3, 36: 5
    const float cx = (m->cols - 1) / 2.0f, cy = (m->rows - 1) / 2.0f;
    for (int k = 0; k < n; k++) {
        int c0 = -1, r0 = 0;
        for (int tries = 0; tries < 60 && c0 < 0; tries++) {
            const int c = 3 + (int)(rnd() % (m->cols - 6)), r = 3 + (int)(rnd() % (m->rows - 6));
            if ((fabsf(c - cx) < 8 && fabsf(r - cy) < 8) || !iso_cell(m, c, r)->h) continue;
            c0 = c, r0 = r;
        }
        if (c0 < 0) continue;
        if (el == EL_AIR) {                                      // a chasm winding across the land
            int c = c0, r = r0, dir = (int)(rnd() % 4);
            const int len = 12 + (int)(rnd() % 10);
            static const int8_t dc[4] = { 1, 0, -1, 0 }, dr[4] = { 0, 1, 0, -1 };
            for (int s = 0; s < len; s++) {
                for (int w = 0; w < 1 + (rnd() % 4 == 0); w++) {
                    const int cc = c + w * dr[dir], rr = r + w * dc[dir];
                    if (iso_inside(m, cc, rr) && !(fabsf(cc - cx) < 7 && fabsf(rr - cy) < 7))
                        *iso_cell(m, cc, rr) = (iso_cell_t){ 0, ISO_NONE, ISO_NONE, ISO_NONE, 0, 0 };
                }
                if (rnd() % 3 == 0) dir = (dir + (rnd() % 2 ? 1 : 3)) % 4;   // a bend
                c += dc[dir], r += dr[dir];
                if (c < 1 || r < 1 || c >= m->cols - 1 || r >= m->rows - 1) break;
            }
            continue;
        }
        const float rad = el == EL_EARTH ? 2.0f + rndf() * 1.2f : el == EL_FIRE ? 1.3f + rndf() * 1.0f : 1.8f + rndf() * 1.4f;
        for (int r = r0 - 4; r <= r0 + 4; r++)
            for (int c = c0 - 4; c <= c0 + 4; c++) {
                const float d = sqrtf((float)((c - c0) * (c - c0) + (r - r0) * (r - r0))) + (rndf() - 0.5f) * 0.6f;
                if (d > rad || !iso_inside(m, c, r) || (fabsf(c - cx) < 7 && fabsf(r - cy) < 7)) continue;
                if (el == EL_EARTH) {                            // a high hill, rocks all over its top
                    paint(m, th, c, r, 5 - (int)(d * 0.8f), false);
                    iso_cell(m, c, r)->obj = rnd() % 3 ? tile(65) : tile(54);
                } else {                                         // a lake, or a pit of lava
                    paint(m, th, c, r, 0, false);
                }
            }
    }
}

// An island of hills from the battle's seed and an attempt: a gentle island round the middle (smaller in the water
// realm, its hills taller in the air realm), a few bumps for every 12 x 12 of it.
static void island(iso_map_t *m, const battle_def_t *b, const theme_t *th, uint32_t attempt)
{
    s_rng = 0x9e3779b9u ^ (b->seed * 2654435761u) ^ (attempt * 40503u);
    if (!s_rng) s_rng = 1;
    enum { HILLS = 48 };
    float hx[HILLS], hy[HILLS], hh[HILLS], hr[HILLS];
    int nh = 3 + rnd() % 3;
    if (m->cols * m->rows > 14 * 14) nh = nh * m->cols * m->rows / 144;   // (a small island's as it always was)
    if (nh > HILLS) nh = HILLS;
    const float shore = b->realm == EL_WATER - 1 ? 0.86f : 0.98f, tall = b->realm == EL_AIR - 1 ? 3.6f : 2.6f;
    for (int k = 0; k < nh; k++) {
        hx[k] = rndf() * m->cols;
        hy[k] = rndf() * m->rows;
        hh[k] = 1.0f + rndf() * tall;
        hr[k] = 2.0f + rndf() * 3.5f;
    }
    const float cx = (m->cols - 1) / 2.0f, cy = (m->rows - 1) / 2.0f;
    for (int r = 0; r < m->rows; r++)
        for (int c = 0; c < m->cols; c++) {
            const float dx = (c - cx) / (cx + 0.5f), dy = (r - cy) / (cy + 0.5f);
            const float edge = sqrtf(dx * dx + dy * dy) + (rndf() - 0.5f) * 0.18f;
            if (edge > shore) {
                paint(m, th, c, r, 0, false);
                continue;
            }
            float h = 1.0f;
            for (int k = 0; k < nh; k++) {
                const float d = sqrtf((c - hx[k]) * (c - hx[k]) + (r - hy[k]) * (r - hy[k]));
                if (d < hr[k]) h += hh[k] * (1.0f - d / hr[k]);
            }
            int hi = (int)h;
            if (hi > 5) hi = 5;
            paint(m, th, c, r, hi, (int)(rnd() % th->obj_chance) == 0);
        }
    if (m->cols * m->rows > 14 * 14) features(m, b, th);
}

// Where the heroes gather and where the enemies wait: the near and far corners of a small island; on a big one,
// near its middle, about as far apart as on a small one (fourteen steps), the rest of the island room round them.
static void meeting(const iso_map_t *m, int *hc, int *hr, int *fc, int *fr)
{
    if (m->cols <= 14) {
        *hc = m->cols / 4, *hr = m->rows - 1 - m->rows / 4, *fc = m->cols - 1 - m->cols / 4, *fr = m->rows / 4;
        return;
    }
    *hc = m->cols / 2 - 4, *hr = m->rows / 2 + 4;
    *fc = *hc + 7, *fr = *hr - 7;
}

static void build_random(iso_map_t *m, const battle_def_t *b, uint8_t spots[PARTY_MAX][2], uint8_t foes[FOES_MAX][2])
{
    theme_t th;
    theme_of(b->theme, &th);
    static uint8_t seen[36 * 36];
    for (uint32_t attempt = 0; attempt < 12; attempt++) {
        island(m, b, &th, attempt);
        int hc, hr2, fc, fr;
        meeting(m, &hc, &hr2, &fc, &fr);
        uint8_t start[2];
        uint8_t none[1][2];
        if (!spot_near(m, (memset(seen, 1, sizeof(seen)), seen), hc, hr2, none, 0, start)) continue;
        if (reach_count(m, start[0], start[1], seen) < m->cols * m->rows / 3) continue;   // too broken up
        uint8_t taken[PARTY_MAX + FOES_MAX][2];
        int nt = 0;
        bool ok = true;
        for (int k = 0; k < PARTY_MAX && ok; k++) {
            ok = spot_near(m, seen, hc, hr2, taken, nt, spots[k]);
            if (ok) memcpy(taken[nt++], spots[k], 2);
        }
        for (int k = 0; k < (b->n1 ? b->n1 : b->n) && ok; k++) {    // (a second wave comes later, elsewhere)
            ok = spot_near(m, seen, fc, fr, taken, nt, foes[k]);
            if (ok) memcpy(taken[nt++], foes[k], 2);
        }
        if (ok) return;
    }
    // No good island from this seed: a flat one.
    for (int r = 0; r < m->rows; r++)
        for (int c = 0; c < m->cols; c++) paint(m, &th, c, r, 1, false);
    for (int k = 0; k < PARTY_MAX; k++) spots[k][0] = 1 + k, spots[k][1] = m->rows - 2;
    for (int k = 0; k < (b->n1 ? b->n1 : b->n); k++) foes[k][0] = m->cols - 2 - k, foes[k][1] = 1;
}

// A map drawn by hand (an arena: acols x arows); on a bigger map, set in the middle of an island of the battle's
// size, its own water giving way to the island's ground.
static void build_fixed(iso_map_t *m, const battle_def_t *b, uint8_t spots[PARTY_MAX][2], uint8_t foes[FOES_MAX][2])
{
    theme_t th;
    theme_of(b->theme, &th);
    const int ac = b->acols ? b->acols : m->cols, ar = b->arows ? b->arows : m->rows;
    const int ox = (m->cols - ac) / 2, oy = (m->rows - ar) / 2;
    if (ox || oy) island(m, b, &th, 0);
    s_rng = 12345;
    for (int r = 0; r < ar; r++)
        for (int c = 0; c < ac; c++) {
            const int h = b->heights[r][c] - '0';
            const char o = b->objects ? b->objects[r][c] : '.';
            if ((ox || oy) && h <= 0) continue;                  // (the island's ground round the arena)
            paint(m, &th, c + ox, r + oy, h, false);
            iso_cell_t *cell = iso_cell(m, c + ox, r + oy);
            if (h > 0 && o == 'r') cell->obj = tile(65);
            else if (h > 0 && o == 'p') cell->obj = tile(43);
            else if (h > 0 && o == 's') cell->obj = tile(52);
            else if (h > 0 && (o == 'f' || o == 'b' || o == 'B')) {   // a slab one block thick, floating
                cell->base = (uint8_t)h;
                cell->bob = (uint8_t)(o == 'b' ? 1 : o == 'B' ? 2 : 0);
            }
        }
    for (int k = 0; k < PARTY_MAX; k++) spots[k][0] = b->heroes[k][0] + ox, spots[k][1] = b->heroes[k][1] + oy;
    for (int k = 0; k < (b->n1 ? b->n1 : b->n); k++) foes[k][0] = b->foes[k].c + ox, foes[k][1] = b->foes[k].r + oy;
}

// One scenery pixel through the realm's colours.
static uint16_t grade_px(uint16_t v, void *user)
{
    const int el = *(const int *)user;
    int r = (v >> 11) << 3, g = ((v >> 5) & 63) << 2, b = (v & 31) << 3;
    const bool water = b > r + 24 && b >= g;
    const int lum = (r * 3 + g * 6 + b) / 10;
    if (el == EL_FIRE) {
        if (water) {                                             // lava: dark red to yellow by brightness
            r = 120 + lum;
            g = lum * 3 / 4;
            b = lum / 8;
        } else {                                                 // scorched: warm and dark
            r = r * 9 / 10 + 18;
            g = g * 7 / 10;
            b = b / 2;
        }
    } else if (el == EL_WATER) {
        r = r * 8 / 10;
        g = g * 9 / 10 + 6;
        b = b + 20;
    } else if (el == EL_EARTH) {
        r = r * 9 / 10;
        g = g + 10;
        b = b * 8 / 10;
    } else {                                                     // air: pale, cold, light
        r = r + (200 - r) / 4;
        g = g + (215 - g) / 4;
        b = b + (240 - b) / 4;
    }
    r = r > 255 ? 255 : r;
    g = g > 255 ? 255 : g;
    b = b > 255 ? 255 : b;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// The realm's colours: over the scenery's images (so the layer, its redraws and the scenery drawn over units all
// have them) and the sky.
void map_grade(iso_map_t *m, int realm)
{
    static int el;
    el = g_realms[realm].element;
    m->sky = g_realms[realm].sky;
    iso_grade(m, grade_px, &el);
}

// The water by its depth: next to land (the eight cells round), shallow and light, its swell rolling along the
// shore, now and then a rock standing in it; further out, deep and dark, glinting here and there.
static void shore(iso_map_t *m)
{
    const int shallow = tile(88), deep = tile(94);
    static const int rocks[5] = { 77, 78, 79, 80, 81 };
    for (int r = 0; r < m->rows; r++)
        for (int c = 0; c < m->cols; c++) {
            iso_cell_t *cell = iso_cell(m, c, r);
            if (cell->h > 0 || (cell->top == ISO_NONE && cell->fill == ISO_NONE)) continue;   // (land, a chasm)
            bool land = false;
            for (int dr = -1; dr <= 1 && !land; dr++)
                for (int dc = -1; dc <= 1 && !land; dc++)
                    land = iso_inside(m, c + dc, r + dr) && iso_cell(m, c + dc, r + dr)->h > 0;
            cell->top = cell->fill = land ? shallow : deep;
            if (land && rnd() % 14 == 0) cell->top = tile(rocks[rnd() % 5]);
        }
}

// The showcase's stage: the cells round the heroes and their creatures made flat ground, without objects.
static void stage(iso_map_t *m, const battle_def_t *b)
{
    theme_t th;
    theme_of(b->theme, &th);
    uint8_t h[4][2], f[4][2];
    show_spots(b, h, f);
    for (int r = f[0][1] - 1; r <= h[0][1] + 1; r++)
        for (int c = h[0][0] - 1; c <= h[3][0] + 1; c++)
            if (iso_inside(m, c, r)) {
                paint(m, &th, c, r, 2, false);
                iso_cell(m, c, r)->base = iso_cell(m, c, r)->bob = 0;
            }
    // Props from the portal pack: a portal's frame behind the creatures' middle between two broken pillars (battle.c
    // draws its vortex, the engine's portal), a campfire past each end of the heroes' row (battle.c flickers the
    // flames and sends sparks up from them).
    const int pc = h[0][0] + 3, pr = f[0][1] - 2;
    const struct { int c, r; const char *name; } props[] = {
        { pc, pr, "prop.portal.frame" }, { pc - 2, pr, "prop.pillar.broken" }, { pc + 2, pr, "prop.pillar.broken" },
        { h[0][0] - 2, h[0][1], "prop.campfire" }, { h[3][0] + 2, h[3][1], "prop.campfire" },
    };
    for (unsigned k = 0; k < sizeof(props) / sizeof(props[0]); k++) {
        if (!iso_inside(m, props[k].c, props[k].r)) continue;
        for (int dr = -1; dr <= 0; dr++)
            for (int dc = -1; dc <= 1; dc++)
                if (iso_inside(m, props[k].c + dc, props[k].r + dr)) {
                    paint(m, &th, props[k].c + dc, props[k].r + dr, 2, false);
                    iso_cell(m, props[k].c + dc, props[k].r + dr)->base = iso_cell(m, props[k].c + dc, props[k].r + dr)->bob = 0;
                }
    }
    for (unsigned k = 0; k < sizeof(props) / sizeof(props[0]); k++)
        if (iso_inside(m, props[k].c, props[k].r)) iso_cell(m, props[k].c, props[k].r)->obj = (uint16_t)prop(props[k].name);
}

void map_build(iso_map_t *m, const battle_def_t *b, uint8_t spots[PARTY_MAX][2], uint8_t foes[FOES_MAX][2])
{
    if (b->map == MAP_FIXED) build_fixed(m, b, spots, foes);
    else build_random(m, b, spots, foes);
    if (b->show) stage(m, b);
    shore(m);
}

// The first fase's end: the island grows round the arena to size x size. The arena's land stays as it is; the new
// land (an island of the second fase's kind, its shore) is listed to rise out of the sea in waves from the old
// island outward: cells (r * cols + c) and each one's delay (ms). Returns how many (0: no room, or the map could
// not grow).
int map_grow(iso_map_t *m, const battle_def_t *b, int size, uint16_t *cells, uint16_t *delay, int max)
{
    theme_t th;
    theme_of(b->theme, &th);
    const iso_cell_t sea = { 0, th.water, th.water, ISO_NONE, 0, 0 };
    const int oc = m->cols, orr = m->rows, dc = (size - oc) / 2, dr = (size - orr) / 2;
    iso_cell_t *keep = malloc((size_t)oc * orr * sizeof(iso_cell_t));
    if (!keep) return 0;
    memcpy(keep, m->cells, (size_t)oc * orr * sizeof(iso_cell_t));
    if (!iso_resize(m, size, size, dc, dr, &sea)) {
        free(keep);
        return 0;
    }
    battle_def_t big = *b;
    big.cols = big.rows = (uint8_t)size;
    big.seed = (uint16_t)(b->seed * 7 + 101);
    island(m, &big, &th, 0);
    for (int r = 0; r < orr; r++)                                // the arena's land as it was
        for (int c = 0; c < oc; c++)
            if (keep[r * oc + c].h) *iso_cell(m, c + dc, r + dr) = keep[r * oc + c];
    shore(m);
    // The new land, nearest the old island first, a little jitter in each wave.
    const float cx = (size - 1) / 2.0f, cy = (size - 1) / 2.0f;
    int n = 0;
    for (int r = 0; r < size && n < max; r++)
        for (int c = 0; c < size && n < max; c++) {
            const int oc2 = c - dc, or2 = r - dr;
            const bool was = oc2 >= 0 && or2 >= 0 && oc2 < oc && or2 < orr && keep[or2 * oc + oc2].h;
            if (was || !iso_cell(m, c, r)->h) continue;
            const float d = sqrtf((c - cx) * (c - cx) + (r - cy) * (r - cy)) - oc / 2.0f;
            cells[n] = (uint16_t)(r * size + c);
            delay[n++] = (uint16_t)((d < 0 ? 0 : d) * 170 + (rnd() % 160));
        }
    free(keep);
    return n;
}
