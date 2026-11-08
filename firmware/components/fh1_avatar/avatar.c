// The avatar renderer (fh1_avatar.h): a port of firmware/docs/avatar-showcase.html's renderer, style A.
// Layers of shapes, each filled with a palette colour in 3 tones and an edge: a coloured edge (sel-out), a lit
// rim to the top-left, a shadow band to the bottom-right. Shapes are masks in pixel coordinates tested at the
// pixel's centre; head parts are given in head units (u, v: the head is the circle u^2 + v^2 <= 1), so the same
// hair and face fit the full figure, the face and the mini face. Rounding follows JavaScript's Math.round (half
// up) so both draw the same pixels.
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "fh1_avatar.h"

// ── Colours ──

#define INK 0x1a1222u
static const uint32_t PAL_SKIN[] = { 0xffe2cc, 0xf6cfa8, 0xe6b088, 0xc98d62, 0x9a6440, 0x6e4429 };
static const uint32_t PAL_HAIR[] = { 0x2a2232, 0x5a3a26, 0xa8643a, 0xe8c46a, 0xf0e8d0, 0xc83a3a, 0xe870a6, 0x7a5ad8, 0x3a8ae0, 0x36b892 };
static const uint32_t PAL_EYES[] = { 0x3f78e0, 0x2f9a6a, 0x8a5a2e, 0x9a48d0, 0xcc3e3e, 0x3c3c48 };
static const uint32_t PAL_CLOTH[] = { 0xecedf2, 0x2a2b36, 0xd03a3a, 0xe88a2a, 0xf0d040, 0x4aae4a, 0x2a98d0, 0x3a48c8, 0x8a48d0, 0xe86aa8, 0x8a6a4a, 0x5a7a8a };
static const uint32_t PAL_ACC[] = { 0xf2d040, 0xd8dce6, 0xd03a3a, 0x3a8ae0, 0x4aae4a, 0xe86aa8, 0x2a2b36, 0x8a48d0 };
#define N(a) ((int)(sizeof(a) / sizeof(a[0])))

static const char *const HAIRS[] = { "Spiky", "Short", "Bob", "Long", "Twin tails", "Ponytail" };
static const char *const TOPS[] = { "T-shirt", "Hoodie", "Jacket", "Gi", "Sailor top" };
static const char *const BOTTOMS[] = { "Trousers", "Shorts", "Skirt", "Long skirt" };
static const char *const BASES[] = { "Male", "Female" };
static const char *const ACCS[] = { "Round glasses", "Square glasses", "Earrings", "Scarf", "Necklace", "Face marks" };
static const char *const FIELDS[] = { "Body", "Hair", "Hair colour", "Skin", "Eyes", "Top", "Top colour", "Bottom",
                                      "Bottom colour", "Accessory colour" };

static inline int R(float x) { return (int)floorf(x + 0.5f); }

static uint32_t mix(uint32_t a, uint32_t b, float k)
{
    uint32_t out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        const float x = (float)((a >> sh) & 0xff), y = (float)((b >> sh) & 0xff);
        int v = R(x + (y - x) * k);
        v = v < 0 ? 0 : v > 255 ? 255 : v;
        out |= (uint32_t)v << sh;
    }
    return out;
}

// ── Pixels: 0 empty, else 0xff000000 | colour ──

typedef struct {
    int w, h;
    uint32_t *c;
} pix_t;

#define OPAQUE 0xff000000u
static inline void pset(pix_t *p, int x, int y, uint32_t col)
{
    if (x >= 0 && y >= 0 && x < p->w && y < p->h) p->c[y * p->w + x] = OPAQUE | col;
}
static inline bool drawn(const pix_t *p, int x, int y)
{
    return x >= 0 && y >= 0 && x < p->w && y < p->h && (p->c[y * p->w + x] & OPAQUE) == OPAQUE;
}

// ── Shapes ──

enum { SH_ELLIPSE, SH_POLY, SH_RECT, SH_UNION, SH_CLIPY };
#define POLY_MAX 14
#define KIDS_MAX 6
typedef struct shape {
    int kind;
    int x0, y0, x1, y1;                   // the box to test (inclusive)
    float a, b, c, d;                     // ellipse cx cy rx ry; rect x y w h; clipY ymin ymax
    int n;
    float p[POLY_MAX][2];
    const struct shape *k[KIDS_MAX];
} shape_t;

static shape_t ellipse(float cx, float cy, float rx, float ry)
{
    shape_t s = { .kind = SH_ELLIPSE, .a = cx, .b = cy, .c = rx, .d = ry };
    s.x0 = (int)floorf(cx - rx - 1), s.y0 = (int)floorf(cy - ry - 1), s.x1 = (int)ceilf(cx + rx + 1), s.y1 = (int)ceilf(cy + ry + 1);
    return s;
}

static shape_t poly(int n, const float p[][2])
{
    shape_t s = { .kind = SH_POLY, .n = n > POLY_MAX ? POLY_MAX : n };
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int i = 0; i < s.n; i++) {
        s.p[i][0] = p[i][0], s.p[i][1] = p[i][1];
        x0 = fminf(x0, p[i][0]), x1 = fmaxf(x1, p[i][0]), y0 = fminf(y0, p[i][1]), y1 = fmaxf(y1, p[i][1]);
    }
    s.x0 = (int)floorf(x0) - 1, s.y0 = (int)floorf(y0) - 1, s.x1 = (int)ceilf(x1) + 1, s.y1 = (int)ceilf(y1) + 1;
    return s;
}

static shape_t rect(float x, float y, float w, float h)
{
    shape_t s = { .kind = SH_RECT, .a = x, .b = y, .c = w, .d = h };
    s.x0 = (int)floorf(x), s.y0 = (int)floorf(y), s.x1 = (int)ceilf(x + w), s.y1 = (int)ceilf(y + h);
    return s;
}

static shape_t uni(int n, ...)
{
    shape_t s = { .kind = SH_UNION, .n = n, .x0 = 1 << 20, .y0 = 1 << 20, .x1 = -(1 << 20), .y1 = -(1 << 20) };
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n && i < KIDS_MAX; i++) {
        const shape_t *k = va_arg(ap, const shape_t *);
        s.k[i] = k;
        if (k->x0 < s.x0) s.x0 = k->x0;
        if (k->y0 < s.y0) s.y0 = k->y0;
        if (k->x1 > s.x1) s.x1 = k->x1;
        if (k->y1 > s.y1) s.y1 = k->y1;
    }
    va_end(ap);
    return s;
}

static shape_t clip_y(const shape_t *a, float ymin, float ymax)
{
    shape_t s = { .kind = SH_CLIPY, .a = ymin, .b = ymax, .x0 = a->x0, .x1 = a->x1 };
    s.k[0] = a;
    s.y0 = a->y0 > ymin ? a->y0 : (int)ymin;
    s.y1 = a->y1 < ymax ? a->y1 : (int)ymax;
    return s;
}

static bool in(const shape_t *s, int x, int y)
{
    switch (s->kind) {
    case SH_ELLIPSE: {
        const float a = (x + 0.5f - s->a) / s->c, b = (y + 0.5f - s->b) / s->d;
        return a * a + b * b <= 1;
    }
    case SH_POLY: {
        const float px = x + 0.5f, py = y + 0.5f;
        bool inside = false;
        for (int i = 0, j = s->n - 1; i < s->n; j = i++) {
            const float xi = s->p[i][0], yi = s->p[i][1], xj = s->p[j][0], yj = s->p[j][1];
            if ((yi > py) != (yj > py) && px < (xj - xi) * (py - yi) / (yj - yi) + xi) inside = !inside;
        }
        return inside;
    }
    case SH_RECT: return x >= s->a && x < s->a + s->c && y >= s->b && y < s->b + s->d;
    case SH_UNION:
        for (int i = 0; i < s->n; i++)
            if (in(s->k[i], x, y)) return true;
        return false;
    case SH_CLIPY: return y >= s->a && y < s->b && in(s->k[0], x, y);
    }
    return false;
}

// A whole row of a shape at once (x0..x1 at y): the same tests as in(), but a polygon's edge crossings are
// worked out once per row (the same arithmetic, so the same pixels).
#define ROW_MAX 128
static void row_in(const shape_t *s, int y, int x0, int x1, uint8_t *out)
{
    const int n = x1 - x0 + 1;
    switch (s->kind) {
    case SH_POLY: {
        const float py = y + 0.5f;
        float xc[POLY_MAX];
        int k = 0;
        for (int i = 0, j = s->n - 1; i < s->n; j = i++) {
            const float xi = s->p[i][0], yi = s->p[i][1], xj = s->p[j][0], yj = s->p[j][1];
            if ((yi > py) != (yj > py)) xc[k++] = (xj - xi) * (py - yi) / (yj - yi) + xi;
        }
        for (int x = 0; x < n; x++) {
            const float px = x0 + x + 0.5f;
            bool inside = false;
            for (int c = 0; c < k; c++)
                if (px < xc[c]) inside = !inside;
            out[x] = inside;
        }
        return;
    }
    case SH_UNION: {
        uint8_t tmp[ROW_MAX];
        memset(out, 0, n);
        for (int i = 0; i < s->n; i++) {
            const shape_t *kid = s->k[i];
            if (y < kid->y0 || y > kid->y1) continue;                // not on this row: nothing to add
            row_in(kid, y, x0, x1, tmp);
            for (int x = 0; x < n; x++) out[x] |= tmp[x];
        }
        return;
    }
    case SH_CLIPY:
        if (y >= s->a && y < s->b) row_in(s->k[0], y, x0, x1, out);
        else memset(out, 0, n);
        return;
    default:
        for (int x = 0; x < n; x++) out[x] = in(s, x0 + x, y);
        return;
    }
}

// The shape tested once per pixel into a mask over its box (clipped to the picture plus a margin of 2, enough
// for the neighbour tests below): fill reads the mask instead of testing the shape ~9 times per pixel.
#define MASK_MAX ((FH1_AVATAR_FULL_W + 5) * (FH1_AVATAR_FULL_H + 5))
static uint8_t *s_mask;
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
static uint8_t *mask_buf(void) { return s_mask ? s_mask : (s_mask = heap_caps_malloc(MASK_MAX, MALLOC_CAP_SPIRAM)); }
#else
#include <stdlib.h>
static uint8_t *mask_buf(void) { return s_mask ? s_mask : (s_mask = malloc(MASK_MAX)); }
#endif

typedef struct {
    int x0, y0, x1, y1, w;
    const uint8_t *m;
} mask_t;

static inline bool m_in(const mask_t *m, int x, int y)
{
    return x >= m->x0 && x <= m->x1 && y >= m->y0 && y <= m->y1 && m->m[(y - m->y0) * m->w + (x - m->x0)];
}

static bool mask_make(const pix_t *pix, const shape_t *s, mask_t *m)
{
    uint8_t *buf = mask_buf();
    m->x0 = s->x0 > -2 ? s->x0 : -2, m->y0 = s->y0 > -2 ? s->y0 : -2;
    m->x1 = s->x1 < pix->w + 1 ? s->x1 : pix->w + 1, m->y1 = s->y1 < pix->h + 1 ? s->y1 : pix->h + 1;
    if (!buf || m->x1 < m->x0 || m->y1 < m->y0) return false;
    m->w = m->x1 - m->x0 + 1;
    if (m->w > ROW_MAX) return false;
    for (int y = m->y0; y <= m->y1; y++) row_in(s, y, m->x0, m->x1, buf + (y - m->y0) * m->w);
    m->m = buf;
    return true;
}

// Fill a shape: edge, shadow band (sh pixels, 0 none), lit rim.
static void fill(pix_t *pix, const shape_t *s, uint32_t col, int sh)
{
    mask_t m;
    if (!mask_make(pix, s, &m)) return;
    const uint32_t shade = mix(col, 0x3a2450, 0.32f), light = mix(col, 0xfff6e8, 0.38f), edge = mix(col, INK, 0.62f);
    const int y0 = m.y0 > 0 ? m.y0 : 0, y1 = m.y1 < pix->h - 1 ? m.y1 : pix->h - 1;
    const int x0 = m.x0 > 0 ? m.x0 : 0, x1 = m.x1 < pix->w - 1 ? m.x1 : pix->w - 1;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            if (!m_in(&m, x, y)) continue;
            uint32_t c = col;
            if (!m_in(&m, x - 1, y) || !m_in(&m, x + 1, y) || !m_in(&m, x, y - 1) || !m_in(&m, x, y + 1)) c = edge;
            else if (sh && (!m_in(&m, x + sh, y + sh) || !m_in(&m, x + sh, y))) c = shade;
            else if (!m_in(&m, x - 1, y - 1) || !m_in(&m, x - 1, y - 2)) c = light;
            pset(pix, x, y, c);
        }
    }
}


static void flat(pix_t *pix, const shape_t *s, uint32_t col)
{
    const int y0 = s->y0 > 0 ? s->y0 : 0, y1 = s->y1 < pix->h - 1 ? s->y1 : pix->h - 1;
    const int x0 = s->x0 > 0 ? s->x0 : 0, x1 = s->x1 < pix->w - 1 ? s->x1 : pix->w - 1;
    if (x1 < x0 || x1 - x0 + 1 > ROW_MAX) return;
    uint8_t row[ROW_MAX];
    for (int y = y0; y <= y1; y++) {
        row_in(s, y, x0, x1, row);
        for (int x = x0; x <= x1; x++)
            if (row[x - x0]) pset(pix, x, y, col);
    }
}

static void line(pix_t *pix, int x0, int y0, int x1, int y1, uint32_t col, int w)
{
    int n = abs(x1 - x0) > abs(y1 - y0) ? abs(x1 - x0) : abs(y1 - y0);
    if (n < 1) n = 1;
    for (int i = 0; i <= n; i++) {
        const int x = R(x0 + (float)(x1 - x0) * i / n), y = R(y0 + (float)(y1 - y0) * i / n);
        for (int a = 0; a < w; a++)
            for (int b = 0; b < w; b++) pset(pix, x + a, y + b, col);
    }
}

// The whole figure's outline: every empty pixel touching a drawn one (marked first, then inked).
#define MARK 0x01000000u
static void outline(pix_t *pix, uint32_t col)
{
    for (int y = 0; y < pix->h; y++)
        for (int x = 0; x < pix->w; x++)
            if (!drawn(pix, x, y) && (drawn(pix, x - 1, y) || drawn(pix, x + 1, y) || drawn(pix, x, y - 1) || drawn(pix, x, y + 1)))
                pix->c[y * pix->w + x] = MARK;
    for (int i = 0; i < pix->w * pix->h; i++)
        if (pix->c[i] == MARK) pix->c[i] = OPAQUE | col;
}

// Tiny glyphs for effects.
enum { G_Z, G_Q, G_EXCL, G_HEART, G_STAR, G_SPARK, G_ANGER };
static const char *const GLYPHS[][5] = {
    { "111", "001", "010", "100", "111" }, { "110", "001", "010", "000", "010" }, { "1", "1", "1", "0", "1" },
    { "01010", "11111", "11111", "01110", "00100" }, { "010", "111", "010", NULL, NULL },
    { "00100", "00100", "11011", "00100", "00100" }, { "1010", "1111", "0000", "1111", "1010" },
};
static void glyph(pix_t *pix, int g, int x, int y, uint32_t col, int k)
{
    for (int j = 0; j < 5 && GLYPHS[g][j]; j++)
        for (int i = 0; GLYPHS[g][j][i]; i++)
            if (GLYPHS[g][j][i] == '1')
                for (int a = 0; a < k; a++)
                    for (int c = 0; c < k; c++) pset(pix, x + i * k + a, y + j * k + c, col);
}

// ── Expressions ──

enum { E_OPEN, E_SMILE, E_SAD, E_CLOSED, E_ANGRY, E_WIDE, E_WINK, E_HEARTS, E_SLEEPY, E_COOL, E_SIDE };
enum { M_SMILE, M_GRIN, M_LAUGH, M_FROWN, M_WAVY, M_O, M_SMIRK, M_SMALL, M_FLAT };
enum { B_NEUTRAL, B_RAISED, B_SAD, B_ANGRY, B_ONE };
enum { FX_NONE, FX_SPARKLES, FX_JOY, FX_GLOOM, FX_TEARS, FX_ANGER, FX_EXCLAIM, FX_STAR, FX_HEARTS, FX_ZZZ, FX_GLINT, FX_QUESTION };
typedef struct {
    const char *name;
    uint8_t eyes, mouth, brows, fx;
    bool blush;
    uint16_t blink_at;                    // where in the 3.2 s blink cycle (the showcase: id length x 700)
} expr_t;
static const expr_t EXPRS[FH1_AV_EXPRS] = {
    { "Neutral", E_OPEN, M_SMILE, B_NEUTRAL, FX_NONE, false, 7 * 700 },
    { "Happy", E_SMILE, M_GRIN, B_RAISED, FX_SPARKLES, true, 5 * 700 },
    { "Laughing", E_SMILE, M_LAUGH, B_RAISED, FX_JOY, true, 5 * 700 },
    { "Sad", E_SAD, M_FROWN, B_SAD, FX_GLOOM, false, 3 * 700 },
    { "Crying", E_CLOSED, M_WAVY, B_SAD, FX_TEARS, false, 3 * 700 },
    { "Angry", E_ANGRY, M_FROWN, B_ANGRY, FX_ANGER, false, 5 * 700 },
    { "Surprised", E_WIDE, M_O, B_RAISED, FX_EXCLAIM, false, 9 * 700 },
    { "Winking", E_WINK, M_SMIRK, B_NEUTRAL, FX_STAR, true, 4 * 700 },
    { "In love", E_HEARTS, M_SMILE, B_RAISED, FX_HEARTS, true, 4 * 700 },
    { "Sleepy", E_SLEEPY, M_SMALL, B_NEUTRAL, FX_ZZZ, false, 6 * 700 },
    { "Cool", E_COOL, M_SMIRK, B_NEUTRAL, FX_GLINT, false, 4 * 700 },
    { "Thinking", E_SIDE, M_FLAT, B_ONE, FX_QUESTION, false, 5 * 700 },
};

// ── The head. g: centre, radii, level ──

enum { L_FULL, L_FACE, L_MINI };
typedef struct {
    float cx, cy, rx, ry;
    int level;
} head_t;

typedef struct {
    const fh1_avatar_t *a;
    bool blink;
} cfg_t;

#define HX(u) (g->cx + (u) * g->rx)
#define HY(v) (g->cy + (v) * g->ry)
static shape_t hpoly(const head_t *g, int n, const float uv[][2])
{
    float p[POLY_MAX][2];
    for (int i = 0; i < n && i < POLY_MAX; i++) p[i][0] = HX(uv[i][0]), p[i][1] = HY(uv[i][1]);
    return poly(n, p);
}
#define HPOLY(...) hpoly(g, (int)(sizeof((const float[][2]){ __VA_ARGS__ }) / sizeof(float[2])), (const float[][2]){ __VA_ARGS__ })
#define POLY(...) poly((int)(sizeof((const float[][2]){ __VA_ARGS__ }) / sizeof(float[2])), (const float[][2]){ __VA_ARGS__ })
#define ACC(i) ((a->acc >> (i)) & 1)

static void back_hair(pix_t *pix, const fh1_avatar_t *a, const head_t *g, float bottom)
{
    const uint32_t col = PAL_HAIR[a->f[FH1_AV_HAIR]];
    const shape_t cap = ellipse(g->cx, g->cy - g->ry * 0.06f, g->rx * 1.13f, g->ry * 1.1f);
    switch (a->f[FH1_AV_HAIR_STYLE]) {
    case 2: {                                                                         // bob
        const shape_t p = HPOLY({ -1.15f, -0.2f }, { 1.15f, -0.2f }, { 1.1f, 0.95f }, { -1.1f, 0.95f });
        const shape_t u = uni(2, &cap, &p);
        fill(pix, &u, col, 2);
        break;
    }
    case 3: {                                                                         // long
        const float yb = fminf(bottom, g->cy + g->ry * 3.4f);
        const shape_t p = POLY({ HX(-1.12f), HY(-0.2f) }, { HX(1.12f), HY(-0.2f) }, { g->cx + g->rx * 1.25f, yb }, { g->cx - g->rx * 1.25f, yb });
        const shape_t u = uni(2, &cap, &p);
        fill(pix, &u, col, 2);
        break;
    }
    case 4: {                                                                         // twin tails
        fill(pix, &cap, col, 2);
        for (int s = -1; s <= 1; s += 2) {
            const shape_t p = POLY({ HX(s * 0.85f), HY(-0.75f) }, { HX(s * 1.35f), HY(-0.6f) },
                                   { g->cx + s * g->rx * 1.75f, fminf(bottom, g->cy + g->ry * 2.6f) }, { HX(s * 1.05f), HY(0.3f) });
            fill(pix, &p, col, 2);
        }
        break;
    }
    case 5: {                                                                         // ponytail
        const shape_t p = POLY({ HX(0.55f), HY(-0.95f) }, { HX(1.05f), HY(-0.7f) }, { g->cx + g->rx * 1.25f, fminf(bottom, g->cy + g->ry * 2.5f) },
                               { HX(0.75f), HY(0.4f) });
        const shape_t u = uni(2, &cap, &p);
        fill(pix, &u, col, 2);
        break;
    }
    default: fill(pix, &cap, col, 2); break;
    }
}

static void front_hair(pix_t *pix, const fh1_avatar_t *a, const head_t *g)
{
    const uint32_t col = PAL_HAIR[a->f[FH1_AV_HAIR]];
    const shape_t cap = ellipse(g->cx, g->cy - g->ry * 0.06f, g->rx * 1.13f, g->ry * 1.1f);
    const shape_t top = clip_y(&cap, -999, g->cy - g->ry * 0.38f);
    shape_t bangs;
    switch (a->f[FH1_AV_HAIR_STYLE]) {
    case 0: {                                                                         // spiky
        bangs = HPOLY({ -1.08f, -0.5f }, { -0.9f, -0.05f }, { -0.62f, -0.42f }, { -0.38f, 0.0f }, { -0.12f, -0.38f }, { 0.15f, 0.02f },
                      { 0.4f, -0.4f }, { 0.66f, -0.02f }, { 0.92f, -0.45f }, { 1.08f, -0.5f }, { 1.0f, -0.9f }, { -1.0f, -0.9f });
        const shape_t s1 = HPOLY({ -0.9f, -0.7f }, { -1.35f, -1.05f }, { -0.55f, -0.95f });
        const shape_t s2 = HPOLY({ -0.35f, -0.95f }, { -0.2f, -1.5f }, { 0.15f, -1.0f });
        const shape_t s3 = HPOLY({ 0.3f, -0.95f }, { 0.85f, -1.35f }, { 0.75f, -0.75f });
        const shape_t s4 = HPOLY({ 0.85f, -0.6f }, { 1.4f, -0.75f }, { 1.0f, -0.3f });
        const shape_t u = uni(6, &top, &bangs, &s1, &s2, &s3, &s4);
        fill(pix, &u, col, 2);
        return;
    }
    case 1:                                                                           // short, side swept
        bangs = HPOLY({ -1.06f, -0.45f }, { -0.95f, 0.0f }, { -0.7f, -0.25f }, { 0.2f, -0.3f }, { 0.85f, -0.05f }, { 1.06f, -0.45f },
                      { 0.9f, -0.9f }, { -0.9f, -0.9f });
        break;
    case 2:                                                                           // bob: a straight fringe
        bangs = HPOLY({ -1.08f, -0.45f }, { -1.0f, 0.0f }, { -0.6f, -0.1f }, { -0.55f, -0.2f }, { 0.55f, -0.2f }, { 0.6f, -0.1f },
                      { 1.0f, 0.0f }, { 1.08f, -0.45f }, { 0.9f, -0.9f }, { -0.9f, -0.9f });
        break;
    case 3:                                                                           // long, parted
        bangs = HPOLY({ -1.08f, -0.4f }, { -0.92f, 0.55f }, { -0.72f, -0.15f }, { -0.15f, -0.5f }, { 0.0f, -0.3f }, { 0.15f, -0.5f },
                      { 0.72f, -0.15f }, { 0.92f, 0.55f }, { 1.08f, -0.4f }, { 0.9f, -0.9f }, { -0.9f, -0.9f });
        break;
    default:
        bangs = HPOLY({ -1.08f, -0.45f }, { -0.95f, 0.05f }, { -0.65f, -0.3f }, { -0.35f, -0.05f }, { -0.1f, -0.35f }, { 0.2f, -0.05f },
                      { 0.5f, -0.32f }, { 0.8f, 0.0f }, { 1.08f, -0.45f }, { 0.9f, -0.9f }, { -0.9f, -0.9f });
        break;
    }
    const shape_t u = uni(2, &top, &bangs);
    fill(pix, &u, col, 2);
    if (a->f[FH1_AV_HAIR_STYLE] == 3 && g->level != L_MINI) {                         // side locks
        for (int s = -1; s <= 1; s += 2) {
            const shape_t l = HPOLY({ s * 0.95f, -0.2f }, { s * 1.12f, -0.1f }, { s * 1.08f, 1.25f }, { s * 0.88f, 1.0f });
            fill(pix, &l, col, 2);
        }
    }
}

static void eyes(pix_t *pix, const cfg_t *cf, const expr_t *e, const head_t *g)
{
    const fh1_avatar_t *a = cf->a;
    const int lv = g->level;
    const uint32_t ink = INK, iris = PAL_EYES[a->f[FH1_AV_EYES]];
    const bool fem = a->f[FH1_AV_BASE] == 1;
    const float ex = g->rx * 0.42f, ey = g->cy + g->ry * 0.08f;
    const float ew = lv == L_MINI ? 1 : fmaxf(2, R(g->rx * 0.2f)), eh = lv == L_MINI ? 1 : fmaxf(2, R(g->ry * 0.24f));
    if (e->eyes == E_COOL) {                                                          // sunglasses over both
        const shape_t l = ellipse(g->cx - ex, ey, ew + 1.2f, eh), r = ellipse(g->cx + ex, ey, ew + 1.2f, eh);
        const shape_t gl = uni(2, &l, &r);
        flat(pix, &gl, 0x141420);
        line(pix, R(g->cx - ex + ew), R(ey - eh * 0.6f), R(g->cx + ex - ew), R(ey - eh * 0.6f), 0x141420, 1);
        if (lv != L_MINI) {
            pset(pix, R(g->cx - ex - ew * 0.4f), R(ey - eh * 0.4f), 0x9ab0d0);
            pset(pix, R(g->cx + ex - ew * 0.4f), R(ey - eh * 0.4f), 0x9ab0d0);
        }
        return;
    }
    for (int s = -1; s <= 1; s += 2) {
        int type = cf->blink ? E_CLOSED : e->eyes == E_WINK ? (s < 0 ? E_OPEN : E_SMILE) : e->eyes;
        const float cx = g->cx + s * ex;
        if (lv == L_MINI) {                                                           // 16 x 16: two pixels an eye
            const int x = R(cx - 0.5f), y = R(ey - 0.5f);
            if (type == E_SMILE) {
                pset(pix, x - 1, y + 1, ink), pset(pix, x, y, ink), pset(pix, x + 1, y + 1, ink);
            } else if (type == E_CLOSED || type == E_SLEEPY) {
                pset(pix, x - 1, y + 1, ink), pset(pix, x, y + 1, ink), pset(pix, x + 1, y + 1, ink);
            } else if (type == E_HEARTS) {
                pset(pix, x, y, 0xe8406a), pset(pix, x + 1, y, 0xe8406a), pset(pix, x, y + 1, 0xe8406a), pset(pix, x + 1, y + 1, 0xc02050);
            } else {
                pset(pix, x, y, ink);
                pset(pix, x, y + 1, type == E_WIDE ? ink : iris);
                if (type != E_WIDE) pset(pix, x + (type == E_SIDE ? 1 : 0), y + 1, iris);
            }
            continue;
        }
        const int lw = lv == L_FULL ? 2 : 1;
        if (type == E_SMILE) {                                                        // ^ ^
            line(pix, R(cx - ew), R(ey + 1), R(cx), R(ey - eh * 0.6f), ink, lw);
            line(pix, R(cx), R(ey - eh * 0.6f), R(cx + ew), R(ey + 1), ink, lw);
            continue;
        }
        if (type == E_CLOSED) {                                                       // a curved line down
            line(pix, R(cx - ew), R(ey), R(cx), R(ey + eh * 0.4f), ink, lw);
            line(pix, R(cx), R(ey + eh * 0.4f), R(cx + ew), R(ey), ink, lw);
            continue;
        }
        if (type == E_HEARTS) {
            const int k = lv == L_FULL ? 2 : 1;
            glyph(pix, G_HEART, R(cx - 2.5f * k), R(ey - 2.5f * k), 0xe8406a, k);
            continue;
        }
        // Open eyes: white, iris (darker on top), pupil, highlights, the upper lash.
        const bool wide = type == E_WIDE;
        const float rx = ew + (wide ? 0.6f : 0), ry = eh + (wide ? 0.6f : 0);
        const shape_t eye = ellipse(cx, ey, rx, ry);
        flat(pix, &eye, 0xfbfbff);
        const float ix = type == E_SIDE ? cx + rx * 0.35f * -s : cx;
        const shape_t ir = ellipse(ix, ey + 0.4f, rx * (wide ? 0.55f : 0.72f), ry * (wide ? 0.6f : 0.92f));
        for (int y = ir.y0; y <= ir.y1; y++) {
            for (int x = ir.x0; x <= ir.x1; x++) {
                if (!in(&ir, x, y) || !in(&eye, x, y)) continue;
                const float k = (y + 0.5f - (ey - ry)) / (2 * ry);
                pset(pix, x, y, k < 0.4f ? mix(iris, INK, 0.45f) : k > 0.75f ? mix(iris, 0xffffff, 0.35f) : iris);
            }
        }
        if (lv == L_FULL) {
            const shape_t pu = ellipse(ix, ey + 0.6f, rx * 0.28f, ry * 0.36f);
            flat(pix, &pu, mix(iris, INK, 0.7f));
        }
        pset(pix, R(ix - rx * 0.35f), R(ey - ry * 0.35f), 0xffffff);
        if (lv == L_FULL) {
            pset(pix, R(ix - rx * 0.35f) + 1, R(ey - ry * 0.35f), 0xffffff);
            pset(pix, R(ix + rx * 0.3f), R(ey + ry * 0.4f), 0xffffff);
        }
        // Lids: sleepy lowers it, angry slants it in, sad slants it out.
        const uint32_t skin = PAL_SKIN[a->f[FH1_AV_SKIN]];
        for (int x = (int)floorf(cx - rx - 1); x <= (int)ceilf(cx + rx + 1); x++) {
            const float dx = (x + 0.5f - cx) / (rx + 0.5f);
            float top = ey - ry * sqrtf(fmaxf(0, 1 - dx * dx)) - 0.2f;
            if (type == E_SLEEPY) top = ey - 0.2f;
            if (type == E_ANGRY) top += (dx * -s) * ry * 0.7f + ry * 0.25f;
            if (type == E_SAD) top += (dx * s) * ry * 0.6f + ry * 0.15f;
            for (int y = (int)floorf(ey - ry - 2); y < top; y++)
                if (in(&eye, x, y)) pset(pix, x, y, skin);
            const int ly = R(top);
            if (fabsf(dx) <= 1.05f) {
                pset(pix, x, ly, ink);
                if (lv == L_FULL) pset(pix, x, ly - 1, ink);
            }
        }
        // The outer corner's lash, longer for the female body.
        const int ox = R(cx + s * (rx + 0.5f)), oy = R(ey - ry * 0.55f);
        pset(pix, ox, oy, ink);
        if (fem) {
            pset(pix, ox + s, oy - 1, ink);
            if (lv == L_FULL) pset(pix, ox + 2 * s, oy - 2, ink);
        }
    }
}

static void brows(pix_t *pix, const expr_t *e, const fh1_avatar_t *a, const head_t *g)
{
    if (g->level == L_MINI || e->eyes == E_COOL) return;
    const uint32_t col = mix(PAL_HAIR[a->f[FH1_AV_HAIR]], INK, 0.55f);
    const int w = g->level == L_FULL ? 2 : 1;
    const float ex = g->rx * 0.42f, by = g->cy - g->ry * 0.22f, len = g->rx * 0.24f;
    for (int s = -1; s <= 1; s += 2) {
        float din = 0, dout = 0;
        if (e->brows == B_RAISED) din = dout = -g->ry * 0.08f;
        if (e->brows == B_ANGRY) din = g->ry * 0.1f, dout = -g->ry * 0.06f;
        if (e->brows == B_SAD) din = -g->ry * 0.1f, dout = g->ry * 0.05f;
        if (e->brows == B_ONE && s > 0) din = dout = -g->ry * 0.12f;
        const float cx = g->cx + s * ex;
        line(pix, R(cx - s * len), R(by + din), R(cx + s * len), R(by + dout - 1), col, w);
    }
}

static void mouth(pix_t *pix, const expr_t *e, const head_t *g)
{
    const uint32_t ink = INK, inner = 0x8a2030, tongue = 0xe86a80;
    const float my = g->cy + g->ry * 0.55f, mx = g->cx;
    const int lv = g->level, w = lv == L_FULL ? R(g->rx * 0.2f) : lv == L_FACE ? 2 : 1;
    if (lv == L_MINI) {
        const int x = R(mx - 0.5f), y = R(my);
        switch (e->mouth) {
        case M_GRIN: case M_LAUGH: case M_O: pset(pix, x, y, inner), pset(pix, x + 1, y, inner); break;
        case M_FROWN: case M_WAVY: pset(pix, x, y, ink), pset(pix, x + 1, y, ink); break;
        case M_FLAT: case M_SMALL: pset(pix, x, y, ink); break;
        default: pset(pix, x - 1, y - 1, ink), pset(pix, x, y, ink), pset(pix, x + 1, y, ink), pset(pix, x + 2, y - 1, ink); break;
        }
        return;
    }
    switch (e->mouth) {
    case M_GRIN: case M_LAUGH: {
        const int mh = e->mouth == M_LAUGH ? R(g->ry * 0.22f) : R(g->ry * 0.14f);
        const shape_t o = ellipse(mx, my - 1, w + 1, mh + 1);
        const shape_t m = clip_y(&o, R(my - 1), 999);
        flat(pix, &m, inner);
        if (lv == L_FULL) {
            const shape_t t = ellipse(mx, my + mh * 0.6f, w * 0.7f, mh * 0.6f);
            const shape_t tc = clip_y(&t, R(my), 999);
            flat(pix, &tc, tongue);
            line(pix, R(mx - w), R(my - 1), R(mx + w), R(my - 1), 0xffffff, 1);
        }
        break;
    }
    case M_O: {
        const shape_t o = ellipse(mx, my, fmaxf(1, w * 0.6f), fmaxf(1.2f, g->ry * 0.1f));
        flat(pix, &o, inner);
        break;
    }
    case M_FROWN:
        line(pix, R(mx - w), R(my + 1), R(mx), R(my), ink, 1);
        line(pix, R(mx), R(my), R(mx + w), R(my + 1), ink, 1);
        break;
    case M_FLAT: line(pix, R(mx - w * 0.6f), R(my), R(mx + w * 0.6f), R(my), ink, 1); break;
    case M_SMALL:
        pset(pix, R(mx), R(my), ink);
        if (lv == L_FULL) pset(pix, R(mx) + 1, R(my), ink);
        break;
    case M_WAVY:
        for (int i = -w; i <= w; i++) pset(pix, R(mx + i), R(my + ((i & 1) ? 1 : 0)), ink);
        break;
    case M_SMIRK: line(pix, R(mx - w * 0.6f), R(my), R(mx + w), R(my - 1), ink, 1); break;
    default:                                                                          // smile
        line(pix, R(mx - w), R(my - 1), R(mx), R(my), ink, 1);
        line(pix, R(mx), R(my), R(mx + w), R(my - 1), ink, 1);
        break;
    }
}

static void head(pix_t *pix, const cfg_t *cf, const expr_t *e, const head_t *g, float bottom, bool with_back)
{
    const fh1_avatar_t *a = cf->a;
    const uint32_t skin = PAL_SKIN[a->f[FH1_AV_SKIN]], accc = PAL_ACC[a->f[FH1_AV_ACC_COLOUR]];
    const int lv = g->level;
    if (with_back) back_hair(pix, a, g, bottom);
    // Ears, then the face: a round skull and a soft pointed chin.
    if (lv != L_MINI) {
        for (int s = -1; s <= 1; s += 2) {
            const shape_t ear = ellipse(g->cx + s * g->rx * 0.96f, g->cy + g->ry * 0.12f, g->rx * 0.16f, g->ry * 0.2f);
            fill(pix, &ear, skin, 1);
        }
    }
    const shape_t skull = ellipse(g->cx, g->cy - g->ry * 0.05f, g->rx, g->ry * 0.92f);
    const shape_t chin = HPOLY({ -0.92f, 0.25f }, { 0.92f, 0.25f }, { 0.32f, 0.92f }, { 0, 1.0f }, { -0.32f, 0.92f });
    const shape_t face = uni(2, &skull, &chin);
    fill(pix, &face, skin, lv == L_FULL ? 2 : 1);
    if (ACC(FH1_AV_ACC_EARRINGS) && lv != L_MINI) {
        for (int s = -1; s <= 1; s += 2) {
            const int x = R(g->cx + s * g->rx * 0.98f), y = R(g->cy + g->ry * 0.36f);
            pset(pix, x, y, accc);
            pset(pix, x, y + 1, mix(accc, INK, 0.4f));
        }
    }
    if (e->blush || ACC(FH1_AV_ACC_FACE_MARKS)) {
        for (int s = -1; s <= 1; s += 2) {
            const int bx = R(g->cx + s * g->rx * 0.55f), by = R(g->cy + g->ry * 0.38f);
            const uint32_t c = ACC(FH1_AV_ACC_FACE_MARKS) ? accc : 0xf08a98;
            if (lv == L_MINI) {
                pset(pix, bx, by, c);
                continue;
            }
            for (int i = 0; i < (lv == L_FULL ? 3 : 2); i++) pset(pix, bx - s * i * 2, by + (lv == L_FULL ? i % 2 : 0), c);
        }
    }
    if (lv == L_FULL) pset(pix, R(g->cx), R(g->cy + g->ry * 0.33f), mix(skin, INK, 0.35f));      // the nose
    eyes(pix, cf, e, g);
    mouth(pix, e, g);
    front_hair(pix, a, g);
    brows(pix, e, a, g);
    if ((ACC(FH1_AV_ACC_ROUND_GLASSES) || ACC(FH1_AV_ACC_SQUARE_GLASSES)) && e->eyes != E_COOL && lv != L_MINI) {
        const float ex = g->rx * 0.42f, ey = g->cy + g->ry * 0.08f, r = fmaxf(2, g->rx * 0.27f);
        for (int s = -1; s <= 1; s += 2) {
            const shape_t sh = ACC(FH1_AV_ACC_ROUND_GLASSES) ? ellipse(g->cx + s * ex, ey, r, r * 0.9f)
                                                             : rect(R(g->cx + s * ex - r), R(ey - r * 0.8f), R(r * 2), R(r * 1.6f));
            for (int y = sh.y0; y <= sh.y1; y++)
                for (int x = sh.x0; x <= sh.x1; x++)
                    if (in(&sh, x, y) && (!in(&sh, x - 1, y) || !in(&sh, x + 1, y) || !in(&sh, x, y - 1) || !in(&sh, x, y + 1)))
                        pset(pix, x, y, accc);
        }
        line(pix, R(g->cx - ex + r), R(ey), R(g->cx + ex - r), R(ey), accc, 1);
    }
}

// ── The body, for the full figure ──

typedef struct {
    int cx, neck, sh, waist, hip, feet, sw, ww;
} body_t;

static void body(pix_t *pix, const fh1_avatar_t *a, const body_t *P)
{
    const uint32_t skin = PAL_SKIN[a->f[FH1_AV_SKIN]], M = PAL_CLOTH[a->f[FH1_AV_TOP_COLOUR]], S = PAL_CLOTH[a->f[FH1_AV_BOTTOM_COLOUR]],
                   A = PAL_ACC[a->f[FH1_AV_ACC_COLOUR]], shoes = 0x2a2232;
    const bool fem = a->f[FH1_AV_BASE] == 1;
    const int cx = P->cx, sw = fem ? P->sw - 2 : P->sw, ww = fem ? P->ww - 2 : P->ww, hw = fem ? P->ww + 1 : P->ww;
    const int top = a->f[FH1_AV_TOP], b = a->f[FH1_AV_BOTTOM];
    // Legs (skin), shoes.
    for (int s = -1; s <= 1; s += 2) {
        const shape_t leg = POLY({ cx + s * 1, P->hip }, { cx + s * hw, P->hip }, { cx + s * (hw - 2), P->feet - 3 }, { cx + s * 2, P->feet - 3 });
        fill(pix, &leg, skin, 2);
        const shape_t shoe = POLY({ cx + s * 1, P->feet - 4 }, { cx + s * (hw - 1), P->feet - 4 }, { cx + s * (hw + 1), P->feet }, { cx + s * 1, P->feet });
        fill(pix, &shoe, shoes, 1);
    }
    // Bottoms.
    if (b == 0 || b == 1) {
        const int end = b == 0 ? P->feet - 4 : R((P->hip + P->feet) / 2.0f) - 2;
        for (int s = -1; s <= 1; s += 2) {
            const shape_t p = POLY({ cx, P->waist }, { cx + s * (ww + 0.5f), P->waist }, { cx + s * (hw + 0.5f), P->hip },
                                   { cx + s * (hw - 0.5f + (b == 0 ? 0 : 1)), end }, { cx + s * 1, end });
            fill(pix, &p, S, 2);
        }
    } else {
        const int end = b == 2 ? R((P->hip + P->feet) / 2.0f) + 1 : P->feet - 6, flare = b == 2 ? 6 : 5;
        const shape_t p = POLY({ cx - ww, P->waist }, { cx + ww, P->waist }, { cx + hw + flare, end }, { cx - hw - flare, end });
        fill(pix, &p, S, 2);
        for (int i = -2; i <= 2; i++) line(pix, R(cx + i * 3.2f), P->waist + 3, R(cx + i * 4.4f), end - 1, mix(S, INK, 0.35f), 1);   // pleats
    }
    // Arms behind the torso's edge: sleeves, forearms, hands.
    const bool long_sleeve = top != 0 && top != 4;
    for (int s = -1; s <= 1; s += 2) {
        const float shx = cx + s * (sw - 1), elx = cx + s * (sw + 2), ely = P->waist - 1, hx = cx + s * (sw + 1), hy = P->hip + 3;
        const shape_t up = POLY({ shx - s * 2, P->sh }, { shx + s * 3, P->sh + 2 }, { elx + s * 2, ely }, { elx - s * 3, ely });
        fill(pix, &up, M, 2);
        const shape_t fore = POLY({ elx - s * 3, ely }, { elx + s * 2, ely }, { hx + s * 2, hy }, { hx - s * 2, hy });
        fill(pix, &fore, long_sleeve ? M : skin, 2);
        const shape_t hand = ellipse(hx, hy + 2, 2.6f, 3);
        fill(pix, &hand, skin, 1);
        if (top == 3) {                                                               // gi: wide sleeves
            const shape_t sl = POLY({ elx - s * 3, ely }, { elx + s * 4, ely - 2 }, { elx + s * 5, ely + 7 }, { elx - s * 3, ely + 5 });
            fill(pix, &sl, M, 2);
        }
    }
    // Torso, neck.
    const shape_t torso = POLY({ cx - sw, P->sh + 2 }, { cx - sw + 3, P->sh }, { cx + sw - 3, P->sh }, { cx + sw, P->sh + 2 },
                               { cx + ww + 1, P->waist + 2 }, { cx - ww - 1, P->waist + 2 });
    fill(pix, &torso, M, 2);
    const shape_t neck = rect(cx - 3, P->neck, 7, P->sh - P->neck + 2);
    fill(pix, &neck, skin, 1);
    // Details per top.
    switch (top) {
    case 0: {
        const shape_t c = ellipse(cx + 0.5f, P->sh, 5, 3);
        const shape_t cc = clip_y(&c, P->sh, P->sh + 3);
        flat(pix, &cc, mix(skin, 0x3a2450, 0.2f));
        line(pix, cx - 5, P->sh + 1, cx - 3, P->sh + 3, mix(M, INK, 0.5f), 1);
        line(pix, cx + 6, P->sh + 1, cx + 4, P->sh + 3, mix(M, INK, 0.5f), 1);
        break;
    }
    case 1: {
        const shape_t hood = POLY({ cx - 8, P->sh - 1 }, { cx + 9, P->sh - 1 }, { cx + 6, P->sh + 4 }, { cx - 5, P->sh + 4 });
        fill(pix, &hood, S, 1);
        line(pix, cx - 2, P->sh + 3, cx - 2, P->sh + 10, S, 1);
        line(pix, cx + 3, P->sh + 3, cx + 3, P->sh + 10, S, 1);
        line(pix, cx - ww + 2, P->waist - 6, cx + ww - 1, P->waist - 6, mix(M, INK, 0.45f), 1);
        break;
    }
    case 2: {
        const shape_t shirt = POLY({ cx - 4, P->sh }, { cx + 5, P->sh }, { cx + 4, P->waist + 2 }, { cx - 3, P->waist + 2 });
        fill(pix, &shirt, S, 1);
        line(pix, cx - 4, P->sh, cx - 1, P->sh + 9, mix(M, INK, 0.55f), 1);
        line(pix, cx + 5, P->sh, cx + 2, P->sh + 9, mix(M, INK, 0.55f), 1);
        break;
    }
    case 3: {
        line(pix, cx - 6, P->sh, cx + 3, P->waist - 3, S, 2);
        line(pix, cx + 6, P->sh, cx - 2, P->waist - 6, S, 2);
        const shape_t belt = rect(cx - ww - 1, P->waist - 2, 2 * ww + 3, 4);
        fill(pix, &belt, S, 1);
        break;
    }
    case 4: {
        const shape_t collar = POLY({ cx - sw + 1, P->sh + 1 }, { cx + sw - 1, P->sh + 1 }, { cx + 4, P->sh + 9 }, { cx - 3, P->sh + 9 });
        fill(pix, &collar, S, 1);
        const shape_t tie = POLY({ cx - 2, P->sh + 7 }, { cx + 3, P->sh + 7 }, { cx + 1, P->sh + 14 });
        fill(pix, &tie, A, 0);
        break;
    }
    default: break;
    }
    if (ACC(FH1_AV_ACC_SCARF)) {
        const shape_t w1 = POLY({ cx - 6, P->neck + 3 }, { cx + 7, P->neck + 3 }, { cx + 7, P->sh + 3 }, { cx - 6, P->sh + 3 });
        fill(pix, &w1, A, 2);
        const shape_t end = POLY({ cx + 2, P->sh + 2 }, { cx + 6, P->sh + 2 }, { cx + 7, P->sh + 13 }, { cx + 3, P->sh + 12 });
        fill(pix, &end, A, 2);
    }
    if (ACC(FH1_AV_ACC_NECKLACE)) {
        for (int i = -4; i <= 4; i++) pset(pix, cx + i, P->sh + 2 + R((16 - i * i) / 6.0f), A);
        pset(pix, cx, P->sh + 6, mix(A, 0xffffff, 0.5f));
    }
}

// ── Effects, drawn over the figure; and the head's motion ──

static float fmod1(float x) { return x - floorf(x); }

static void effect(pix_t *pix, int fx, const head_t *g, uint32_t t)
{
    const int lv = g->level, k = lv == L_FULL ? 2 : 1;
    const float cx = g->cx, top = g->cy - g->ry * 1.1f, Rr = g->rx, ph = (t % 1600) / 1600.0f;
    const bool blink = (t / 250) % 2;
    switch (fx) {
    case FX_HEARTS:
        for (int i = 0; i < (lv == L_MINI ? 1 : 3); i++) {
            const float p = fmod1(ph + i / 3.0f);
            glyph(pix, G_HEART, R(cx + Rr * (0.9f + 0.3f * sinf(p * 6 + i))), R(top + Rr * 0.6f - p * Rr), i % 2 ? 0xff6a90 : 0xe8406a, 1);
        }
        break;
    case FX_SPARKLES: {
        static const float dx[3] = { -1.3f, 1.15f, 0.9f }, dy[3] = { 0.2f, 0.05f, 0.9f };
        for (int i = 0; i < 3; i++)
            if ((t / 200 + i) % 3) glyph(pix, lv == L_MINI ? G_STAR : G_SPARK, R(cx + Rr * dx[i]), R(top + Rr * dy[i]), 0xffe070, 1);
        break;
    }
    case FX_JOY:
        for (int s = -1; s <= 1; s += 2) {
            const float p = fmod1(ph * 2);
            const int x = R(cx + s * Rr * (0.8f + p)), y = R(g->cy + g->ry * (p * 0.3f));
            pset(pix, x, y, 0x7ad0ff);
            pset(pix, x + s, y, 0xbfe8ff);
        }
        break;
    case FX_GLOOM:
        for (int i = 0; i < 4; i++) {
            const int x = R(cx - Rr * 0.6f + i * Rr * 0.4f);
            line(pix, x, R(top - 2), x, R(top + Rr * 0.25f), 0x5a6aa0, 1);
        }
        break;
    case FX_TEARS:
        for (int s = -1; s <= 1; s += 2) {
            const int ex = R(cx + s * Rr * 0.42f), ey = R(g->cy + g->ry * 0.2f);
            line(pix, ex, ey, ex, R(ey + g->ry * 0.5f), 0x6ac0ff, 1);
            if (lv != L_MINI) pset(pix, ex, R(ey + g->ry * (0.5f + ph * 0.5f)), 0xbfe8ff);
        }
        break;
    case FX_ANGER:
        if (blink || lv != L_FULL) glyph(pix, G_ANGER, R(cx + Rr * 0.75f), R(top + Rr * 0.1f), 0xff3a3a, 1);
        break;
    case FX_EXCLAIM: glyph(pix, G_EXCL, R(cx + Rr * 1.1f), R(top - (blink ? 1 : 0) - (lv == L_MINI ? 0 : 2)), 0xffe070, k); break;
    case FX_STAR:
        if (blink) glyph(pix, G_STAR, R(cx + Rr * 0.8f), R(g->cy - g->ry * 0.15f), 0xffe070, 1);
        break;
    case FX_ZZZ:
        for (int i = 0; i < (lv == L_MINI ? 1 : 2); i++) {
            const float p = fmod1(ph + i * 0.5f);
            glyph(pix, G_Z, R(cx + Rr * (0.9f + p * 0.5f)), R(top + Rr * 0.5f - p * Rr), 0xa8c0ff, lv == L_FULL && i == 0 ? 2 : 1);
        }
        break;
    case FX_GLINT:
        if (blink) glyph(pix, G_SPARK, R(cx - Rr * 0.65f), R(g->cy - g->ry * 0.12f), 0xffffff, 1);
        break;
    case FX_QUESTION:
        glyph(pix, G_Q, R(cx + Rr * 1.05f), R(top - (lv == L_MINI ? 0 : 1) + R(sinf(t / 250.0f))), 0xffe070, k);
        break;
    default: break;
    }
}

// Every face moves: a bounce for joy, a shake for anger, a droop for sadness; the others breathe.
static void motion(int expr, uint32_t t, float *mx, float *my)
{
    const float tf = (float)(t % 1000000u);
    float x = 0, y = 0;
    switch (expr) {
    case FH1_AV_HAPPY: y = -fabsf(sinf(tf / 180)) * 1.5f; break;
    case FH1_AV_LAUGH: y = -fabsf(sinf(tf / 110)) * 1.8f; break;
    case FH1_AV_SAD: y = 0.8f + sinf(tf / 700) * 0.6f; break;
    case FH1_AV_CRY: x = sinf(tf / 60) * 0.6f, y = 0.6f; break;
    case FH1_AV_ANGRY: x = ((t / 70) % 2 ? 1 : -1) * 0.8f; break;
    case FH1_AV_SURPRISED: y = (t % 1200) < 160 ? -2 : 0; break;
    case FH1_AV_WINK: y = -fabsf(sinf(tf / 300)); break;
    case FH1_AV_LOVE: x = sinf(tf / 400), y = -fabsf(sinf(tf / 400)); break;
    case FH1_AV_SLEEPY: x = sinf(tf / 900) * 0.8f, y = sinf(tf / 900) * 0.8f + 0.5f; break;
    case FH1_AV_THINK: x = sinf(tf / 600) * 0.8f; break;
    case FH1_AV_COOL: y = ((t / 500) % 2) * 0.6f; break;
    default: y = ((t / 900) % 2) * 0.6f; break;                                      // neutral: breathing
    }
    *mx = x, *my = y;
}

static bool blinking(const expr_t *e, uint32_t t)
{
    const bool open = e->eyes == E_OPEN || e->eyes == E_WIDE || e->eyes == E_SIDE || e->eyes == E_ANGRY || e->eyes == E_SAD;
    return open && (t + e->blink_at) % 3200 < 140;
}

// ── Views ──

static void sane(const fh1_avatar_t *in_, fh1_avatar_t *out)
{
    *out = *in_;
    for (int f = 0; f < FH1_AV_FIELDS; f++)
        if (out->f[f] >= fh1_avatar_choices(f)) out->f[f] = 0;
    out->acc &= (1 << FH1_AV_ACCS) - 1;
}

void fh1_avatar_render_full(const fh1_avatar_t *av, int expr, uint32_t t, uint32_t *px)
{
    fh1_avatar_t a;
    sane(av, &a);
    if (expr < 0 || expr >= FH1_AV_EXPRS) expr = 0;
    const expr_t *e = &EXPRS[expr];
    pix_t pix = { FH1_AVATAR_FULL_W, FH1_AVATAR_FULL_H, px };
    memset(px, 0, FH1_AVATAR_FULL_W * FH1_AVATAR_FULL_H * 4);
    const body_t P = { 48, 50, 56, 84, 88, 125, 16, 12 };
    const head_t g = { 48, 34, 16, 18, L_FULL };
    const cfg_t cf = { &a, blinking(e, t) };
    back_hair(&pix, &a, &g, P.waist);            // long hair goes behind the body
    body(&pix, &a, &P);
    head(&pix, &cf, e, &g, P.waist, false);
    outline(&pix, INK);
    effect(&pix, e->fx, &g, t);
}

void fh1_avatar_render_face(const fh1_avatar_t *av, int expr, uint32_t t, int size, uint32_t *px)
{
    fh1_avatar_t a;
    sane(av, &a);
    if (expr < 0 || expr >= FH1_AV_EXPRS) expr = 0;
    const expr_t *e = &EXPRS[expr];
    pix_t pix = { size, size, px };
    memset(px, 0, (size_t)size * size * 4);
    head_t g = size >= 32 ? (head_t){ 16, 16, 10.5f, 11.5f, L_FACE } : (head_t){ 8, 8.5f, 5.6f, 6.0f, L_MINI };
    float mx, my;
    const float k = size >= 32 ? 1 : 0.6f;
    motion(expr, t, &mx, &my);
    g.cy += R(my * k);
    g.cx += R(mx * k);
    const cfg_t cf = { &a, blinking(e, t) };
    head(&pix, &cf, e, &g, (float)size, true);
    outline(&pix, INK);
    effect(&pix, e->fx, &g, t);
}

// ── Codes and lists ──

size_t fh1_avatar_encode(const fh1_avatar_t *a, uint8_t *out, size_t max)
{
    if (max < FH1_AVATAR_CODE_LEN) return 0;
    out[0] = 1;
    memcpy(out + 1, a->f, FH1_AV_FIELDS);
    out[1 + FH1_AV_FIELDS] = a->acc;
    return FH1_AVATAR_CODE_LEN;
}

bool fh1_avatar_decode(const uint8_t *in_, size_t n, fh1_avatar_t *a)
{
    if (n < 2 || in_[0] != 1) return false;
    fh1_avatar_t raw = { 0 };
    for (int f = 0; f < FH1_AV_FIELDS && 1 + (size_t)f < n; f++) raw.f[f] = in_[1 + f];
    if (n > 1 + FH1_AV_FIELDS) raw.acc = in_[1 + FH1_AV_FIELDS];
    sane(&raw, a);
    return true;
}

void fh1_avatar_from_seed(const void *seed, size_t n, fh1_avatar_t *a)
{
    // FNV-1a, then a small generator: each field from the next number.
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t *)seed)[i]) * 16777619u;
    memset(a, 0, sizeof(*a));
    for (int f = 0; f < FH1_AV_FIELDS; f++) {
        h ^= h << 13, h ^= h >> 17, h ^= h << 5;
        a->f[f] = h % fh1_avatar_choices(f);
    }
    h ^= h << 13, h ^= h >> 17, h ^= h << 5;
    if (h % 10 < 3) a->acc = 1 << (h / 10 % FH1_AV_ACCS);       // now and then one accessory
}

int fh1_avatar_choices(int field)
{
    switch (field) {
    case FH1_AV_BASE: return N(BASES);
    case FH1_AV_HAIR_STYLE: return N(HAIRS);
    case FH1_AV_HAIR: return N(PAL_HAIR);
    case FH1_AV_SKIN: return N(PAL_SKIN);
    case FH1_AV_EYES: return N(PAL_EYES);
    case FH1_AV_TOP: return N(TOPS);
    case FH1_AV_TOP_COLOUR: case FH1_AV_BOTTOM_COLOUR: return N(PAL_CLOTH);
    case FH1_AV_BOTTOM: return N(BOTTOMS);
    case FH1_AV_ACC_COLOUR: return N(PAL_ACC);
    }
    return 1;
}

const char *fh1_avatar_field_name(int field) { return field >= 0 && field < FH1_AV_FIELDS ? FIELDS[field] : ""; }

const char *fh1_avatar_choice_name(int field, int i)
{
    if (i < 0 || i >= fh1_avatar_choices(field)) return NULL;
    switch (field) {
    case FH1_AV_BASE: return BASES[i];
    case FH1_AV_HAIR_STYLE: return HAIRS[i];
    case FH1_AV_TOP: return TOPS[i];
    case FH1_AV_BOTTOM: return BOTTOMS[i];
    }
    return NULL;
}

uint32_t fh1_avatar_choice_colour(int field, int i)
{
    if (i < 0 || i >= fh1_avatar_choices(field)) return 0;
    switch (field) {
    case FH1_AV_HAIR: return PAL_HAIR[i];
    case FH1_AV_SKIN: return PAL_SKIN[i];
    case FH1_AV_EYES: return PAL_EYES[i];
    case FH1_AV_TOP_COLOUR: case FH1_AV_BOTTOM_COLOUR: return PAL_CLOTH[i];
    case FH1_AV_ACC_COLOUR: return PAL_ACC[i];
    }
    return 0;
}

const char *fh1_avatar_acc_name(int acc) { return acc >= 0 && acc < FH1_AV_ACCS ? ACCS[acc] : ""; }
const char *fh1_avatar_expr_name(int expr) { return expr >= 0 && expr < FH1_AV_EXPRS ? EXPRS[expr].name : ""; }
