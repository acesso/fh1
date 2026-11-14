#include "fart.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

bool fart_load(fart_t *a, const char *path)
{
    memset(a, 0, sizeof(*a));
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    a->data = size > 12 ? malloc(size) : NULL;           // large: the heap puts it in PSRAM
    bool ok = a->data && fread(a->data, 1, size, f) == (size_t)size;
    fclose(f);
    const int ver = ok && size > 12 ? rd16(a->data + 4) : 0;
    if (!ok || memcmp(a->data, "FART", 4) != 0 || (ver != 2 && ver != 3)) {
        fart_free(a);
        return false;
    }
    a->nimg = rd16(a->data + 6);
    a->nanim = rd16(a->data + 8);
    a->img = calloc(a->nimg, sizeof(fart_img_t));
    // Over 4 KB, so the heap puts it in PSRAM (ESP-IDF serves smaller blocks from internal RAM, Wi-Fi's).
    const size_t at = (a->nanim ? a->nanim : 1) * sizeof(fart_anim_t);
    a->anim = calloc(at < 4160 ? 4160 : at, 1);
    if (!a->img || !a->anim) {
        fart_free(a);
        return false;
    }
    const uint8_t *p = a->data + 12;
    for (int i = 0; i < a->nimg; i++, p += ver >= 3 ? 48 : 44) {
        fart_img_t *im = &a->img[i];
        im->name = (const char *)p;
        im->ox = (int16_t)rd16(p + 24);
        im->oy = (int16_t)rd16(p + 26);
        im->w = rd16(p + 28);
        im->h = rd16(p + 30);
        im->spans = (const uint16_t *)(a->data + rd32(p + 32));   // offsets are even
        im->hx = (int16_t)rd16(p + 36);
        im->hy = (int16_t)rd16(p + 38);
        const uint32_t po = rd32(p + 40);
        im->parts = po ? a->data + po : NULL;
        const uint32_t eo = ver >= 3 ? rd32(p + 44) : 0;
        im->edge = eo ? (const uint16_t *)(a->data + eo) : NULL;
    }
    for (int i = 0; i < a->nanim; i++) {
        fart_anim_t *an = &a->anim[i];
        an->name = (const char *)p;
        an->dirs = p[24];
        an->frames = p[25];
        an->ms = rd16(p + 26);
        an->idx = (const uint16_t *)(p + 28);
        p += 28 + an->dirs * an->frames * 2;
    }
    return true;
}

void fart_free(fart_t *a)
{
    while (a->pool) {
        uint8_t *next = *(uint8_t **)a->pool;
        free(a->pool);
        a->pool = next;
    }
    free(a->orig);
    free(a->data);
    free(a->img);
    free(a->anim);
    memset(a, 0, sizeof(*a));
}

static const uint8_t s_bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

void fart_draw_dither(uint16_t *dst, int stride, const fart_clip_t *clip, const fart_img_t *im, int ax, int ay,
                      bool mirror, int level)
{
    if (level <= 0) return;
    const int top = ay + im->oy, left = mirror ? ax - im->ox - 1 : ax + im->ox;
    const uint16_t *p = im->spans;
    for (int row = 0; row < im->h; row++) {
        const int y = top + row;
        int runs = *p++, x = 0;
        const bool on = y >= clip->y0 && y < clip->y1;
        while (runs--) {
            x += *p++;
            const int len = *p++;
            for (int i = 0; on && i < len; i++) {
                const int sx = mirror ? left - (x + i) : left + x + i;
                if (sx < clip->x0 || sx >= clip->x1 || s_bayer[y & 3][sx & 3] >= level) continue;
                dst[y * stride + sx] = p[i];
            }
            p += len;
            x += len;
        }
    }
}

bool fart_unpack(const fart_img_t *im, uint16_t *px, uint8_t *mask, int max)
{
    if (im->w > max || im->h > max) return false;
    memset(mask, 0, (size_t)im->w * im->h);
    const uint16_t *p = im->spans;
    for (int row = 0; row < im->h; row++) {
        int runs = *p++, x = 0;
        while (runs--) {
            x += *p++;
            const int len = *p++;
            for (int i = 0; i < len; i++) {
                px[row * im->w + x + i] = p[i];
                mask[row * im->w + x + i] = 1;
            }
            p += len;
            x += len;
        }
    }
    return true;
}

// How many 16-bit words an image's spans take.
static size_t span_words(const fart_img_t *im)
{
    const uint16_t *p = im->spans;
    for (int row = 0; row < im->h; row++) {
        int runs = *p++;
        while (runs--) {
            const int len = p[1];
            p += 2 + len;
        }
    }
    return (size_t)(p - im->spans);
}

// Room for an original's copy. Small blocks would come from internal RAM (ESP-IDF serves blocks of up to 4 KB from
// there: Wi-Fi's), so copies share 16 KB blocks, which come from PSRAM; one larger than a block gets its own.
#define POOL_BLOCK 16384
static void *pool_take(fart_t *a, size_t n)
{
    n = (n + 7) & ~(size_t)7;
    const size_t head = sizeof(uint8_t *) > 8 ? sizeof(uint8_t *) : 8;
    if (!a->pool || a->pool_used + n > POOL_BLOCK) {
        const size_t size = n + head > POOL_BLOCK ? n + head : POOL_BLOCK;
        uint8_t *b = malloc(size);
        if (!b) return NULL;
        *(uint8_t **)b = a->pool;
        a->pool = b;
        a->pool_used = head;
        if (size > POOL_BLOCK) {                                  // its own block: full at once
            a->pool_used = POOL_BLOCK;
            return b + head;
        }
    }
    void *p = a->pool + a->pool_used;
    a->pool_used += n;
    return p;
}

bool fart_recolor(fart_t *a, int idx, uint16_t (*fn)(uint16_t v, void *user), void *user)
{
    if (idx < 0 || idx >= a->nimg) return false;
    // More than 4 KB, so it comes from PSRAM.
    if (!a->orig && !(a->orig = calloc(a->nimg < 1040 ? 1040 : a->nimg, sizeof(uint16_t *)))) return false;
    fart_img_t *im = &a->img[idx];
    const size_t n = span_words(im);
    if (!a->orig[idx]) {
        if (!fn) return true;                                     // never recoloured: already as loaded
        if (!(a->orig[idx] = pool_take(a, n * 2))) return false;
        memcpy(a->orig[idx], im->spans, n * 2);
    }
    const uint16_t *o = a->orig[idx];
    uint16_t *d = (uint16_t *)im->spans;                          // the pack is in RAM: its pixels can change
    size_t i = 0;
    for (int row = 0; row < im->h; row++) {
        int runs = o[i++];
        while (runs--) {
            const int len = o[i + 1];
            i += 2;
            for (int k = 0; k < len; k++, i++) d[i] = fn ? fn(o[i], user) : o[i];
        }
    }
    return true;
}

int fart_image(const fart_t *a, const char *name)
{
    for (int i = 0; i < a->nimg; i++)
        if (strcmp(a->img[i].name, name) == 0) return i;
    return -1;
}

const fart_anim_t *fart_anim(const fart_t *a, const char *name)
{
    for (int i = 0; i < a->nanim; i++)
        if (strcmp(a->anim[i].name, name) == 0) return &a->anim[i];
    return NULL;
}

int fart_frame(const fart_anim_t *an, int dir, uint32_t t_ms, bool *mirror)
{
    *mirror = false;
    if (!an || !an->frames) return -1;
    const int f = an->ms ? (int)((t_ms / an->ms) % an->frames) : 0;
    int i = an->idx[dir * an->frames + f];
    if (i == FART_NONE) {
        static const uint8_t twin[FART_DIRS] = { FART_SW, FART_SE, FART_NE, FART_NW };
        i = an->idx[twin[dir] * an->frames + f];
        *mirror = true;
    }
    return i == FART_NONE ? -1 : i;
}

int fart_frame8(const fart_anim_t *an, int oct, uint32_t t_ms, bool *mirror)
{
    oct &= 7;
    if (oct & 1) return fart_frame(an, oct >> 1, t_ms, mirror);
    *mirror = false;
    if (!an || !an->frames) return -1;
    if (an->dirs >= 8) {
        static const uint8_t slot[4] = { 7, 4, 5, 6 };          // e, s, w, n (by oct / 2) in the pack's order
        const int f = an->ms ? (int)((t_ms / an->ms) % an->frames) : 0;
        int i = an->idx[slot[oct >> 1] * an->frames + f];
        if (i == FART_NONE && !(oct & 2)) {                        // e or w: the other one, mirrored
            i = an->idx[slot[(oct >> 1) ^ 2] * an->frames + f];
            *mirror = i != FART_NONE;
        }
        if (i != FART_NONE) return i;
    }
    return fart_frame(an, ((oct + 1) & 7) >> 1, t_ms, mirror);
}

static inline uint16_t tinted(uint16_t v, int tint)
{
    return tint == FART_WHITE ? 0xFFFF : (uint16_t)((v >> 1) & 0x7BEF);
}

void fart_draw(uint16_t *dst, int stride, const fart_clip_t *clip, const fart_img_t *im, int ax, int ay, bool mirror)
{
    fart_draw_tint(dst, stride, clip, im, ax, ay, mirror, FART_PLAIN);
}

void fart_draw_tint(uint16_t *dst, int stride, const fart_clip_t *clip, const fart_img_t *im, int ax, int ay, bool mirror,
                    int tint)
{
    fart_draw_dressed(dst, stride, clip, im, ax, ay, mirror, tint, 0, NULL);
}

static inline uint16_t dress(uint16_t v, const uint16_t ramp[4])
{
    const int r = (v >> 11) << 3, g = ((v >> 5) & 63) << 2, b = (v & 31) << 3;
    const int lum = (r * 3 + g * 6 + b) / 10;
    if (lum < 40) return v;                          // the outline
    return ramp[lum >= 192 ? 3 : lum >> 6];
}

static inline uint16_t shaded(uint16_t v, const uint16_t shade[32])
{
    const int r = (v >> 11) << 3, g = ((v >> 5) & 63) << 2, b = (v & 31) << 3;
    const int lum = (r * 3 + g * 6 + b) / 10;
    return lum < 20 ? v : shade[lum >> 3];           // the outline keeps its colour
}

// One pixel of image im (index pi into its w x h) as the style draws it.
static inline uint16_t styled(uint16_t v, const fart_img_t *im, int pi, const fart_style_t *st, bool dressing)
{
    if (dressing) {
        const int part = (im->parts[pi >> 2] >> ((pi & 3) * 2)) & 3;
        if (part && (st->cover & (1u << part))) v = dress(v, st->ramp);
    }
    if (st->shade) v = shaded(v, st->shade);
    if (st->fade) {                                  // its colour drained toward a dim grey of its brightness
        const int r = (v >> 11) << 3, g = ((v >> 5) & 63) << 2, b = (v & 31) << 3;
        const int grey = (r * 3 + g * 6 + b) / 10 * 5 / 9, f = st->fade;
        const int R = r + (grey - r) * f / 255, G = g + (grey - g) * f / 255, B = b + (grey + 6 - b) * f / 255;
        v = (uint16_t)(((R >> 3) << 11) | ((G >> 2) << 5) | (B >> 3));
    }
    return st->tint ? tinted(v, st->tint) : v;
}

void fart_draw_dressed(uint16_t *dst, int stride, const fart_clip_t *clip, const fart_img_t *im, int ax, int ay,
                       bool mirror, int tint, uint8_t cover, const uint16_t ramp[4])
{
    const fart_style_t st = { .tint = tint, .cover = cover, .ramp = ramp };
    fart_draw_style(dst, stride, clip, im, ax, ay, mirror, &st);
}

// Zoomed or tilted: the image unpacked, then every screen pixel of the turned, enlarged box looks up its source.
#define BIG 96
static void draw_turned(uint16_t *dst, int stride, const fart_clip_t *clip, const fart_img_t *im, int ax, int ay,
                        bool mirror, const fart_style_t *st, bool dressing)
{
    static uint16_t px[BIG * BIG];
    static uint8_t on[BIG * BIG];
    if (im->w > BIG || im->h > BIG) return;
    memset(on, 0, (size_t)im->w * im->h);
    const uint16_t *p = im->spans;
    for (int row = 0; row < im->h; row++) {
        int runs = *p++, x = 0;
        while (runs--) {
            x += *p++;
            const int len = *p++;
            for (int i = 0; i < len; i++) {
                const int pi = row * im->w + x + i;
                px[pi] = styled(p[i], im, pi, st, dressing);
                on[pi] = 1;
            }
            p += len;
            x += len;
        }
    }
    const int z = st->zoom > 1 ? st->zoom : 1;
    const float a = st->tilt * 0.0174533f, c = cosf(a), s = sinf(a);
    // The box: the image's corners from the anchor, enlarged and turned.
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int k = 0; k < 4; k++) {
        float cx = (k & 1 ? im->ox + im->w : im->ox) * (float)z, cy = (k & 2 ? im->oy + im->h : im->oy) * (float)z;
        if (mirror) cx = -cx;
        const float rx = cx * c - cy * s, ry = cx * s + cy * c;
        if (rx < x0) x0 = rx;
        if (rx > x1) x1 = rx;
        if (ry < y0) y0 = ry;
        if (ry > y1) y1 = ry;
    }
    // (floorf + 2, not ceilf: the firmware exports floorf only.)
    int bx0 = ax + (int)floorf(x0) - 1, bx1 = ax + (int)floorf(x1) + 2, by0 = ay + (int)floorf(y0) - 1, by1 = ay + (int)floorf(y1) + 2;
    if (bx0 < clip->x0) bx0 = clip->x0;
    if (by0 < clip->y0) by0 = clip->y0;
    if (bx1 > clip->x1) bx1 = clip->x1;
    if (by1 > clip->y1) by1 = clip->y1;
    for (int y = by0; y < by1; y++)
        for (int x = bx0; x < bx1; x++) {
            const float dx = x - ax + 0.5f, dy = y - ay + 0.5f;
            float sx = (dx * c + dy * s) / z, sy = (-dx * s + dy * c) / z;
            if (mirror) sx = -sx;
            const int ix = (int)floorf(sx) - im->ox, iy = (int)floorf(sy) - im->oy;
            if (ix < 0 || iy < 0 || ix >= im->w || iy >= im->h || !on[iy * im->w + ix]) continue;
            dst[y * stride + x] = px[iy * im->w + ix];
        }
}

static const uint16_t *s_from, *s_to;
static int s_nmap;
void fart_map(const uint16_t *from, const uint16_t *to, int n)
{
    s_from = from;
    s_to = to;
    s_nmap = from && to ? n : 0;
}
static inline uint16_t mapped(uint16_t c)
{
    for (int i = 0; i < s_nmap; i++)
        if (c == s_from[i]) return s_to[i];
    return c;
}

// c over d at alpha a (0..255), RGB565: the channels spread apart in 32 bits, mixed at once (5-bit alpha).
static inline uint16_t blend565(uint16_t d, uint16_t c, int a)
{
    const uint32_t k = (uint32_t)(a + 4) >> 3;
    const uint32_t dd = (d | (uint32_t)d << 16) & 0x07E0F81F, cc = (c | (uint32_t)c << 16) & 0x07E0F81F;
    const uint32_t m = (dd + ((cc - dd) * k >> 5)) & 0x07E0F81F;
    return (uint16_t)(m | m >> 16);
}

// An image's soft edge over what is drawn (after its opaque runs).
static void draw_edge(uint16_t *dst, int stride, const fart_clip_t *clip, const fart_img_t *im, int top, int left,
                      bool mirror)
{
    const uint16_t *p = im->edge;
    for (int row = 0; row < im->h; row++) {
        const int n = *p++, y = top + row;
        if (y >= clip->y0 && y < clip->y1) {
            uint16_t *line = dst + y * stride;
            for (int i = 0; i < n; i++, p += 3) {
                const int s = mirror ? left - p[0] : left + p[0];
                if (s >= clip->x0 && s < clip->x1) line[s] = blend565(line[s], mapped(p[1]), p[2]);
            }
        } else {
            p += 3 * n;
        }
    }
}

void fart_draw_zoom(uint16_t *dst, int stride, const fart_clip_t *clip, const fart_img_t *im, int ax, int ay,
                    bool mirror, int z256)
{
    enum { ZMAX = 96 };
    static uint16_t px[ZMAX * ZMAX];
    static uint8_t mask[ZMAX * ZMAX];
    if (z256 <= 0 || !fart_unpack(im, px, mask, ZMAX)) return;
    // The box's middle on screen, where it stays; each pixel of the scaled box takes the source pixel under it.
    const int left = mirror ? ax - im->ox - im->w : ax + im->ox, top = ay + im->oy;
    const int cx2 = 2 * left + im->w, cy2 = 2 * top + im->h;             // twice the middle
    const int sw = im->w * z256 / 256, sh = im->h * z256 / 256;
    if (sw <= 0 || sh <= 0) return;
    const int x0 = (cx2 - sw) / 2, y0 = (cy2 - sh) / 2;
    for (int y = 0; y < sh; y++) {
        const int dy = y0 + y;
        if (dy < clip->y0 || dy >= clip->y1) continue;
        const int sy = y * im->h / sh;
        for (int x = 0; x < sw; x++) {
            const int dx = x0 + x;
            if (dx < clip->x0 || dx >= clip->x1) continue;
            int sx = x * im->w / sw;
            if (mirror) sx = im->w - 1 - sx;
            if (mask[sy * im->w + sx]) dst[dy * stride + dx] = mapped(px[sy * im->w + sx]);
        }
    }
}

void fart_draw_alpha(uint16_t *dst, int stride, const fart_clip_t *clip, const fart_img_t *im, int ax, int ay,
                     bool mirror, int alpha)
{
    fart_draw_ghost(dst, stride, clip, im, ax, ay, mirror, alpha, 0, 0);
}

void fart_draw_ghost(uint16_t *dst, int stride, const fart_clip_t *clip, const fart_img_t *im, int ax, int ay,
                     bool mirror, int alpha, uint16_t tint, int tint_a)
{
    if (alpha <= 0) return;
    if (alpha > 255) alpha = 255;
    const int top = ay + im->oy, left = mirror ? ax - im->ox - 1 : ax + im->ox;
    const uint16_t *p = im->spans;
    for (int row = 0; row < im->h; row++) {
        const int y = top + row;
        int runs = *p++, x = 0;
        const bool on = y >= clip->y0 && y < clip->y1;
        while (runs--) {
            x += *p++;
            const int len = *p++;
            for (int i = 0; on && i < len; i++) {
                const int sx = mirror ? left - (x + i) : left + x + i;
                if (sx >= clip->x0 && sx < clip->x1)
                    dst[y * stride + sx] = blend565(dst[y * stride + sx], tint_a ? blend565(mapped(p[i]), tint, tint_a) : mapped(p[i]),
                                                    alpha);
            }
            p += len;
            x += len;
        }
    }
    if (!im->edge) return;
    p = im->edge;
    for (int row = 0; row < im->h; row++) {
        const int n = *p++, y = top + row;
        for (int i = 0; i < n; i++, p += 3) {
            const int s = mirror ? left - p[0] : left + p[0];
            if (y >= clip->y0 && y < clip->y1 && s >= clip->x0 && s < clip->x1)
                dst[y * stride + s] = blend565(dst[y * stride + s], tint_a ? blend565(mapped(p[1]), tint, tint_a) : mapped(p[1]),
                                               p[2] * alpha / 255);
        }
    }
}

void fart_draw_style(uint16_t *dst, int stride, const fart_clip_t *clip, const fart_img_t *im, int ax, int ay,
                     bool mirror, const fart_style_t *st)
{
    const bool dressing = st->cover && st->ramp && im->parts;
    if ((st->zoom > 1) || st->tilt) {
        draw_turned(dst, stride, clip, im, ax, ay, mirror, st, dressing);
        return;
    }
    const bool plain = st->tint == FART_PLAIN && !dressing && !st->shade && !st->fade;
    const int top = ay + im->oy;
    const int left = mirror ? ax - im->ox - 1 : ax + im->ox;      // mirrored: column c lands at left - c
    if (top >= clip->y1 || top + im->h <= clip->y0) return;
    if (!mirror && (left >= clip->x1 || left + im->w <= clip->x0)) return;
    if (mirror && (left < clip->x0 || left - im->w + 1 >= clip->x1)) return;
    const uint16_t *p = im->spans;
    for (int row = 0; row < im->h; row++) {
        const int y = top + row;
        int runs = *p++;
        int x = 0;
        const bool on = y >= clip->y0 && y < clip->y1;
        uint16_t *line = dst + y * stride;
        while (runs--) {
            x += *p++;
            const int len = *p++;
            if (on) {
                if (!mirror) {
                    int s = left + x, a = 0, n = len;
                    if (s < clip->x0) { a = clip->x0 - s; n -= a; s = clip->x0; }
                    if (s + n > clip->x1) n = clip->x1 - s;
                    if (n > 0 && plain && !s_nmap) memcpy(line + s, p + a, n * 2);
                    else if (plain)
                        for (int i = 0; i < n; i++) line[s + i] = mapped(p[a + i]);
                    else
                        for (int i = 0; i < n; i++) line[s + i] = styled(p[a + i], im, row * im->w + x + a + i, st, dressing);
                } else {
                    for (int i = 0; i < len; i++) {
                        const int s = left - (x + i);
                        if (s < clip->x0 || s >= clip->x1) continue;
                        line[s] = plain ? mapped(p[i]) : styled(p[i], im, row * im->w + x + i, st, dressing);
                    }
                }
            }
            p += len;
            x += len;
        }
    }
    if (plain && im->edge) draw_edge(dst, stride, clip, im, top, left, mirror);
}
