#include "portal.h"
#include <math.h>
#include <string.h>

static uint32_t rnd(portal_t *p)
{
    p->seed = p->seed * 1103515245u + 12345u;
    return p->seed >> 8;
}

static int lum565(uint16_t c) { return ((c >> 11) << 3) * 3 + (((c >> 5) & 63) << 2) * 6 + ((c & 31) << 3); }

bool portal_load(portal_t *p, const fart_t *a, const char *anim)
{
    memset(p, 0, sizeof(*p));
    p->seed = 0x5eed;
    p->open_ms = 420;
    p->glow = -1;
    const fart_anim_t *an = fart_anim(a, anim);
    if (!an) return false;
    for (int i = 0; i < an->frames && p->n < PORTAL_FRAMES; i++)
        if (an->idx[i] != FART_NONE) p->frame[p->n++] = an->idx[i];
    p->ms = an->ms ? an->ms : 80;
    if (!p->n) return false;
    // Its reference colours: every frame's, dark to light.
    enum { M = 96 };
    static uint16_t px[M * M];
    static uint8_t mask[M * M];
    for (int f = 0; f < p->n; f++) {
        const fart_img_t *im = &a->img[p->frame[f]];
        if (!fart_unpack(im, px, mask, M)) return false;
        for (int i = 0; i < im->w * im->h; i++) {
            if (!mask[i]) continue;
            bool seen = false;
            for (int k = 0; k < p->nkey && !seen; k++) seen = p->key[k] == px[i];
            if (!seen && p->nkey < PORTAL_KEYS) p->key[p->nkey++] = px[i];
        }
    }
    for (int i = 1; i < p->nkey; i++)
        for (int j = i; j > 0 && lum565(p->key[j - 1]) > lum565(p->key[j]); j--) {
            const uint16_t t = p->key[j];
            p->key[j] = p->key[j - 1];
            p->key[j - 1] = t;
        }
    memcpy(p->col, p->key, sizeof(p->col));
    return true;
}

void portal_glow(portal_t *p, int img) { p->glow = img; }

void portal_colour(portal_t *p, uint32_t rgb)
{
    // Each shade by its place among them (darkest 0, lightest 1): the colour scaled from a fifth of it up to itself,
    // then toward white for the lightest (the rim's glow).
    const float tr = (rgb >> 16) & 255, tg = (rgb >> 8) & 255, tb = rgb & 255;
    const int lo = lum565(p->key[0]), hi = lum565(p->key[p->nkey - 1]);
    for (int k = 0; k < p->nkey; k++) {
        const float v = hi > lo ? (float)(lum565(p->key[k]) - lo) / (hi - lo) : 1;
        float r, g, b;
        if (v <= 0.8f) {
            const float f = 0.2f + v;
            r = tr * f, g = tg * f, b = tb * f;
        } else {
            const float f = (v - 0.8f) / 0.2f * 0.45f;
            r = tr + (255 - tr) * f, g = tg + (255 - tg) * f, b = tb + (255 - tb) * f;
        }
        r = r > 255 ? 255 : r, g = g > 255 ? 255 : g, b = b > 255 ? 255 : b;
        p->col[k] = (uint16_t)(((int)r >> 3) << 11 | ((int)g >> 2) << 5 | ((int)b >> 3));
    }
}

void portal_open(portal_t *p)
{
    if (p->state == PORTAL_OPEN || p->state == PORTAL_OPENING) return;
    p->st_t = p->state == PORTAL_CLOSING ? p->open_ms - p->st_t : 0;   // from where closing had got to
    p->state = PORTAL_OPENING;
}

void portal_close(portal_t *p)
{
    if (p->state == PORTAL_CLOSED || p->state == PORTAL_CLOSING) return;
    p->st_t = p->state == PORTAL_OPENING ? p->open_ms - p->st_t : 0;
    p->state = PORTAL_CLOSING;
}

bool portal_shown(const portal_t *p) { return p->state != PORTAL_CLOSED; }

// How open it is: 0..256, eased (quick at first, settling).
static int openness(const portal_t *p)
{
    if (p->state == PORTAL_OPEN) return 256;
    if (p->state == PORTAL_CLOSED || !p->open_ms) return 0;
    float t = (float)p->st_t / p->open_ms;
    if (t > 1) t = 1;
    if (p->state == PORTAL_CLOSING) t = 1 - t;
    const float e = 1 - (1 - t) * (1 - t) * (1 - t);
    return (int)(256 * e);
}

void portal_update(portal_t *p, uint32_t dt)
{
    p->t += dt;
    if (p->state == PORTAL_OPENING || p->state == PORTAL_CLOSING) {
        p->st_t += dt;
        if (p->st_t >= p->open_ms) p->state = p->state == PORTAL_OPENING ? PORTAL_OPEN : PORTAL_CLOSED;
    }
    // Motes: a few, slow, drifting out and up from round the vortex, while it is open (none born while closing).
    for (int i = 0; i < PORTAL_MOTES; i++) {
        portal_mote_t *m = &p->mote[i];
        if (m->life) {
            m->x += m->vx * dt / 1000.0f;
            m->y += m->vy * dt / 1000.0f;
            if ((m->age += (uint16_t)dt) >= m->life) m->life = 0;
        } else if (p->state == PORTAL_OPEN && rnd(p) % 1000 < dt) {       // now and then
            const float a = (rnd(p) % 628) / 100.0f, d = 1.05f + (rnd(p) % 60) / 100.0f;   // not too close
            m->x = cosf(a) * 9 * d;
            m->y = sinf(a) * 15 * d;
            m->vx = m->x * 0.25f + ((int)(rnd(p) % 7) - 3);
            m->vy = -3.0f - (rnd(p) % 5);
            m->age = 0;
            m->life = (uint16_t)(1600 + rnd(p) % 1600);
            m->shade = (uint8_t)(rnd(p) % 3);
        }
    }
}

void portal_draw(const portal_t *p, const fart_t *a, uint16_t *dst, int stride, const fart_clip_t *clip, int x, int y,
                 bool mirror)
{
    if (!p->n) return;
    const int z = openness(p);
    const fart_img_t *im = &a->img[p->frame[p->t / p->ms % (uint32_t)p->n]];
    if (z > 0) {
        fart_map(p->key, p->col, p->nkey);
        fart_draw_zoom(dst, stride, clip, im, x, y, mirror, z);
        if (p->glow >= 0 && z > 128) fart_draw(dst, stride, clip, &a->img[p->glow], x, y, mirror);   // its light round it
        fart_map(NULL, NULL, 0);
    }
    // The motes, round the vortex's middle: a pixel each, its colour dimming as it ends.
    const int mx = mirror ? x - im->ox - im->w / 2 : x + im->ox + im->w / 2, my = y + im->oy + im->h / 2;
    for (int i = 0; i < PORTAL_MOTES; i++) {
        const portal_mote_t *m = &p->mote[i];
        if (!m->life) continue;
        const int left = m->life - m->age;
        const int shade = p->nkey - 1 - m->shade - (left < 400 ? 2 : left < 800 ? 1 : 0);
        const uint16_t c = p->col[shade < 0 ? 0 : shade];
        const int px = mx + (int)(mirror ? -m->x : m->x), py = my + (int)m->y;
        if (px >= clip->x0 && px < clip->x1 && py >= clip->y0 && py < clip->y1) dst[py * stride + px] = c;
    }
}
