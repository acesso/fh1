#include "viewturn.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

bool viewturn_begin(viewturn_t *v, const uint16_t *px, int w, int h, int dir, uint32_t ms)
{
    if (!v->from || v->w != w || v->h != h) {
        free(v->from);
        v->from = malloc((size_t)w * h * sizeof(uint16_t));   // large: the heap puts it in PSRAM
        if (!v->from) {
            v->on = false;
            return false;
        }
    }
    memcpy(v->from, px, (size_t)w * h * sizeof(uint16_t));
    v->w = w, v->h = h;
    v->dir = dir < 0 ? -1 : 1;
    v->t = 0;
    v->ms = ms ? ms : 1;
    v->on = true;
    return true;
}

void viewturn_update(viewturn_t *v, uint32_t dt)
{
    if (dt > 40) dt = 40;                                       // a slow frame (the new view's first) skips none of it
    if (v->on && (v->t += dt) >= v->ms) v->on = false;
}

void viewturn_free(viewturn_t *v)
{
    free(v->from);
    memset(v, 0, sizeof(*v));
}

static inline uint16_t shade(uint16_t c, int k)             // k of 256
{
    const int r = (c >> 11) * k >> 8, g = ((c >> 5) & 63) * k >> 8, b = (c & 31) * k >> 8;
    return (uint16_t)(r << 11 | g << 5 | b);
}

// Column x of a face from source column sx: squeezed toward the middle by `squeeze` (the face's far side smaller,
// as seen in perspective), darkened by k.
static void column(uint16_t *px, int w, int h, int x, const uint16_t *col, float squeeze, int k, uint16_t back)
{
    const float mid = h * 0.5f, s = 1.0f / (1.0f - squeeze);
    for (int y = 0; y < h; y++) {
        const int sy = (int)(mid + (y + 0.5f - mid) * s);
        px[y * w + x] = sy < 0 || sy >= h ? back : shade(col[sy], k);
    }
}

void viewturn_draw(const viewturn_t *v, uint16_t *px, int w, int h)
{
    if (!v->on || !v->from || v->w != w || v->h != h || h > 512) return;
    // Eased: the cube's edge between the faces at `edge` (from the right side to the left one, or back).
    float p = (float)v->t / v->ms;
    p = p * p * (3 - 2 * p);
    const int edge = (int)(v->dir > 0 ? w * (1 - p) : w * p);   // the old face left of it (dir +1), the new right
    const float bulge = 0.10f * sinf(p * 3.14159f);             // the edge nearest: the faces' far sides squeezed
    const uint16_t back = 0x0841;
    uint16_t col[512];
    // The new face, read from px itself: going away from where it grows, a column's source never yet overwritten.
    const int nw = v->dir > 0 ? w - edge : edge;                // its width on screen
    const int kn = 256 - (int)(110 * (1 - p));
    for (int i = 0; i < nw; i++) {
        const int x = v->dir > 0 ? w - 1 - i : i;               // from the far side in
        const int fx = v->dir > 0 ? x - edge : x;               // within the face
        const int sx = nw > 0 ? fx * w / nw : 0;
        const float far = v->dir > 0 ? (float)fx / nw : 1.0f - (float)fx / nw;
        for (int y = 0; y < h; y++) col[y] = px[y * w + sx];
        column(px, w, h, x, col, bulge * far, kn, back);
    }
    // The old face, from the frame kept.
    const int ow = w - nw, ko = 256 - (int)(110 * p);
    for (int i = 0; i < ow; i++) {
        const int x = v->dir > 0 ? i : edge + i;
        const int sx = i * w / ow;
        const float far = v->dir > 0 ? 1.0f - (float)i / ow : (float)i / ow;
        for (int y = 0; y < h; y++) col[y] = v->from[y * w + sx];
        column(px, w, h, x, col, bulge * far, ko, back);
    }
}
