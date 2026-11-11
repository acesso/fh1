// Battle maps: made from a seed (an island of hills, its tiles and objects by theme; the heroes start in one
// corner, the enemies in the far one, and a walkable way between them is checked), or drawn by hand. Then the
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
    int q[256], head = 0, tail = 0, n = 0;
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
            if (tail < 256) q[tail++] = j;
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

static void build_random(iso_map_t *m, const battle_def_t *b, uint8_t spots[3][2], uint8_t foes[6][2])
{
    theme_t th;
    theme_of(b->theme, &th);
    static uint8_t seen[16 * 16];
    for (uint32_t attempt = 0; attempt < 12; attempt++) {
        s_rng = 0x9e3779b9u ^ (b->seed * 2654435761u) ^ (attempt * 40503u);
        if (!s_rng) s_rng = 1;
        // Hills: a few bumps over a gentle island.
        float hx[5], hy[5], hh[5], hr[5];
        const int nh = 3 + rnd() % 3;
        // The water realm's islands are smaller (more sea round them), the air realm's hills taller.
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
                    paint(m, &th, c, r, 0, false);
                    continue;
                }
                float h = 1.0f;
                for (int k = 0; k < nh; k++) {
                    const float d = sqrtf((c - hx[k]) * (c - hx[k]) + (r - hy[k]) * (r - hy[k]));
                    if (d < hr[k]) h += hh[k] * (1.0f - d / hr[k]);
                }
                int hi = (int)h;
                if (hi > 5) hi = 5;
                paint(m, &th, c, r, hi, (int)(rnd() % th.obj_chance) == 0);
            }
        // Heroes in the lower-left corner (toward the viewer), enemies in the upper-right one.
        const int hc = m->cols / 4, hr2 = m->rows - 1 - m->rows / 4, fc = m->cols - 1 - m->cols / 4, fr = m->rows / 4;
        uint8_t start[2];
        uint8_t none[1][2];
        if (!spot_near(m, (memset(seen, 1, sizeof(seen)), seen), hc, hr2, none, 0, start)) continue;
        if (reach_count(m, start[0], start[1], seen) < m->cols * m->rows / 3) continue;   // too broken up
        uint8_t taken[9][2];
        int nt = 0;
        bool ok = true;
        for (int k = 0; k < 3 && ok; k++) {
            ok = spot_near(m, seen, hc, hr2, taken, nt, spots[k]);
            if (ok) memcpy(taken[nt++], spots[k], 2);
        }
        for (int k = 0; k < b->n && ok; k++) {
            ok = spot_near(m, seen, fc, fr, taken, nt, foes[k]);
            if (ok) memcpy(taken[nt++], foes[k], 2);
        }
        if (ok) return;
    }
    // No good island from this seed: a flat one.
    for (int r = 0; r < m->rows; r++)
        for (int c = 0; c < m->cols; c++) paint(m, &th, c, r, 1, false);
    for (int k = 0; k < 3; k++) spots[k][0] = 1 + k, spots[k][1] = m->rows - 2;
    for (int k = 0; k < b->n; k++) foes[k][0] = m->cols - 2 - k, foes[k][1] = 1;
}

static void build_fixed(iso_map_t *m, const battle_def_t *b, uint8_t spots[3][2], uint8_t foes[6][2])
{
    theme_t th;
    theme_of(b->theme, &th);
    s_rng = 12345;
    for (int r = 0; r < m->rows; r++)
        for (int c = 0; c < m->cols; c++) {
            const int h = b->heights[r][c] - '0';
            const char o = b->objects ? b->objects[r][c] : '.';
            paint(m, &th, c, r, h, false);
            iso_cell_t *cell = iso_cell(m, c, r);
            if (h > 0 && o == 'r') cell->obj = tile(65);
            else if (h > 0 && o == 'p') cell->obj = tile(43);
            else if (h > 0 && o == 's') cell->obj = tile(52);
            else if (h > 0 && (o == 'f' || o == 'b' || o == 'B')) {   // a slab one block thick, floating
                cell->base = (uint8_t)h;
                cell->bob = (uint8_t)(o == 'b' ? 1 : o == 'B' ? 2 : 0);
            }
        }
    for (int k = 0; k < 3; k++) memcpy(spots[k], b->heroes[k], 2);
    for (int k = 0; k < b->n; k++) foes[k][0] = b->foes[k].c, foes[k][1] = b->foes[k].r;
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

void map_build(iso_map_t *m, const battle_def_t *b, uint8_t spots[3][2], uint8_t foes[6][2])
{
    if (b->map == MAP_FIXED) build_fixed(m, b, spots, foes);
    else build_random(m, b, spots, foes);
}
