// The garden and sci-fi looks' pictures (ui_look.h). Everything is drawn on a unit grid and blown up by `u`.
//
// Garden: wooden frames with their grain, a vine that grows round the window as it opens, a flower on each corner
// (a bud on windows out of focus, open on the one in focus, swaying; red roses on an alert), a wooden sign for
// the title, a grass desktop with tiny flowers, a ladybird for a cursor.
// Sci-fi: gunmetal plates that close from the corners, brackets on the corners with a status light, a cyan line of
// light along the inside, the title on an angled tab with three status lights, a dark grid desktop, a chevron cursor.
#include <stdbool.h>
#include <string.h>
#include "ui_look.h"

#define ARGB(c) (0xff000000u | (uint32_t)(c))

// ── Colour helpers ──

static uint32_t mix(uint32_t a, uint32_t b, int t)        // t/256 of b over a
{
    const int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255, br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
    return (uint32_t)((ar + (br - ar) * t / 256) << 16 | (ag + (bg - ag) * t / 256) << 8 | (ab + (bb - ab) * t / 256));
}

// A colour for a window out of focus (dimmed, greyer) or an alert (pulled towards red).
static uint32_t state_tint(uint32_t c, int state, uint32_t alert)
{
    if (state == 0) return mix(mix(c, 0x6e6a62, 90), 0x000000, 60);
    if (state == 2) return mix(c, alert, 70);
    return c;
}

static uint32_t hash(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16;
    return x;
}

// One unit at (x, y) of a W x H unit picture, as u x u pixels.
static void put(uint32_t *px, int W, int H, int u, int x, int y, uint32_t argb)
{
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    for (int sy = 0; sy < u; sy++)
        for (int sx = 0; sx < u; sx++) px[(y * u + sy) * W * u + x * u + sx] = argb;
}

static void put16(uint16_t *px, int W, int u, int x, int y, uint32_t rgb)
{
    const uint16_t c = (uint16_t)(((rgb >> 8) & 0xf800) | ((rgb >> 5) & 0x07e0) | ((rgb >> 3) & 0x001f));
    for (int sy = 0; sy < u; sy++)
        for (int sx = 0; sx < u; sx++) px[(y * u + sy) * W * u + x * u + sx] = c;
}

// Draws a picture given as rows of characters (one unit each), mapping characters to colours; ' ' and '.' skip.
static void art(uint32_t *px, int W, int H, int u, int x0, int y0, const char *const *rows, int n, bool mirror,
                const char *keys, const uint32_t *cols)
{
    for (int y = 0; y < n; y++) {
        const int w = (int)strlen(rows[y]);
        for (int x = 0; x < w; x++) {
            const char *k = strchr(keys, rows[y][x]);
            if (!k || rows[y][x] == '.' || rows[y][x] == ' ') continue;
            put(px, W, H, u, x0 + (mirror ? w - 1 - x : x), y0 + y, ARGB(cols[k - keys]));
        }
    }
}

// ── Garden ──

#define G_OUT 0x3a2412          // the wood's outline
#define G_LIGHT 0xcf9a58        // its lit edge
#define G_WOOD 0xa06a36
#define G_WOOD2 0x93602f
#define G_GRAIN 0x7a4f26
#define G_DARK 0x5c3a1b
#define G_SHADE 0x7a4f26        // the edge away from the light
#define G_VINE 0x2f6b25
#define G_VINE2 0x4f8f2c
#define G_LEAF 0x6cc04a
#define G_LEAF2 0x3f8f2f
#define G_LEAF3 0x2a6a22
#define G_THICK 5
#define G_OUTSIDE 2

// The vine's place along the perimeter, clockwise from the top-left (units).
static int perimeter_pos(int side, int a, int lw, int lh)
{
    switch (side) {
    case 0: return a;
    case 3: return lw + a;
    case 1: return lw + lh + (lw - 1 - a);
    default: return 2 * lw + lh + (lh - 1 - a);
    }
}

// Rows 0..1 hang outside the box (leaves only), 2 the outline, 3 the lit edge, 4..5 the plank, 6 its inner shadow.
static void garden_strip(const look_pal_t *p, int side, int len, int w, int h, int state, int grow, uint32_t *px)
{
    const int u = p->u, L = len / u, T = G_THICK + G_OUTSIDE, lw = w / u, lh = h / u;
    const bool horiz = side < 2;
    const int W = horiz ? L : T, H = horiz ? T : L;
    memset(px, 0, (size_t)W * u * H * u * 4);
    const int P = 2 * (lw + lh), reach = P * grow / 256;
    const bool lit = side == 0 || side == 2;                 // light from the top-left
#define PUT(a_, r_, c_) put(px, W, H, u, horiz ? (a_) : (r_), horiz ? (r_) : (a_), ARGB(state_tint((c_), state, p->alert)))
    for (int a = 0; a < L; a++) {
        const uint32_t hsh = hash((uint32_t)(a / 4) * 2654435761u + (uint32_t)side * 97u + 7u), h2 = hash((uint32_t)a * 40503u + (uint32_t)side);
        for (int r = G_OUTSIDE; r < T; r++) {
            uint32_t c;
            switch (r - G_OUTSIDE) {
            case 0: c = G_OUT; break;
            case 1: c = lit ? G_LIGHT : G_SHADE; if (h2 % 9 == 0) c = G_WOOD; break;
            case 2: c = (hsh % 4 == 0) ? G_GRAIN : G_WOOD; break;
            case 3: c = (hsh % 5 == 1 || h2 % 17 == 0) ? G_GRAIN : G_WOOD2; break;
            default: c = G_DARK; break;
            }
            if (r - G_OUTSIDE >= 1 && r - G_OUTSIDE <= 3 && (a % 41) == 40) c = G_DARK;        // planks meet
            if (r - G_OUTSIDE == 2 && (a % 41) == 0) c = G_OUT;                                 // a knot by the joint
            PUT(a, r, c);
        }
        // The vine winds across the plank (rows 3..5), a leaf every 7 units, hanging out over the edge on one side
        // and lying on the wood on the other, as far as it has grown round.
        const int pos = perimeter_pos(side, a, lw, lh);
        if (pos >= reach) continue;
        static const int wind[10] = { 4, 4, 3, 3, 3, 4, 4, 5, 5, 5 };
        const int rv = G_OUTSIDE + wind[pos % 10] - 1;
        PUT(a, rv, (pos % 10) < 5 ? G_VINE : G_VINE2);
        const int m = pos % 14;
        if (m >= 2 && m <= 4) {                          // a leaf out over the edge: tip, blade, stalk
            if (m == 3) PUT(a, 0, G_LEAF);
            PUT(a, 1, m == 2 ? G_LEAF2 : m == 3 ? G_LEAF : G_LEAF3);
            PUT(a, 2, G_LEAF2);
        } else if (m >= 9 && m <= 10) {                   // a leaf lying on the wood
            PUT(a, G_OUTSIDE + 4, m == 9 ? G_LEAF : G_LEAF2);
        }
        // now and then a tiny bloom on the window in focus, its petals opening and closing with the phase
        if (state == 1 && m == 12 && (pos / 14) % 3 == 1) {
            const uint32_t bloom = ((pos / 42) & 1) ? 0xfff4f8 : 0xffd23f;
            PUT(a, G_OUTSIDE + 3, bloom);
            PUT(a, G_OUTSIDE + 2, mix(bloom, 0xffd23f, 90));
        }
    }
#undef PUT
}

// A flower drawn from five round petals and a centre, `scale` 0 bud, 1 half open, 2 open; centred at (cx, cy).
static void flower(uint32_t *tmp, int S, int cx, int cy, int scale, uint32_t P, uint32_t ps, uint32_t green)
{
#define T(x, y, c) do { if ((x) >= 0 && (y) >= 0 && (x) < S && (y) < S) tmp[(y) * S + (x)] = ARGB(c); } while (0)
    if (scale == 0) {                    // a bud: a green sheath, a tip of colour
        T(cx, cy - 1, P); T(cx, cy, P); T(cx - 1, cy, green); T(cx + 1, cy, green); T(cx, cy + 1, green); T(cx, cy + 2, green);
        T(cx - 1, cy - 1, green); T(cx + 1, cy - 1, green);
        return;
    }
    // petals: five round blobs at 72 degrees (radius 2.4 units out, or 1.6 half open), lit from the top-left
    static const int dir[5][2] = { { 0, -10 }, { 10, -3 }, { 6, 8 }, { -6, 8 }, { -10, -3 } };   // x, y * 10
    const int rr = scale == 2 ? 24 : 16, pr = scale == 2 ? 16 : 11;                     // tenths of a unit
    for (int y = -5; y <= 5; y++)
        for (int x = -5; x <= 5; x++) {
            const int X = x * 10, Y = y * 10;
            for (int k = 0; k < 5; k++) {
                const int dx = X - dir[k][0] * rr / 10, dy = Y - dir[k][1] * rr / 10;
                if (dx * dx + dy * dy <= pr * pr) {
                    const bool shade = dx + dy > pr / 2;
                    T(cx + x, cy + y, shade ? ps : P);
                }
            }
        }
    // the centre: gold, a darker eye, a glint
    const int cr = scale == 2 ? 13 : 9;
    for (int y = -2; y <= 2; y++)
        for (int x = -2; x <= 2; x++)
            if (x * x * 100 + y * y * 100 <= cr * cr) T(cx + x, cy + y, (x + y > 0) ? 0xd8901c : 0xffd23f);
    if (scale == 2) T(cx - 1, cy - 1, 0xfff6c8);
#undef T
}

static void garden_corner(const look_pal_t *p, int i, int state, int grow, int phase, uint32_t *px)
{
    const int u = p->u, S = 17, o = 6;            // the box's corner at (o, o) of the sprite (top-left)
    memset(px, 0, (size_t)S * u * S * u * 4);
    uint32_t tmp[17 * 17];
    memset(tmp, 0, sizeof(tmp));
#define T(x, y, c) do { if ((x) >= 0 && (y) >= 0 && (x) < S && (y) < S) tmp[(y) * S + (x)] = ARGB(state_tint((c), state, p->alert)); } while (0)
    // The joint: a darker square block over the planks' corner, bevelled, a wooden peg in it.
    for (int y = 0; y < G_THICK; y++)
        for (int x = 0; x < G_THICK; x++) {
            uint32_t c = (x == 0 || y == 0 || x == 4 || y == 4) ? G_OUT : (x == 1 || y == 1) ? 0x8a5a2c : 0x6b4423;
            T(o + x, o + y, c);
        }
    T(o + 2, o + 2, 0x9c6e3e); T(o + 3, o + 3, 0x3a2412); T(o + 3, o + 2, 0x4a2e16); T(o + 2, o + 3, 0x4a2e16);
    // Two leaves from the corner, along each edge outwards: blade, a midrib, the stalk.
    static const char *const leaf[3] = { ".LLLL..", "LlllllL", ".rrrr.." };
    for (int y = 0; y < 3; y++)
        for (int x = 0; x < 7; x++) {
            const char ch = leaf[y][x];
            if (ch == '.') continue;
            const uint32_t c = ch == 'L' ? G_LEAF2 : ch == 'l' ? G_LEAF : G_LEAF3;
            T(o + 5 + x, o - 3 + y, c);            // along the top edge
            T(o - 3 + y, o + 5 + x, c);            // along the side
        }
#undef T
    // The flower, by corner: pink, white, lilac, apricot; red roses on an alert. Open on the window in focus once
    // the vine has reached its corner (half open on the way), a bud otherwise; it sways a unit with the phase.
    static const uint32_t petal[4][2] = { { 0xf27fc3, 0xc4508f }, { 0xfafafa, 0xc9ccd8 }, { 0xbb92ee, 0x8a5cc0 }, { 0xffaa52, 0xd8762a } };
    const int k = i == 3 ? 2 : i == 2 ? 3 : i;    // clockwise order: TL, TR, BR, BL
    const bool reached = grow >= 64 * k + 32;
    uint32_t P = petal[i][0], ps = petal[i][1];
    if (state == 2) { P = 0xe4362c; ps = 0xa21f1f; }
    const int sway = (state == 1 && grow >= 256) ? ((phase / 4) % 4 == 1 ? 1 : (phase / 4) % 4 == 3 ? -1 : 0) : 0;
    const int scale = (state == 0 || !reached) ? 0 : (grow < 64 * k + 64 && grow < 256) ? 1 : 2;
    flower(tmp, S, 4 + sway, 4, scale, state == 0 ? state_tint(P, 0, p->alert) : P, state == 0 ? state_tint(ps, 0, p->alert) : ps,
           state_tint(G_LEAF2, state, p->alert));
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            const uint32_t c = tmp[y * S + x];
            if (c) put(px, S, S, u, (i & 1) ? S - 1 - x : x, (i & 2) ? S - 1 - y : y, c);
        }
}

// The sign: a plank as wide as the title and 7 units of margin, 11 tall, its top 4 units above the box; outline,
// a lit top edge, grain, and a nail at each end.
static void garden_title(const look_pal_t *p, int w, int h, int tw, int state, uint32_t *px)
{
    const int u = p->u, W = w / u, H = h / u;
    memset(px, 0, (size_t)w * h * 4);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const bool edge = y == 0 || y == H - 1 || x == 0 || x == W - 1;
            const bool corner = (x == 0 || x == W - 1) && (y == 0 || y == H - 1);
            if (corner) continue;                                  // rounded off
            uint32_t c;
            const uint32_t hsh = hash((uint32_t)(x / 5) * 31u + (uint32_t)y * 7919u), h2 = hash((uint32_t)x * 131u + (uint32_t)y);
            if (edge) c = G_OUT;
            else if (y == 1) c = G_LIGHT;
            else if (y == H - 2) c = G_SHADE;
            else c = (hsh % 5 == 0 || h2 % 23 == 0) ? G_GRAIN : (y == 2 || y == H - 3) ? G_WOOD2 : G_WOOD;
            put(px, W, H, u, x, y, ARGB(state_tint(c, state, p->alert)));
        }
    // the nails: a lit head, its shadow
    const int ny = H / 2 - 1;
    for (int s = 0; s < 2; s++) {
        const int nx = s ? W - 4 : 2;
        put(px, W, H, u, nx, ny, ARGB(state_tint(0xe4e8ee, state, p->alert)));
        put(px, W, H, u, nx + 1, ny, ARGB(state_tint(0xa9adb5, state, p->alert)));
        put(px, W, H, u, nx, ny + 1, ARGB(state_tint(0xa9adb5, state, p->alert)));
        put(px, W, H, u, nx + 1, ny + 1, ARGB(state_tint(0x5d5f66, state, p->alert)));
    }
    (void)tw;
}

static void garden_desk(const look_pal_t *p, uint16_t *px, int *w, int *h)
{
    // As large a tile as the buffer takes (96 x 64 units on a 320 x 240 screen, 48 x 32 on a 480 x 480 one), so the
    // lawn repeats as little as it can; blades and flowers scattered by a hash, as many as its area calls for.
    const int u = p->u, W = LOOK_DESK_MAX / u, H = W * 2 / 3, area = W * H;
    const uint32_t blade[3] = { p->desk_b, mix(p->desk_b, 0x000000, 50), mix(p->desk_a, 0xa8d870, 70) };
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const uint32_t hs = hash((uint32_t)(x * 7 + y * 131));
            put16(px, W, u, x, y, hs % 7 == 0 ? mix(p->desk_a, p->desk_b, 70) : p->desk_a);     // a mottled lawn
        }
    // blades: strokes 2-3 units tall, leaning, in three greens
    for (int k = 0; k < area / 33; k++) {
        const uint32_t hs = hash((uint32_t)k * 2246822519u + 3u);
        const int x = (int)(hs % W), y = (int)((hs >> 8) % H), tall = 2 + (int)((hs >> 16) % 2), lean = (int)((hs >> 20) % 3) - 1;
        const uint32_t c = blade[(hs >> 24) % 3];
        for (int t = 0; t < tall; t++) put16(px, W, u, (x + (t == tall - 1 ? lean : 0) + W) % W, (y + H - t) % H, c);
    }
    // flowers: white daisies with gold eyes, now and then a pink clover or a buttercup; a pebble or two
    for (int k = 0; k < area / 700 + 1; k++) {
        const uint32_t hs = hash((uint32_t)k * 3266489917u + 11u);
        const int x = 2 + (int)(hs % (W - 4)), y = 2 + (int)((hs >> 10) % (H - 4)), kind = (int)((hs >> 20) % 5);
        const uint32_t pc = kind == 3 ? 0xf2a0d0 : kind == 4 ? 0xffd23f : 0xfafafa, pd = kind == 3 ? 0xd070a8 : kind == 4 ? 0xd8a020 : 0xd6d8e0;
        put16(px, W, u, x, y - 1, pc); put16(px, W, u, x - 1, y, pd); put16(px, W, u, x + 1, y, pc); put16(px, W, u, x, y + 1, pd);
        put16(px, W, u, x, y, kind == 4 ? 0xfff0a0 : 0xffd23f);
    }
    for (int k = 0; k < area / 2000 + 1; k++) {
        const uint32_t hs = hash((uint32_t)k * 668265263u + 5u);
        const int x = (int)(hs % (W - 1)), y = (int)((hs >> 12) % H);
        put16(px, W, u, x, y, 0x9a9a8a); put16(px, W, u, x + 1, y, 0x7c7c6e);
    }
    *w = W * u;
    *h = H * u;
}

static const char *const s_ladybird[7] = { "..kkk..", ".rkkkr.", "rrrkrrr", "rkrkrkr", "rrrkrrr", ".rrkrr.", "..rrr.." };

// ── Sci-fi ──

#define S_EDGE 0x0d1216
#define S_HI 0x9aa9b5
#define S_MID 0x56646f
#define S_LO 0x3a4550
#define S_SLOT 0x161d23
#define S_RIVET 0xd2dbe2
#define S_THICK 5               // edge, bevel, plate, the line of light, its glow

static void scifi_strip(const look_pal_t *p, int side, int len, int state, int grow, uint32_t *px)
{
    const int u = p->u, L = len / u, T = S_THICK;
    const bool horiz = side < 2, lit = side == 0 || side == 2;
    const int W = horiz ? L : T, H = horiz ? T : L;
    memset(px, 0, (size_t)W * u * H * u * 4);
    const int half = L * grow / 512;                    // the plates close from both ends towards the middle
    const uint32_t glow = state == 2 ? p->alert : state == 1 ? p->ink : p->dim;
    for (int a = 0; a < L; a++) {
        const bool shown = a < half || L - 1 - a < half || grow >= 256;
        if (!shown) continue;
        for (int r = 0; r < T; r++) {
            uint32_t c, alpha = 0xff;
            switch (r) {
            case 0: c = S_EDGE; break;
            case 1: c = lit ? S_HI : S_LO; break;          // the bevel, lit from the top-left
            case 2: c = S_MID; break;
            case 3: c = glow; break;                        // the line of light along the inside
            default: c = glow; alpha = 0x50; break;         // its glow on the panel
            }
            // panel lines: a vent of three slots every 32 units, a screw between, a seam every 64
            const int m = a % 32;
            if (r >= 1 && r <= 2 && m >= 12 && m <= 16 && (m & 1) == 0) c = S_SLOT;
            if (r == 2 && m == 4) c = S_RIVET;
            if (r == 2 && m == 5) c = S_SLOT;
            if (r >= 1 && r <= 2 && a % 64 == 63) c = S_EDGE;
            // where the plates meet as they close, a bright seam
            if (r < 3 && grow < 256 && (a == half - 1 || L - 1 - a == half - 1)) c = 0xe6f6ff;
            if (r < 3) c = state_tint(c, state == 1 ? 1 : state, p->alert);
            put(px, W, H, u, horiz ? a : r, horiz ? r : a, (alpha << 24) | c);
        }
    }
}

static void scifi_corner(const look_pal_t *p, int i, int state, int phase, uint32_t *px)
{
    const int u = p->u, S = 14, A = S_THICK;           // arms as thick as the plates
    memset(px, 0, (size_t)S * u * S * u * 4);
    const uint32_t glow = state == 2 ? p->alert : state == 1 ? p->ink : p->dim;
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            const int d = x + y;
            uint32_t c = 0;
            const bool arm = x < A || y < A;
            if (d < 4) c = p->paper;                              // the corner cut away (the desktop is as dark)
            else if (!arm) continue;
            else if (d == 4 || x == 0 || y == 0 || (x == A - 1 && y >= A - 1) || (y == A - 1 && x >= A - 1)) c = S_EDGE;
            else if (x == S - 1 || y == S - 1) c = S_EDGE;          // the arms' ends
            else if (d == 5 || x == 1 || y == 1) c = 0xb9c6d0;     // the bevel's light
            else if (x == A - 2 || y == A - 2) c = 0x2f3942;       // its shade
            else c = 0x75838f;
            if ((x == 11 && y == 3) || (x == 3 && y == 11)) c = S_RIVET;           // screws
            if ((x == 12 && y == 3) || (x == 3 && y == 12)) c = S_SLOT;
            if (c != p->paper) c = state_tint(c, state == 1 ? 1 : state, p->alert);
            put(px, S, S, u, (i & 1) ? S - 1 - x : x, (i & 2) ? S - 1 - y : y, ARGB(c));
        }
    // the status light, a halo round it; blinking slowly on the window in focus
    const bool on = state != 1 || (phase / 6) % 4 != 3;
    const uint32_t led = on ? glow : mix(glow, 0x000000, 170);
    static const int lp[][2] = { { 3, 3 }, { 4, 3 }, { 3, 4 }, { 4, 4 } };
    for (int k = 0; k < 4; k++) {
        const int x = lp[k][0], y = lp[k][1];
        put(px, S, S, u, (i & 1) ? S - 1 - x : x, (i & 2) ? S - 1 - y : y, ARGB(k == 0 ? mix(led, 0xffffff, 120) : led));
    }
}

// The tab: from 6 units in, rising 4 above the box, 11 tall; its right edge leans out; three lights after it.
#define S_TAB_IN 13              // clear of the corner bracket (14 units, 2 of them the border)
#define S_LIGHTS 14
static void scifi_title(const look_pal_t *p, int w, int h, int tw, int state, int phase, uint32_t *px)
{
    const int u = p->u, W = w / u, H = h / u, TW = W - S_LIGHTS;
    memset(px, 0, (size_t)w * h * 4);
    const uint32_t glow = state == 2 ? p->alert : state == 1 ? p->ink : p->dim;
    for (int y = 0; y < H; y++) {
        const int right = TW - 1 - y / 2;                 // the leaning edge
        for (int x = 0; x <= right; x++) {
            uint32_t c;
            if (y == 0 || x == 0 || x == right) c = glow;                         // outlined in light
            else if (y == 1 || x == 1 || x == right - 1) c = S_EDGE;              // a dark line inside it
            else if (y == 2) c = 0x22303b;
            else c = 0x111a21;
            put(px, W, H, u, x, y, ARGB(c));
        }
    }
    // the lights: three framed squares on the plate's row, one dark in turn on the window in focus
    static const uint32_t lights[3] = { 0x46e07a, 0xffb43a, 0x3fd8ff };
    for (int k = 0; k < 3; k++) {
        const int x = TW + 1 + k * 4, y = H - 8;
        const bool lit = state != 1 || ((phase / 3) % 3) != k;
        const uint32_t c = state == 2 ? (k == 0 ? p->alert : mix(p->alert, 0x000000, 140)) : lit ? lights[k] : mix(lights[k], 0x000000, 170);
        for (int dy = 0; dy < 4; dy++)
            for (int dx = 0; dx < 4; dx++) {
                const bool frame = dx == 0 || dy == 0 || dx == 3 || dy == 3;
                put(px, W, H, u, x + dx, y + dy, ARGB(frame ? S_EDGE : (dx == 1 && dy == 1 ? mix(c, 0xffffff, 110) : c)));
            }
    }
    (void)tw;
}

static void scifi_desk(const look_pal_t *p, uint16_t *px, int *w, int *h)
{
    const int u = p->u, W = 32, H = 32;
    const uint32_t cross = mix(p->desk_b, 0x3fd8ff, 45);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint32_t c = p->desk_a;
            if (x % 16 == 0 || y % 16 == 0) c = p->desk_b;                         // the grid
            if ((x % 16 == 0 && (y % 16 <= 2 || y % 16 >= 14)) || (y % 16 == 0 && (x % 16 <= 2 || x % 16 >= 14))) c = cross;
            if (x % 4 == 2 && y % 4 == 2 && !(x % 16 == 0 || y % 16 == 0)) c = mix(p->desk_a, p->desk_b, 100);
            put16(px, W, u, x, y, c);
        }
    *w = W * u;
    *h = H * u;
}

static const char *const s_chevron[7] = { "Cg.....", "gCCg...", ".gCCCg.", "..gCCCC", ".gCCCg.", "gCCg...", "Cg....." };
static const char *const s_chevron_h[7] = { "g......", ".gg....", "..g.g..", "...g..g", "..g.g..", ".gg....", "g......" };

// ── The interface ──

void look_desk(int look, const look_pal_t *p, uint16_t *px, int *w, int *h)
{
    if (look == LOOK_GARDEN) garden_desk(p, px, w, h);
    else if (look == LOOK_SCIFI) scifi_desk(p, px, w, h);
    else *w = *h = 0;
}

int look_strip_thick(int look) { return look == LOOK_GARDEN ? G_THICK : look == LOOK_SCIFI ? S_THICK : 0; }
int look_strip_out(int look) { return look == LOOK_GARDEN ? G_OUTSIDE : 0; }

void look_strip(int look, const look_pal_t *p, int side, int len, int w, int h, int state, int grow, int phase, uint32_t *px)
{
    if (look == LOOK_GARDEN) garden_strip(p, side, len, w, h, state, grow, px);
    else if (look == LOOK_SCIFI) scifi_strip(p, side, len, state, grow, px);
    (void)phase;
}

int look_corner_size(int look) { return look == LOOK_GARDEN ? 17 : look == LOOK_SCIFI ? 14 : 0; }
int look_corner_out(int look) { return look == LOOK_GARDEN ? 6 : 0; }

void look_corner(int look, const look_pal_t *p, int i, int state, int grow, int phase, uint32_t *px)
{
    if (look == LOOK_GARDEN) garden_corner(p, i, state, grow, phase, px);
    else if (look == LOOK_SCIFI) scifi_corner(p, i, state, phase, px);
}

void look_title_box(int look, int u, int cw, int tw, int th, int *w, int *h, int *x, int *y, int *tx, int *ty)
{
    // Content coordinates start inside the box's 2-unit border: its top edge is at -2 units.
    if (look == LOOK_GARDEN) {
        *w = tw + 14 * u;
        *h = 11 * u;
        *x = (cw - *w) / 2;
        *y = -2 * u - 4 * u;
        *tx = *x + 7 * u;
        *ty = *y + 2 * u - 1;
    } else {                                            // sci-fi
        *w = tw + 14 * u + S_LIGHTS * u;
        *h = 11 * u;
        *x = S_TAB_IN * u;
        *y = -2 * u - 4 * u;
        *tx = *x + 5 * u;
        *ty = *y + 2 * u - 1;
    }
    (void)th;
}

void look_title(int look, const look_pal_t *p, int w, int h, int tw, int state, int phase, uint32_t *px)
{
    if (look == LOOK_GARDEN) garden_title(p, w, h, tw, state, px);
    else if (look == LOOK_SCIFI) scifi_title(p, w, h, tw, state, phase, px);
}

uint32_t look_title_ink(int look, const look_pal_t *p, int state)
{
    if (look == LOOK_GARDEN) return state == 0 ? 0xd9c9a3 : 0xfff1cc;
    if (look == LOOK_SCIFI) return state == 2 ? 0xffffff : state == 1 ? p->ink : p->dim;
    return p->ink;
}

int look_body_top(int look) { (void)look; return 7; }

int look_cursor(int look, const look_pal_t *p, int hollow, uint32_t *px, int *w, int *h)
{
    if (look != LOOK_GARDEN && look != LOOK_SCIFI) return 0;
    const int u = p->u, W = 7, H = 7;
    memset(px, 0, (size_t)W * u * H * u * 4);
    if (look == LOOK_GARDEN) {
        // a ladybird; out of focus, a faded one
        const uint32_t cols[2] = { hollow ? 0x8a7a6a : 0x1c1410, hollow ? 0xc89a8a : 0xd8262c };
        art(px, W, H, u, 0, 0, s_ladybird, 7, false, "kr", cols);
    } else {
        const uint32_t cols[2] = { p->ink, p->dim };
        if (hollow) { const uint32_t d[1] = { p->dim }; art(px, W, H, u, 0, 0, s_chevron_h, 7, false, "g", d); }
        else art(px, W, H, u, 0, 0, s_chevron, 7, false, "Cg", cols);
    }
    *w = W * u;
    *h = H * u;
    return 1;
}

uint32_t look_focus_text(int look, const look_pal_t *p)
{
    if (look == LOOK_GARDEN) return 0x2e6b1f;
    return p->ink;
}

// ── Motes: the one small thing that moves round the window in focus ──

static const char *const s_fly[2][5] = {
    { "oo.k.oo", "oyykyyo", "oywkwyo", ".oykyo.", "..o.o.." },
    { ".o...o.", ".yo.oy.", "..yky..", "...k...", "......." },
};
static const char *const s_spark[5] = { "..a..", ".aca.", "acWca", ".aca.", "..a.." };

int look_mote(int look, const look_pal_t *p, int frame, uint32_t *px, int *w, int *h)
{
    const int u = p->u;
    if (look == LOOK_GARDEN) {
        memset(px, 0, (size_t)7 * u * 5 * u * 4);
        static const uint32_t cols[4] = { 0x6b4a10, 0xffd84a, 0xfff6c0, 0x3b2a1a };
        art(px, 7, 5, u, 0, 0, s_fly[frame & 1], 5, false, "oywk", cols);
        *w = 7 * u;
        *h = 5 * u;
        return 1;
    }
    if (look == LOOK_SCIFI) {
        memset(px, 0, (size_t)5 * u * 5 * u * 4);
        const uint32_t c = frame ? mix(p->ink, 0xffffff, 60) : p->ink;
        for (int y = 0; y < 5; y++)
            for (int x = 0; x < 5; x++) {
                const char ch = s_spark[y][x];
                if (ch == '.') continue;
                const uint32_t argb = ch == 'a' ? (0x70000000u | p->ink) : ch == 'c' ? ARGB(c) : ARGB(0xffffff);
                put(px, 5, 5, u, x, y, argb);
            }
        *w = *h = 5 * u;
        return 1;
    }
    return 0;
}

// Where the mote is at t (ms) on a window w x h pixels: the sprite's top-left from the box's outer top-left, and
// which picture; returns 0 while it is out of sight. Garden: a butterfly flits clockwise from flower to flower,
// arcing outside the frame, and rests on each a while, opening and closing its wings. Sci-fi: a spark runs along
// the line of light and slips under the corner brackets.
int look_mote_at(int look, int u, int w, int h, uint32_t t, int *x, int *y, int *frame)
{
    if (look == LOOK_GARDEN) {
        // the flowers' centres: 2 units outside each corner
        const int x0 = -2 * u, y0 = -2 * u, x1 = w + 2 * u - 1, y1 = h + 2 * u - 1;
        const int len[4] = { x1 - x0, y1 - y0, x1 - x0, y1 - y0 };
        const int px_[5] = { x0, x1, x1, x0, x0 }, py_[5] = { y0, y0, y1, y1, y0 };
        const uint32_t rest = 1600, speed = 30;             // ms on a flower; units a second
        uint32_t legs[4], total = 0;
        for (int k = 0; k < 4; k++) total += (legs[k] = rest + (uint32_t)len[k] * 1000 / (speed * (uint32_t)u));
        uint32_t m = t % total;
        int k = 0;
        while (m >= legs[k]) m -= legs[k++];
        int cx, cy;
        if (m < rest) {                                     // resting on corner k's flower
            cx = px_[k];
            cy = py_[k];
            *frame = (m / 500) & 1;
        } else {
            const int f = (int)((m - rest) * 256 / (legs[k] - rest));
            cx = px_[k] + (px_[k + 1] - px_[k]) * f / 256;
            cy = py_[k] + (py_[k + 1] - py_[k]) * f / 256;
            // an arc outwards with a flutter in it: up to 5 units out, a 1-unit bob every 300 ms
            static const int8_t arc[17] = { 0, 18, 34, 48, 60, 70, 77, 82, 84, 82, 77, 70, 60, 48, 34, 18, 0 };
            const int out = arc[f * 16 / 256] * 5 * u / 84 + (((m / 150) & 1) ? u : 0);
            if (k == 0) cy -= out;
            else if (k == 1) cx += out;
            else if (k == 2) cy += out;
            else cx -= out;
            *frame = (m / 90) & 1;
        }
        *x = cx - 3 * u;
        *y = cy - 3 * u;
        return 1;
    }
    if (look == LOOK_SCIFI) {
        // the line of light: 3 units in from the outer edge (the strip's row 3)
        const int x0 = 3 * u, y0 = 3 * u, x1 = w - 3 * u - 1, y1 = h - 3 * u - 1;
        const int P = 2 * (x1 - x0) + 2 * (y1 - y0);
        if (P <= 0) return 0;
        int d = (int)(t * 70u * (uint32_t)u / 1000u) % P, cx, cy;
        if (d < x1 - x0) { cx = x0 + d; cy = y0; }
        else if ((d -= x1 - x0) < y1 - y0) { cx = x1; cy = y0 + d; }
        else if ((d -= y1 - y0) < x1 - x0) { cx = x1 - d; cy = y1; }
        else { d -= x1 - x0; cx = x0; cy = y1 - d; }
        // under the brackets (14 units from each corner) it is out of sight
        const int B = 14 * u;
        if ((cx < B || cx >= w - B) && (cy < B || cy >= h - B)) return 0;
        *frame = (t / 90) & 1;
        *x = cx - 2 * u;
        *y = cy - 2 * u;
        return 1;
    }
    return 0;
}
