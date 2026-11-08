// Duelo's fighters (see fighter.h). Coordinates are 96 x 128 design units (the body's
// middle at x 44, the ground at y 125); S scales them to the sprite's pixels.
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "fighter.h"

#define S 0.5f
#define INK 0x1a1222u

static uint32_t *s_px;
static int s_bx0, s_by0, s_bx1, s_by1;   // what has been drawn (the outline only looks there)

static inline void grow(int x0, int y0, int x1, int y1)
{
    if (x0 < s_bx0) s_bx0 = x0;
    if (y0 < s_by0) s_by0 = y0;
    if (x1 > s_bx1) s_bx1 = x1;
    if (y1 > s_by1) s_by1 = y1;
}

static inline int RD(float x) { return (int)floorf(x + 0.5f); }
// Only what the FH1 gives apps (fh1_sdk exports floorf, not ceilf, fminf or fmaxf).
static inline float CEIL(float x) { return -floorf(-x); }
static inline float MIN(float a, float b) { return a < b ? a : b; }
static inline float MAX(float a, float b) { return a > b ? a : b; }
static inline float TX(float x) { return FTR_CX + (x - 44) * S; }
static inline float TY(float y) { return FTR_FEET + (y - 125) * S; }

static uint32_t mix(uint32_t a, uint32_t b, float k)
{
    int out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        const int x = a >> sh & 255, y = b >> sh & 255;
        int v = RD(x + (y - x) * k);
        v = v < 0 ? 0 : v > 255 ? 255 : v;
        out |= v << sh;
    }
    return (uint32_t)out;
}

static inline void pset(int x, int y, uint32_t col)
{
    if (x >= 0 && y >= 0 && x < FTR_W && y < FTR_H) {
        s_px[y * FTR_W + x] = FTR_SET | col;
        grow(x, y, x, y);
    }
}


// ── Shapes, in sprite pixels, tested at the pixel's centre ──

enum { SH_ELLIPSE, SH_POLY, SH_CAPSULE, SH_RECT, SH_UNION, SH_CLIPY };

// Everything a test needs is worked out when the shape is made: the ESP32-S3 divides floats in software, so a
// test only multiplies.
typedef struct shape {
    int t, n;
    float a, b, c, d, e, f;
    float p[14][2], k_[14];          // a polygon's corners, and each edge's dx/dy
    const struct shape *k[5];
    int x0, y0, x1, y1;
} shape_t;

static shape_t ellipse(float cx, float cy, float rx, float ry)
{
    const float rxp = rx * S, ryp = ry * S;
    shape_t s = { .t = SH_ELLIPSE, .a = TX(cx), .b = TY(cy), .c = 1 / rxp, .d = 1 / ryp };
    s.x0 = (int)floorf(s.a - rxp - 1), s.y0 = (int)floorf(s.b - ryp - 1), s.x1 = (int)CEIL(s.a + rxp + 1), s.y1 = (int)CEIL(s.b + ryp + 1);
    return s;
}

static shape_t poly(int n, const float (*p)[2])
{
    shape_t s = { .t = SH_POLY, .n = n };
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int i = 0; i < n; i++) {
        s.p[i][0] = TX(p[i][0]);
        s.p[i][1] = TY(p[i][1]);
        x0 = MIN(x0, s.p[i][0]), x1 = MAX(x1, s.p[i][0]), y0 = MIN(y0, s.p[i][1]), y1 = MAX(y1, s.p[i][1]);
    }
    for (int i = 0, j = n - 1; i < n; j = i++) {
        const float dy = s.p[j][1] - s.p[i][1];
        s.k_[i] = dy != 0 ? (s.p[j][0] - s.p[i][0]) / dy : 0;
    }
    s.x0 = (int)floorf(x0) - 1, s.y0 = (int)floorf(y0) - 1, s.x1 = (int)CEIL(x1) + 1, s.y1 = (int)CEIL(y1) + 1;
    return s;
}

static shape_t capsule(const float *a, const float *b, float r)
{
    shape_t s = { .t = SH_CAPSULE, .a = TX(a[0]), .b = TY(a[1]), .c = TX(b[0]), .d = TY(b[1]), .e = r * S * r * S };
    const float dx = s.c - s.a, dy = s.d - s.b, L = dx * dx + dy * dy, rp = r * S;
    s.f = L > 0 ? 1 / L : 0;
    s.x0 = (int)floorf(MIN(s.a, s.c) - rp - 1), s.y0 = (int)floorf(MIN(s.b, s.d) - rp - 1);
    s.x1 = (int)CEIL(MAX(s.a, s.c) + rp + 1), s.y1 = (int)CEIL(MAX(s.b, s.d) + rp + 1);
    return s;
}

static shape_t rect(float x, float y, float w, float h)
{
    shape_t s = { .t = SH_RECT, .a = TX(x), .b = TY(y), .c = TX(x + w), .d = TY(y + h) };
    s.x0 = (int)floorf(s.a) - 1, s.y0 = (int)floorf(s.b) - 1, s.x1 = (int)CEIL(s.c) + 1, s.y1 = (int)CEIL(s.d) + 1;
    return s;
}

static shape_t uni(int n, const shape_t *const *k)
{
    shape_t s = { .t = SH_UNION, .n = n, .x0 = 1 << 20, .y0 = 1 << 20, .x1 = -(1 << 20), .y1 = -(1 << 20) };
    for (int i = 0; i < n; i++) {
        s.k[i] = k[i];
        if (k[i]->x0 < s.x0) s.x0 = k[i]->x0;
        if (k[i]->y0 < s.y0) s.y0 = k[i]->y0;
        if (k[i]->x1 > s.x1) s.x1 = k[i]->x1;
        if (k[i]->y1 > s.y1) s.y1 = k[i]->y1;
    }
    return s;
}

static shape_t clip_y(const shape_t *a, float ymax)          // what of a lies above ymax (design units)
{
    shape_t s = { .t = SH_CLIPY, .a = TY(ymax), .x0 = a->x0, .y0 = a->y0, .x1 = a->x1, .y1 = a->y1 };
    s.k[0] = a;
    if (s.y1 > (int)CEIL(s.a)) s.y1 = (int)CEIL(s.a);
    return s;
}

static bool in(const shape_t *s, float px, float py)        // px, py: the pixel's centre
{
    switch (s->t) {
    case SH_ELLIPSE: {
        const float a = (px - s->a) * s->c, b = (py - s->b) * s->d;
        return a * a + b * b <= 1;
    }
    case SH_POLY: {
        bool inside = false;
        for (int i = 0, j = s->n - 1; i < s->n; j = i++) {
            const float yi = s->p[i][1], yj = s->p[j][1];
            if ((yi > py) != (yj > py) && px < s->k_[i] * (py - yi) + s->p[i][0]) inside = !inside;
        }
        return inside;
    }
    case SH_CAPSULE: {
        const float dx = s->c - s->a, dy = s->d - s->b;
        float t = ((px - s->a) * dx + (py - s->b) * dy) * s->f;
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        const float qx = s->a + dx * t - px, qy = s->b + dy * t - py;
        return qx * qx + qy * qy <= s->e;
    }
    case SH_RECT: return px >= s->a && px < s->c && py >= s->b && py < s->d;
    case SH_UNION:
        for (int i = 0; i < s->n; i++)
            if (in(s->k[i], px, py)) return true;
        return false;
    case SH_CLIPY: return py < s->a && in(s->k[0], px, py);
    }
    return false;
}

// One row of a shape into out[x - x0] for x0..x1 (sets the pixels inside, leaves the others): polygons and
// ellipses by their spans (crossings worked out once a row), capsules pixel by pixel.
static void row_in(const shape_t *s, int y, int x0, int x1, uint8_t *out)
{
    const float py = y + 0.5f;
    switch (s->t) {
    case SH_ELLIPSE: {
        const float v = (py - s->b) * s->d, q = 1 - v * v;
        if (q < 0) return;
        const float w = sqrtf(q) / s->c;
        int a = (int)CEIL(s->a - w - 0.5f), b = (int)floorf(s->a + w - 0.5f);
        if (a < x0) a = x0;
        if (b > x1) b = x1;
        for (int x = a; x <= b; x++) out[x - x0] = 1;
        return;
    }
    case SH_POLY: {
        float xs[14];
        int n = 0;
        for (int i = 0, j = s->n - 1; i < s->n; j = i++) {
            const float yi = s->p[i][1], yj = s->p[j][1];
            if ((yi > py) != (yj > py)) xs[n++] = s->k_[i] * (py - yi) + s->p[i][0];
        }
        for (int i = 1; i < n; i++)          // a few crossings: insertion sort
            for (int j = i; j > 0 && xs[j - 1] > xs[j]; j--) {
                const float t = xs[j];
                xs[j] = xs[j - 1];
                xs[j - 1] = t;
            }
        for (int i = 0; i + 1 < n; i += 2) {     // inside: px in [xs[i], xs[i + 1])
            int a = (int)CEIL(xs[i] - 0.5f), b = (int)CEIL(xs[i + 1] - 0.5f) - 1;
            if (a < x0) a = x0;
            if (b > x1) b = x1;
            for (int x = a; x <= b; x++) out[x - x0] = 1;
        }
        return;
    }
    case SH_UNION:
        for (int i = 0; i < s->n; i++) row_in(s->k[i], y, x0, x1, out);
        return;
    case SH_CLIPY:
        if (py < s->a) row_in(s->k[0], y, x0, x1, out);
        return;
    default:
        for (int x = x0; x <= x1; x++)
            if (in(s, x + 0.5f, py)) out[x - x0] = 1;
        return;
    }
}

// Each pixel is tested once, into a mask the size of the sprite with a margin (the fill looks 2 pixels around).
#define MG 3
#define MW (FTR_W + 2 * MG)
#define MH (FTR_H + 2 * MG)
static uint8_t *s_mask;             // allocated on the first drawing, freed by ftr_free

static inline bool M(int x, int y) { return s_mask[(y + MG) * MW + x + MG]; }

// The fighter fill: a black edge, a deep shadow to the bottom right, a shadow band, the base, a light rim to the
// top left and a shine at its very corner.
static void fill_sh(const shape_t *s, uint32_t col, int sh)
{
    const int x0 = s->x0 < 0 ? 0 : s->x0, y0 = s->y0 < 0 ? 0 : s->y0;
    const int x1 = s->x1 >= FTR_W ? FTR_W - 1 : s->x1, y1 = s->y1 >= FTR_H ? FTR_H - 1 : s->y1;
    if (x1 < x0 || y1 < y0) return;
    grow(x0, y0, x1, y1);
    for (int y = y0 - MG; y <= y1 + MG; y++) {
        uint8_t *row = &s_mask[(y + MG) * MW + MG];
        memset(row + x0 - MG, 0, x1 - x0 + 1 + 2 * MG);
        if (y >= y0 && y <= y1) row_in(s, y, x0, x1, row + x0);
    }
    const uint32_t deep = mix(col, 0x140a24, 0.58f), shade = mix(col, 0x2a1446, 0.34f);
    const uint32_t light = mix(col, 0xfffbe8, 0.32f), shine = mix(col, 0xffffff, 0.7f);
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            if (!M(x, y)) continue;
            uint32_t c = col;
            if (!M(x - 1, y) || !M(x + 1, y) || !M(x, y - 1) || !M(x, y + 1)) c = FTR_EDGE;
            else if (!M(x + 1, y + 1)) c = deep;
            else if (sh && (!M(x + sh + 1, y + sh) || !M(x + sh, y + sh + 1))) c = shade;
            else if (!M(x - 2, y - 2) && !M(x - 1, y - 2)) c = shine;
            else if (!M(x - 1, y - 1) || !M(x - 2, y - 1)) c = light;
            s_px[y * FTR_W + x] = FTR_SET | c;
        }
    }
}

static void fill(const shape_t *s, uint32_t col) { fill_sh(s, col, 1); }

static void limb(const float *a, const float *b, float r, uint32_t col)
{
    const shape_t s = capsule(a, b, r);
    fill(&s, col);
}

static void line_px(float x0, float y0, float x1, float y1, uint32_t col)     // sprite pixels
{
    const int n = (int)MAX(MAX(fabsf(x1 - x0), fabsf(y1 - y0)), 1);
    const float sx = (x1 - x0) / n, sy = (y1 - y0) / n;
    for (int i = 0; i <= n; i++) pset(RD(x0 + sx * i), RD(y0 + sy * i), col);
}

static void outline(void)
{
    // The sprite's border rows and columns stay empty (the figure has room), so no bounds checks.
    const int x0 = s_bx0 > 1 ? s_bx0 - 1 : 1, y0 = s_by0 > 1 ? s_by0 - 1 : 1;
    const int x1 = s_bx1 < FTR_W - 2 ? s_bx1 + 1 : FTR_W - 2, y1 = s_by1 < FTR_H - 2 ? s_by1 + 1 : FTR_H - 2;
    if (x1 < x0 || y1 < y0) return;
    uint8_t *add = s_mask;           // the mask's room, reused
    memset(add + y0 * FTR_W, 0, (size_t)(y1 - y0 + 1) * FTR_W);
    for (int y = y0; y <= y1; y++) {
        const uint32_t *r = s_px + y * FTR_W;
        for (int x = x0; x <= x1; x++)
            if (!r[x] && (r[x - 1] | r[x + 1] | r[x - FTR_W] | r[x + FTR_W])) add[y * FTR_W + x] = 1;
    }
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            if (add[y * FTR_W + x]) s_px[y * FTR_W + x] = FTR_SET | FTR_EDGE;
}

// Where an elbow goes between a shoulder and a hand (the arm's two lengths), bent downwards.
static void elbow(const float *sh, const float *hand, float l1, float l2, float *out)
{
    const float dx = hand[0] - sh[0], dy = hand[1] - sh[1];
    float d = sqrtf(dx * dx + dy * dy);
    if (d < 0.01f) d = 0.01f;
    const float dd = MIN(d, l1 + l2 - 0.01f);
    const float a = (l1 * l1 - l2 * l2 + dd * dd) / (2 * dd), h = sqrtf(MAX(0, l1 * l1 - a * a));
    const float ux = dx / d, uy = dy / d;
    const float nx = ux >= 0 ? -uy : uy, ny = ux >= 0 ? ux : -ux;      // the perpendicular pointing down
    out[0] = sh[0] + a * ux + h * nx;
    out[1] = sh[1] + a * uy + h * ny;
}

// ── The head ──

typedef struct {
    float cx, cy, rx, ry;
} head_t;

static void H(const head_t *g, float u, float v, float *out)
{
    out[0] = g->cx + u * g->rx;
    out[1] = g->cy + v * g->ry;
}

static shape_t hpoly(const head_t *g, int n, const float (*uv)[2])
{
    float p[14][2];
    for (int i = 0; i < n; i++) H(g, uv[i][0], uv[i][1], p[i]);
    return poly(n, (const float (*)[2])p);
}

static void back_hair(const ftr_look_t *l, const head_t *g)
{
    const shape_t cap = ellipse(g->cx, g->cy - g->ry * 0.06f, g->rx * 1.13f, g->ry * 1.1f);
    if (l->hair_style == FTR_HAIR_PONYTAIL) {       // the tail hangs at the back (left, facing right)
        float p[4][2];
        H(g, -0.55f, -0.95f, p[0]);
        H(g, -1.05f, -0.7f, p[1]);
        p[2][0] = g->cx - g->rx * 1.7f, p[2][1] = g->cy + g->ry * 2.2f;
        H(g, -0.75f, 0.4f, p[3]);
        const shape_t tail = poly(4, (const float (*)[2])p);
        const shape_t *k[] = { &cap, &tail };
        const shape_t u = uni(2, k);
        fill(&u, l->hair);
    } else {
        fill(&cap, l->hair);
    }
}

static void front_hair(const ftr_look_t *l, const head_t *g)
{
    const shape_t capl = ellipse(g->cx, g->cy - g->ry * 0.06f, g->rx * 1.13f, g->ry * 1.1f);
    const shape_t top = clip_y(&capl, g->cy - g->ry * 0.38f);
    if (l->hair_style == FTR_HAIR_SPIKY) {
        static const float bangs_uv[12][2] = { { -1.08f, -0.5f }, { -0.9f, -0.05f }, { -0.62f, -0.42f }, { -0.38f, 0.0f },
                                               { -0.12f, -0.38f }, { 0.15f, 0.02f }, { 0.4f, -0.4f }, { 0.66f, -0.02f },
                                               { 0.92f, -0.45f }, { 1.08f, -0.5f }, { 1.0f, -0.9f }, { -1.0f, -0.9f } };
        static const float sp[4][3][2] = { { { -0.9f, -0.7f }, { -1.35f, -1.05f }, { -0.55f, -0.95f } },
                                           { { -0.35f, -0.95f }, { -0.2f, -1.5f }, { 0.15f, -1.0f } },
                                           { { 0.3f, -0.95f }, { 0.85f, -1.35f }, { 0.75f, -0.75f } },
                                           { { 0.85f, -0.6f }, { 1.4f, -0.75f }, { 1.0f, -0.3f } } };
        const shape_t bangs = hpoly(g, 12, bangs_uv);
        const shape_t s0 = hpoly(g, 3, sp[0]), s1 = hpoly(g, 3, sp[1]), s2 = hpoly(g, 3, sp[2]), s3 = hpoly(g, 3, sp[3]);
        const shape_t *k[] = { &top, &bangs, &s0, &s1, &s2 };
        const shape_t u0 = uni(5, k);
        const shape_t *k2[] = { &u0, &s3 };
        const shape_t u = uni(2, k2);
        fill(&u, l->hair);
    } else {
        static const float bangs_uv[11][2] = { { -1.08f, -0.45f }, { -0.95f, 0.05f }, { -0.65f, -0.3f }, { -0.35f, -0.05f },
                                               { -0.1f, -0.35f }, { 0.2f, -0.05f }, { 0.5f, -0.32f }, { 0.8f, 0.0f },
                                               { 1.08f, -0.45f }, { 0.9f, -0.9f }, { -0.9f, -0.9f } };
        const shape_t bangs = hpoly(g, 11, bangs_uv);
        const shape_t *k[] = { &top, &bangs };
        const shape_t u = uni(2, k);
        fill(&u, l->hair);
    }
}

static void head(const ftr_look_t *l, const ftr_pose_t *p, const head_t *g)
{
    static const float chin[5][2] = { { -0.92f, 0.25f }, { 0.92f, 0.25f }, { 0.32f, 0.92f }, { 0.0f, 1.0f }, { -0.32f, 0.92f } };
    const shape_t skull = ellipse(g->cx, g->cy - g->ry * 0.05f, g->rx, g->ry * 0.92f), jaw = hpoly(g, 5, chin);
    const shape_t *k[] = { &skull, &jaw };
    const shape_t face = uni(2, k);
    fill(&face, l->skin);
    front_hair(l, g);
    // The headband across the forehead, its knot at the back.
    {
        const int by = RD(TY(g->cy - g->ry * 0.42f)), x0 = RD(TX(g->cx - g->rx * 1.08f)), x1 = RD(TX(g->cx + g->rx * 1.0f));
        for (int x = x0; x <= x1; x++) {
            pset(x, by, x == x0 + 1 ? mix(l->band, 0xffffff, 0.5f) : l->band);
            pset(x, by + 1, mix(l->band, 0x140a24, 0.35f));
        }
    }
    // Features at this size are a few pixels (the "mini" level), turned a pixel to the front.
    const float fx = TX(g->cx) + 1, ex = g->rx * 0.42f * S, ey = TY(g->cy + g->ry * 0.1f);
    for (int s = -1; s <= 1; s += 2) {
        const int x = RD(fx + s * ex - 0.5f), y = RD(ey - 0.5f);
        if (p->face == FTR_FACE_OPEN) {
            pset(x, y, INK);
            pset(x, y + 1, l->eyes);
            if (s > 0) pset(x + 1, y + 1, l->eyes);       // looking ahead
        } else {
            pset(x - 1, y + 1, INK);
            pset(x, y + 1, INK);
            if (s > 0) pset(x + 1, y + 1, INK);
        }
    }
    const int mx = RD(fx - 0.5f), my = RD(TY(g->cy + g->ry * 0.58f));
    if (p->face == FTR_FACE_SHOUT) {
        pset(mx, my, 0x8a2030);
        pset(mx + 1, my, 0x8a2030);
        pset(mx, my + 1, 0x8a2030);
        pset(mx + 1, my + 1, 0x5a1020);
    } else {
        pset(mx, my, mix(l->skin, INK, 0.6f));
        pset(mx + 1, my, mix(l->skin, INK, 0.6f));
    }
}

// ── The fighter ──

void ftr_draw(uint32_t *px, const ftr_look_t *l, const ftr_pose_t *p, int *grip_x, int *grip_y)
{
    if (!s_mask && !(s_mask = malloc(MW * MH))) return;
    s_px = px;
    memset(px, 0, FTR_W * FTR_H * sizeof(uint32_t));
    s_bx0 = FTR_W, s_by0 = FTR_H, s_bx1 = s_by1 = -1;
    const float cx = 44, br = p->breath * 1.6f, sh = 36 + br, hip = 71, waist = 66, gnd = 125, ww = 11, sw = 19, neck = 29;
    const uint32_t M = l->top, skin = l->skin;
    // Walking: the feet trade places, the lifted one off the ground.
    const float st = p->step, liftF = st > 0 ? st * 5 : 0, liftB = st < 0 ? -st * 5 : 0;
    float hipB[2] = { cx - 7, hip + 2 }, kneeB[2] = { cx - 18 - st * 4, hip + 26 - liftB }, footB[2] = { cx - 24 - st * 7, gnd - 5 - liftB };
    float hipF[2] = { cx + 7, hip + 2 }, kneeF[2] = { cx + 19 + st * 4, hip + 24 - liftF }, footF[2] = { cx + 22 + st * 7, gnd - 5 - liftF };
    float shB[2] = { cx - 14, sh + 4 }, elbB[2] = { cx - 13, sh + 21 }, fistB[2] = { cx + 6, sh + 6 - br };
    float shF[2] = { cx + 15, sh + 4 }, elbF[2], grip[2];
    // The sword arm: the hand sweeps an arc in front of the shoulder with the blade's angle.
    const float a = p->no_blade ? -1.1f : p->blade, b = -0.35f + 0.55f * a;
    grip[0] = shF[0] + 18 * cosf(b);
    grip[1] = shF[1] - 18 * sinf(b);
    elbow(shF, grip, 13, 12, elbF);
    const head_t g = { 46, 18 + br, 11, 12.5f };

    back_hair(l, &g);
    // The headband's ribbons fly back behind the head.
    {
        const float kx = TX(g.cx - g.rx * 0.95f), ky = TY(g.cy - g.ry * 0.3f);
        line_px(kx, ky, kx - 7, ky + 1 + p->wave * 2, l->band);
        line_px(kx, ky + 1, kx - 6, ky + 4 + p->wave * 1.5f, mix(l->band, 0x140a24, 0.3f));
    }
    // The back leg and the back upper arm, behind.
    limb(hipB, kneeB, 6.6f, l->bottom);
    limb(kneeB, footB, 5.4f, l->bottom);
    {
        const shape_t e = ellipse(footB[0] - 2, footB[1] + 2, 8, 4.5f), r = rect(footB[0] - 6, footB[1] - 7, 10, 7);
        const shape_t *k[] = { &e, &r };
        const shape_t u = uni(2, k);
        fill(&u, l->boots);
    }
    limb(shB, elbB, 5.8f, M);
    // The front leg.
    limb(hipF, kneeF, 6.6f, l->bottom);
    limb(kneeF, footF, 5.4f, l->bottom);
    {
        const shape_t e = ellipse(footF[0] + 3, footF[1] + 2, 9, 4.5f), r = rect(footF[0] - 4, footF[1] - 7, 10, 7);
        const shape_t *k[] = { &e, &r };
        const shape_t u = uni(2, k);
        fill(&u, l->boots);
    }
    // The hakama: wide from the waist, split between the knees, to mid-shin.
    {
        const float end = hip + 34;
        const float pts[7][2] = { { cx - ww, waist }, { cx + ww, waist }, { kneeF[0] + 9, end }, { cx + 3, end },
                                  { cx, hip + 16 }, { cx - 3, end }, { kneeB[0] - 9, end } };
        const shape_t hk = poly(7, pts);
        fill(&hk, l->bottom);
    }
    // The torso, three-quarters, wider at the front shoulder; the neck.
    {
        const shape_t nk = rect(cx - 3, neck + br, 9, sh - neck - br + 4);
        fill(&nk, skin);
        const float pts[8][2] = { { cx - sw, sh + 4 }, { cx - sw + 5, sh }, { cx + sw - 3, sh }, { cx + sw + 2, sh + 5 },
                                  { cx + sw - 1, sh + 15 }, { cx + ww + 1, waist + 2 }, { cx - ww, waist + 2 }, { cx - sw + 2, sh + 15 } };
        const shape_t t = poly(8, pts);
        fill(&t, M);
        // The gi: its crossed collar and the belt.
        const uint32_t collar = mix(M, 0xffffff, 0.55f);
        line_px(TX(cx - 6), TY(sh + 1), TX(cx + 4), TY(waist - 3), collar);
        line_px(TX(cx + 7), TY(sh + 1), TX(cx - 1), TY(waist - 6), collar);
        const shape_t belt = rect(cx - ww, waist - 2, 2 * ww + 2, 4);
        fill_sh(&belt, 0x20202a, 0);
    }
    // The back forearm guards the chin.
    limb(elbB, fistB, 5.2f, skin);
    {
        const shape_t f = ellipse(fistB[0], fistB[1], 5.8f, 5.4f);
        fill(&f, l->wraps);
    }
    // The katana: the handle behind the hand, the guard, the blade (its edge shining on top).
    if (!p->no_blade) {
        const float ca = cosf(a), sa = sinf(a), gx = TX(grip[0]), gy = TY(grip[1]);
        line_px(gx - 6 * ca, gy + 6 * sa, gx, gy, 0x402020);
        line_px(gx - 6 * ca, gy + 6 * sa + 1, gx, gy + 1, 0x402020);
        // Two pixels wide: the steel, and its edge on the side away from the body.
        const bool flat = fabsf(sa) < 0.7f;
        const uint32_t edge = mix(p->steel, 0xffffff, 0.6f);
        for (int i = 0; i <= 30; i++) {
            const int x = RD(gx + i * ca), y = RD(gy - i * sa);
            pset(x, y, p->steel);
            if (flat) pset(x, y - 1, edge);
            else pset(x + (ca >= 0 ? 1 : -1), y, edge);
        }
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) pset(RD(gx + 2 * ca) + dx, RD(gy - 2 * sa) + dy, dx || dy ? 0xd4a020 : 0xf0c840);
    }
    // The front arm over all, its hand on the handle.
    limb(shF, elbF, 4.8f, M);
    limb(elbF, grip, 4.2f, skin);
    {
        const shape_t f = ellipse(grip[0], grip[1], 5, 4.6f);
        fill(&f, l->wraps);
    }
    head(l, p, &g);
    outline();
    if (grip_x) *grip_x = RD(TX(grip[0]));
    if (grip_y) *grip_y = RD(TY(grip[1]));
}

void ftr_free(void)
{
    free(s_mask);
    s_mask = NULL;
}
