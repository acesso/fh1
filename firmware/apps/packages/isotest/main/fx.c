#include "fx.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PROJ 8
#define PARTS 160

typedef struct {
    bool on, landed;
    int img;
    bool rot, spin;
    float x0, y0, x1, y1, arc;
    uint32_t t, len;
    uint16_t trail, trail2;          // sparks left behind (0: none); no image: a glowing ball of them
    uint32_t spark;
    uint8_t style;                   // FX_PLAIN, FX_NEEDLE, FX_TEMPEST
} proj_t;

typedef struct {
    float x, y, vx, vy, g;
    uint16_t c;
    uint16_t t, life;
    uint8_t size;
    bool ambient;                    // decoration: the battle does not wait for it
} part_t;

static proj_t s_proj[PROJ];
static part_t s_part[PARTS];
static int s_nparts, s_nbusy;     // particles, and those the battle waits for
static struct { bool on; float x, y; uint32_t t; int16_t zig[10]; } s_bolt;
#define SPIKES 4
static struct { bool on; float x, y; uint32_t t; } s_spike[SPIKES];
#define VORTICES 2
static struct { bool on; float x, y; uint32_t t, spawn; int16_t crack[2][6]; } s_vortex[VORTICES];

static float rnd(void) { return (rand() & 0xffff) / 65536.0f; }

void fx_reset(void)
{
    memset(s_proj, 0, sizeof(s_proj));
    s_nparts = s_nbusy = 0;
    s_bolt.on = false;
    memset(s_spike, 0, sizeof(s_spike));
    memset(s_vortex, 0, sizeof(s_vortex));
}

bool fx_busy(void)
{
    for (int i = 0; i < PROJ; i++)
        if (s_proj[i].on && !s_proj[i].landed) return true;
    for (int i = 0; i < SPIKES; i++)
        if (s_spike[i].on) return true;
    for (int i = 0; i < VORTICES; i++)
        if (s_vortex[i].on) return true;
    return s_bolt.on || s_nbusy > 0;
}

static void particle_of(float x, float y, float vx, float vy, float g, uint16_t c, int life, int size, bool ambient)
{
    if (s_nparts >= PARTS || (ambient && s_nparts >= PARTS * 3 / 4)) return;   // effects first, decoration after
    s_part[s_nparts++] = (part_t){ x, y, vx, vy, g, c, 0, (uint16_t)life, (uint8_t)size, ambient };
    s_nbusy += !ambient;
}
static void particle(float x, float y, float vx, float vy, float g, uint16_t c, int life, int size)
{
    particle_of(x, y, vx, vy, g, c, life, size, false);
}

int fx_shoot(int img, bool rot, bool spin, float x0, float y0, float x1, float y1, float arc, uint32_t ms)
{
    for (int i = 0; i < PROJ; i++)
        if (!s_proj[i].on || s_proj[i].landed) {
            s_proj[i] = (proj_t){ true, false, img, rot, spin, x0, y0, x1, y1, arc, 0, ms ? ms : 1, 0, 0, 0, FX_PLAIN };
            return i;
        }
    return -1;
}

bool fx_landed(int id) { return id < 0 || id >= PROJ || !s_proj[id].on || s_proj[id].landed; }

void fx_trail(int id, uint16_t c1, uint16_t c2)
{
    if (id < 0 || id >= PROJ) return;
    s_proj[id].trail = c1;
    s_proj[id].trail2 = c2;
}

static void proj_at(const proj_t *p, float t, float *x, float *y)
{
    *x = p->x0 + (p->x1 - p->x0) * t;
    *y = p->y0 + (p->y1 - p->y0) * t - p->arc * 4 * t * (1 - t);
}
static void proj_pos(const proj_t *p, float *x, float *y) { proj_at(p, (float)p->t / p->len, x, y); }

void fx_style(int id, int style)
{
    if (id >= 0 && id < PROJ) s_proj[id].style = (uint8_t)style;
}

void fx_spike(float x, float y)
{
    for (int i = 0; i < SPIKES; i++)
        if (!s_spike[i].on) {
            s_spike[i].on = true;
            s_spike[i].x = x;
            s_spike[i].y = y;
            s_spike[i].t = 0;
            break;
        }
    fx_burst(x, y, 0x8a85, 0xc58a, 20, 55);                     // earth thrown up
    fx_burst(x, y + 2, 0x52aa, 0xa514, 10, 25);
}

void fx_vortex(float x, float y)
{
    for (int i = 0; i < VORTICES; i++)
        if (!s_vortex[i].on) {
            s_vortex[i].on = true;
            s_vortex[i].x = x;
            s_vortex[i].y = y;
            s_vortex[i].t = s_vortex[i].spawn = 0;
            for (int k = 0; k < 2; k++)
                for (int j = 0; j < 6; j++) s_vortex[i].crack[k][j] = (int16_t)((rnd() - 0.5f) * 16);
            break;
        }
    fx_burst(x, y, 0xffff, 0xffe0, 14, 45);
}

void fx_aura(float x, float y, float r, uint16_t c1, uint16_t c2, int kind)
{
    const float a = rnd() * 6.2832f, d = r * (0.3f + rnd() * 0.7f);
    const uint16_t c = rnd() < 0.5f ? c1 : c2;
    const float px = x + cosf(a) * d, py = y + sinf(a) * d * 0.5f;
    switch (kind) {
    case FX_RISE: particle_of(px, py, (rnd() - 0.5f) * 6, -20 - rnd() * 20, 0, c, 500 + (int)(rnd() * 300), 1, true); break;
    case FX_DRIP: particle_of(px, py - r * 0.6f, 0, 6, 60, c, 500 + (int)(rnd() * 200), 1, true); break;
    case FX_DUST: particle_of(px, py + r * 0.3f, (rnd() - 0.5f) * 16, -6, 20, c, 400 + (int)(rnd() * 300), 1 + (rnd() < 0.3f), true); break;
    default: particle_of(px, py, -sinf(a) * 26, cosf(a) * 13 - 4, 0, c, 380 + (int)(rnd() * 200), 1, true); break;
    }
}

void fx_burst(float x, float y, uint16_t c1, uint16_t c2, int n, float speed)
{
    for (int i = 0; i < n; i++) {
        const float a = rnd() * 6.2832f, v = speed * (0.4f + rnd() * 0.8f);
        particle(x, y, cosf(a) * v, sinf(a) * v * 0.6f - speed * 0.5f, speed * 2.2f, (i & 1) ? c1 : c2,
                 300 + (int)(rnd() * 300), 1 + (rnd() < 0.35f));
    }
}

void fx_sparkle(float x, float y, uint16_t c, int n)
{
    for (int i = 0; i < n; i++)
        particle(x + (rnd() - 0.5f) * 22, y + (rnd() - 0.5f) * 8, (rnd() - 0.5f) * 6, -18 - rnd() * 30, 0, c,
                 450 + (int)(rnd() * 400), 1 + (rnd() < 0.3f));
}

void fx_glow(float x, float y, uint16_t c)
{
    const float a = rnd() * 6.2832f, r = 4 + rnd() * 6;
    particle(x + cosf(a) * r, y + sinf(a) * r, -cosf(a) * 8, -sinf(a) * 8 - 6, 0, c, 220 + (int)(rnd() * 160), 1);
}

void fx_bolt(float x, float y)
{
    s_bolt.on = true;
    s_bolt.x = x;
    s_bolt.y = y;
    s_bolt.t = 0;
    for (int i = 0; i < 10; i++) s_bolt.zig[i] = (int16_t)((rnd() - 0.5f) * 14);
    fx_burst(x, y, 0xffff, 0xffe0, 18, 40);
}

void fx_update(uint32_t dt)
{
    for (int i = 0; i < PROJ; i++) {
        proj_t *p = &s_proj[i];
        if (!p->on || p->landed) continue;
        p->t += dt;
        if (p->style == FX_TEMPEST) {                               // two spirals of wind wound round the path
            const float t = (float)p->t / p->len;
            float x, y, x2, y2;
            proj_at(p, t, &x, &y);
            proj_at(p, t + 0.02f > 1 ? 1 : t + 0.02f, &x2, &y2);
            float nx = -(y2 - y), ny = x2 - x;                    // across the path
            const float l = sqrtf(nx * nx + ny * ny);
            if (l > 0.01f) nx /= l, ny /= l;
            // Two strands, half a turn apart, 7 pixels out, a few sparks each per frame, plus a glint now and then.
            for (int j = 0; j < 3; j++) {
                const float a = (p->t - j * 5) * 0.05f;
                for (int k = 0; k < 2; k++) {
                    const float r = 7.0f * sinf(a + k * 3.1416f);
                    const uint16_t c = k ? 0xffff : (j ? 0xbf3f : 0xffe0);
                    particle_of(x + nx * r - (x2 - x) * j, y + ny * r - (y2 - y) * j, nx * r * 2, ny * r * 2, 0, c,
                                220 + (int)(rnd() * 120), 1 + (j == 0 && rnd() < 0.25f), true);
                }
            }
            if (rnd() < 0.3f) particle_of(x, y, (rnd() - 0.5f) * 30, (rnd() - 0.5f) * 30, 0, 0xffe0, 160, 2, true);
        }
        if (p->trail && (p->spark += dt) >= 24) {                 // a spark left behind every 24 ms
            p->spark = 0;
            float x, y;
            proj_pos(p, &x, &y);
            const int n = p->img < 0 ? 3 : 1;
            for (int k = 0; k < n; k++)
                particle_of(x + (rnd() - 0.5f) * 4, y + (rnd() - 0.5f) * 4, (rnd() - 0.5f) * 10, (rnd() - 0.5f) * 10, 0,
                            (k & 1) ? p->trail2 : p->trail, 200 + (int)(rnd() * 160), 1 + (p->img < 0 && k == 0), true);
        }
        if (p->t >= p->len) {
            p->t = p->len;
            p->landed = true;
        }
    }
    const float s = dt / 1000.0f;
    int n = 0, busy = 0;
    for (int i = 0; i < s_nparts; i++) {
        part_t q = s_part[i];
        q.t += dt;
        if (q.t >= q.life) continue;
        q.vy += q.g * s;
        q.x += q.vx * s;
        q.y += q.vy * s;
        busy += !q.ambient;
        s_part[n++] = q;
    }
    s_nparts = n;
    s_nbusy = busy;
    if (s_bolt.on) {
        s_bolt.t += dt;
        if (s_bolt.t > 320) s_bolt.on = false;
    }
    for (int i = 0; i < SPIKES; i++)
        if (s_spike[i].on && (s_spike[i].t += dt) > 620) s_spike[i].on = false;
    // A whirlwind: sparks thrown out along a funnel that widens as it rises, turning fast; 700 ms.
    for (int i = 0; i < VORTICES; i++) {
        if (!s_vortex[i].on) continue;
        s_vortex[i].t += dt;
        // It grows to full height in 250 ms, spins 600 ms, and pops at the top.
        const float grow = s_vortex[i].t < 250 ? s_vortex[i].t / 250.0f : 1.0f;
        for (s_vortex[i].spawn += dt; s_vortex[i].spawn >= 5; s_vortex[i].spawn -= 5) {
            const float h = rnd() * grow, a = s_vortex[i].t * 0.035f + rnd() * 6.2832f, r = 3 + h * 18;
            const float x = s_vortex[i].x + cosf(a) * r, y = s_vortex[i].y - h * 50 + sinf(a) * r * 0.35f;
            const float front = sinf(a);                                 // the near side brighter
            const uint16_t c = front > 0.3f ? 0xffff : rnd() < 0.35f ? 0xffe0 : front > -0.3f ? 0xbedf : 0x7d1b;
            particle_of(x, y, -sinf(a) * r * 7, -22 - h * 24, 0, c, 200 + (int)(rnd() * 160), 1 + (front > 0.6f), false);
        }
        if (s_vortex[i].t < 300 && (s_vortex[i].spawn & 1))              // dust thrown out round its foot
            particle_of(s_vortex[i].x + (rnd() - 0.5f) * 8, s_vortex[i].y, (rnd() - 0.5f) * 90, (rnd() - 0.5f) * 20, 30, 0xad55, 300, 1, false);
        if (s_vortex[i].t > 850) {
            fx_burst(s_vortex[i].x, s_vortex[i].y - 50, 0xffff, 0xffe0, 16, 60);   // the pop at the top
            s_vortex[i].on = false;
        }
    }
}

static void dot(uint16_t *px, int w, int h, int x, int y, uint16_t c, int size)
{
    for (int j = 0; j < size; j++)
        for (int i = 0; i < size; i++)
            if (x + i >= 0 && x + i < w && y + j >= 0 && y + j < h) px[(y + j) * w + x + i] = c;
}

static void line(uint16_t *px, int w, int h, int x0, int y0, int x1, int y1, uint16_t c, int size)
{
    const int dx = abs(x1 - x0), dy = abs(y1 - y0), n = dx > dy ? dx : dy;
    for (int k = 0; k <= n; k++) dot(px, w, h, x0 + (x1 - x0) * k / (n ? n : 1), y0 + (y1 - y0) * k / (n ? n : 1), c, size);
}

void fx_draw(const fart_t *art, uint16_t *px, int w, int h, int camx, int camy)
{
    const fart_clip_t clip = { 0, 0, w, h };
    for (int i = 0; i < PROJ; i++) {
        const proj_t *p = &s_proj[i];
        if (!p->on || p->landed) continue;
        if (p->style == FX_NEEDLE) {                 // an ice needle: a bright streak along its path
            const float t = (float)p->t / p->len;
            float x, y, bx, by;
            proj_at(p, t, &x, &y);
            proj_at(p, t > 0.06f ? t - 0.06f : 0, &bx, &by);
            line(px, w, h, (int)bx - camx, (int)by - camy, (int)x - camx, (int)y - camy, 0x5dff, 1);
            dot(px, w, h, (int)x - camx - 1, (int)y - camy - 1, 0xffff, 2);
            continue;
        }
        if (p->style == FX_TEMPEST && p->img >= 0 && p->img < art->nimg) {   // its after-images, fading behind it
            for (int k = 3; k >= 1; k--) {                        // white ghosts close behind, dark ones further
                const float t = (float)p->t / p->len - k * 0.06f;
                if (t <= 0) continue;
                float x, y, x2, y2;
                proj_at(p, t, &x, &y);
                proj_at(p, t + 0.01f, &x2, &y2);
                const int deg = (int)(atan2f(-(y2 - y), x2 - x) * 57.2958f);
                const int img = p->img + (((deg % 360 + 360) * 16 + 180) / 360) % 16;
                if (img < art->nimg)
                    fart_draw_tint(px, w, &clip, &art->img[img], (int)x - camx, (int)y - camy, false, k == 1 ? FART_WHITE : FART_DIM);
            }
            // Speed lines either side of its path.
            const float t = (float)p->t / p->len;
            float x, y, bx, by;
            proj_at(p, t, &x, &y);
            proj_at(p, t > 0.18f ? t - 0.18f : 0, &bx, &by);
            float nx = -(y - by), ny = x - bx;
            const float l = sqrtf(nx * nx + ny * ny);
            if (l > 0.5f) {
                nx = nx / l * 4;
                ny = ny / l * 4;
                line(px, w, h, (int)(bx + nx) - camx, (int)(by + ny) - camy, (int)(x + nx * 0.5f) - camx, (int)(y + ny * 0.5f) - camy, 0xdf1f, 1);
                line(px, w, h, (int)(bx - nx) - camx, (int)(by - ny) - camy, (int)(x - nx * 0.5f) - camx, (int)(y - ny * 0.5f) - camy, 0xdf1f, 1);
            }
        }
        if (p->img < 0) {                            // a ball of light: its trail's colours
            float x, y;
            proj_pos(p, &x, &y);
            dot(px, w, h, (int)x - camx - 2, (int)y - camy - 2, p->trail2 ? p->trail2 : 0xffff, 4);
            dot(px, w, h, (int)x - camx - 1, (int)y - camy - 1, 0xffff, 2);
            continue;
        }
        const float t = (float)p->t / p->len;
        const float x = p->x0 + (p->x1 - p->x0) * t, y = p->y0 + (p->y1 - p->y0) * t - p->arc * 4 * t * (1 - t);
        int img = p->img;
        if (p->rot) {                                // facing where it flies: the path's slope now
            const float vx = p->x1 - p->x0, vy = (p->y1 - p->y0) - p->arc * 4 * (1 - 2 * t);
            int deg = (int)(atan2f(-vy, vx) * 57.2958f);
            img = p->img + (((deg % 360 + 360) * 16 + 180) / 360) % 16;
        }
        if (img >= art->nimg) continue;
        const fart_img_t *im = &art->img[img];
        // A spinning item turns a quarter at a time (mirrored halves), enough to read as tumbling.
        const bool mirror = p->spin && ((p->t / 90) % 2);
        fart_draw(px, w, &clip, im, (int)x - camx - (p->rot ? 0 : im->w / 2 + im->ox), (int)y - camy - (p->rot ? 0 : im->h / 2 + im->oy),
                  mirror);
    }
    for (int i = 0; i < s_nparts; i++) {
        const part_t *q = &s_part[i];
        dot(px, w, h, (int)q->x - camx, (int)q->y - camy, q->c, q->size);
    }
    // Rock spikes: a tall one with two short ones, rising out of the ground, holding, sinking back.
    for (int i = 0; i < SPIKES; i++) {
        if (!s_spike[i].on) continue;
        const uint32_t t = s_spike[i].t;
        const float grow = t < 140 ? t / 140.0f : t < 440 ? 1.0f : 1.0f - (t - 440) / 180.0f;
        static const int8_t at[3][3] = { { 0, 24, 5 }, { -8, 13, 4 }, { 8, 15, 4 } };   // dx, height, half width
        for (int k = 0; k < 3; k++) {
            const int bx = (int)s_spike[i].x - camx + at[k][0], by = (int)s_spike[i].y - camy + (k ? 2 : 0);
            const int hgt = (int)(at[k][1] * grow), hw = at[k][2];
            for (int yy = 0; yy < hgt; yy++) {
                const int half = hw * (hgt - yy) / (hgt ? hgt : 1);
                for (int xx = -half; xx <= half; xx++) {
                    const int X = bx + xx, Y = by - yy;
                    if (X < 0 || Y < 0 || X >= w || Y >= h) continue;
                    // Lit on the left, dark on the right, an edge line.
                    px[Y * w + X] = xx == -half || xx == half ? 0x2104 : xx < 0 ? 0xbd31 : xx == 0 ? 0x9cae : 0x6b4a;
                }
            }
        }
    }
    // The whirlwind's crackle: two short jagged white lines inside it, flickering.
    for (int i = 0; i < VORTICES; i++) {
        if (!s_vortex[i].on || (s_vortex[i].t / 60) % 2) continue;
        for (int k = 0; k < 2; k++) {
            int x0 = (int)s_vortex[i].x - camx + s_vortex[i].crack[k][0], y0 = (int)s_vortex[i].y - camy - 4;
            for (int j = 1; j < 6; j++) {
                const int x1 = (int)s_vortex[i].x - camx + s_vortex[i].crack[k][j] * (j + 2) / 5, y1 = y0 - 9;
                line(px, w, h, x0, y0, x1, y1, j & 1 ? 0xffff : 0xffe0, 1);
                x0 = x1;
                y0 = y1;
            }
        }
    }
    if (s_bolt.on) {
        const int x = (int)s_bolt.x - camx, y = (int)s_bolt.y - camy, top = y - 150;
        int px0 = x, py0 = top;
        for (int k = 1; k <= 10; k++) {
            const int py1 = top + (y - top) * k / 10, px1 = x + (k < 10 ? s_bolt.zig[k - 1] : 0);
            const bool flick = (s_bolt.t / 50) % 2;
            line(px, w, h, px0, py0, px1, py1, flick ? 0xffff : 0xffe0, 2);
            px0 = px1;
            py0 = py1;
        }
    }
}
