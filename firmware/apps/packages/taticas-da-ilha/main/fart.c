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
    if (!ok || memcmp(a->data, "FART", 4) != 0 || rd16(a->data + 4) != 2) {
        fart_free(a);
        return false;
    }
    a->nimg = rd16(a->data + 6);
    a->nanim = rd16(a->data + 8);
    a->img = calloc(a->nimg, sizeof(fart_img_t));
    a->anim = calloc(a->nanim ? a->nanim : 1, sizeof(fart_anim_t));
    if (!a->img || !a->anim) {
        fart_free(a);
        return false;
    }
    const uint8_t *p = a->data + 12;
    for (int i = 0; i < a->nimg; i++, p += 44) {
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
                    if (n > 0 && plain) memcpy(line + s, p + a, n * 2);
                    else
                        for (int i = 0; i < n; i++) line[s + i] = styled(p[a + i], im, row * im->w + x + a + i, st, dressing);
                } else {
                    for (int i = 0; i < len; i++) {
                        const int s = left - (x + i);
                        if (s < clip->x0 || s >= clip->x1) continue;
                        line[s] = styled(p[i], im, row * im->w + x + i, st, dressing);
                    }
                }
            }
            p += len;
            x += len;
        }
    }
}
