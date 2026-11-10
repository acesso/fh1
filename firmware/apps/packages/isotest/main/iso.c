#include "iso.h"
#include <stdlib.h>
#include <string.h>

#define STEP_MS 360                  // one cell's walk

static inline uint16_t rgb565(uint32_t rgb)
{
    return (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
}

bool iso_init(iso_map_t *m, const fart_t *art, int cols, int rows)
{
    memset(m, 0, sizeof(*m));
    m->art = art;
    m->cols = cols;
    m->rows = rows;
    m->sky = rgb565(0x1a1c2c);
    m->cells = calloc(cols * rows, sizeof(iso_cell_t));
    m->mark = calloc(cols * rows, 1);
    if (!m->cells || !m->mark) return false;
    for (int i = 0; i < cols * rows; i++) m->cells[i] = (iso_cell_t){ 0, ISO_NONE, ISO_NONE, ISO_NONE };
    return true;
}

void iso_free(iso_map_t *m)
{
    free(m->cells);
    free(m->layer);
    free(m->mark);
    m->cells = NULL;
    m->layer = NULL;
    m->mark = NULL;
}

static void pos(int c, int r, int level, int *x, int *y)
{
    *x = (c - r) * ISO_HW;
    *y = (c + r) * ISO_HH - level * ISO_STEP;
}

void iso_world(const iso_map_t *m, int c, int r, int *x, int *y)
{
    pos(c, r, m->cells[r * m->cols + c].h, x, y);
}

static void draw_img(iso_map_t *m, uint16_t *dst, int stride, const fart_clip_t *clip, int idx, int x, int y)
{
    if (idx == ISO_NONE || idx >= m->art->nimg) return;
    fart_draw(dst, stride, clip, &m->art->img[idx], x, y, false);
}

// One cell's blocks, bottom up, and its object; (ox, oy) is the world position of dst's top-left.
static void draw_column(iso_map_t *m, uint16_t *dst, int stride, const fart_clip_t *clip, int c, int r, int ox, int oy)
{
    const iso_cell_t *cell = iso_cell(m, c, r);
    int x, y;
    for (int l = 0; l <= cell->h; l++) {
        pos(c, r, l, &x, &y);
        draw_img(m, dst, stride, clip, l == cell->h ? cell->top : cell->fill, x - ox, y - oy);
    }
    if (cell->obj != ISO_NONE) {
        pos(c, r, cell->h + 1, &x, &y);
        draw_img(m, dst, stride, clip, cell->obj, x - ox, y - oy);
    }
}

bool iso_build_layer(iso_map_t *m)
{
    int maxh = 0;
    for (int i = 0; i < m->cols * m->rows; i++)
        if (m->cells[i].h > maxh) maxh = m->cells[i].h;
    const int lx = -m->rows * ISO_HW, ly = -(maxh + 1) * ISO_STEP - 2 * ISO_HH;
    const int lw = (m->cols + m->rows) * ISO_HW, lh = (m->cols + m->rows) * ISO_HH + 2 * ISO_HH - ly;
    if (!m->layer || lw * lh > m->lw * m->lh) {
        free(m->layer);
        m->layer = malloc((size_t)lw * lh * 2);
        if (!m->layer) return false;
    }
    m->lx = lx;
    m->ly = ly;
    m->lw = lw;
    m->lh = lh;
    for (int i = 0; i < lw * lh; i++) m->layer[i] = m->sky;
    const fart_clip_t clip = { 0, 0, lw, lh };
    for (int r = 0; r < m->rows; r++)            // back to front: a cell is drawn after the ones behind it
        for (int c = 0; c < m->cols; c++) draw_column(m, m->layer, lw, &clip, c, r, lx, ly);
    return true;
}

iso_unit_t *iso_add_unit(iso_map_t *m, const fart_anim_t *walk, const fart_anim_t *idle, int c, int r, int dir)
{
    if (m->nunits >= ISO_UNITS) return NULL;
    iso_unit_t *u = &m->units[m->nunits++];
    *u = (iso_unit_t){ .walk = walk, .idle = idle, .c = c, .r = r, .fc = c, .fr = r, .dir = dir, .t = rand() % 1000,
                       .wpn = -1, .wpn_angle = ISO_POSE_REST };
    return u;
}

iso_unit_t *iso_unit_at(iso_map_t *m, int c, int r)
{
    for (int i = 0; i < m->nunits; i++)
        if (m->units[i].c == c && m->units[i].r == r) return &m->units[i];
    return NULL;
}

bool iso_step(iso_map_t *m, iso_unit_t *u, int dir)
{
    static const int8_t dc[FART_DIRS] = { 1, 0, -1, 0 }, dr[FART_DIRS] = { 0, 1, 0, -1 };
    u->dir = dir;
    if (u->step) return false;
    const int c = u->c + dc[dir], r = u->r + dr[dir];
    if (!iso_inside(m, c, r) || iso_unit_at(m, c, r)) return false;
    u->fc = u->c;
    u->fr = u->r;
    u->c = c;
    u->r = r;
    u->step = 1;
    if (m->on_step) m->on_step(m, u, iso_cell(m, u->fc, u->fr)->h, iso_cell(m, c, r)->h);
    return true;
}

static const int8_t s_dc[FART_DIRS] = { 1, 0, -1, 0 }, s_dr[FART_DIRS] = { 0, 1, 0, -1 };

void iso_walk(iso_unit_t *u, const uint8_t *dirs, int n)
{
    if (n > ISO_PATH) n = ISO_PATH;
    memcpy(u->path, dirs, n);
    u->npath = n;
    u->ipath = 0;
}

void iso_play(iso_unit_t *u, const fart_anim_t *a, bool hold)
{
    u->once = a;
    u->once_t = 0;
    u->hold = hold;
}

void iso_fall(iso_unit_t *u)
{
    u->falling = true;
    u->tilt = 0;
}

void iso_rise(iso_unit_t *u)
{
    u->falling = false;
    u->tilt = 0;
}

void iso_update(iso_map_t *m, uint32_t dt_ms)
{
    for (int i = 0; i < m->nunits; i++) {
        iso_unit_t *u = &m->units[i];
        u->t += dt_ms;
        if (u->still && u->fade < 230) {                       // a knocked-out unit's colour drains in ~0.7 s
            const int f = u->fade + (int)dt_ms * 230 / 700 + 1;
            u->fade = (uint8_t)(f > 230 ? 230 : f);
        } else if (!u->still) u->fade = 0;
        if (u->falling && u->tilt != 90 && u->tilt != -90) {
            // Backward, away from where it faces: speeding up as it goes, like a body tipping over.
            const int sign = (u->dir == FART_SE || u->dir == FART_NE) ? -1 : 1;
            int a = abs(u->tilt) + 4 + abs(u->tilt) * (int)dt_ms / 90;
            if (a > 90) a = 90;
            u->tilt = (int16_t)(sign * a);
        }
        u->flash = u->flash > dt_ms ? u->flash - dt_ms : 0;
        if (u->once) {
            u->once_t += dt_ms;
            const uint32_t len = (uint32_t)u->once->frames * u->once->ms;
            if (u->once_t >= len) {
                if (u->hold) u->once_t = len - 1;
                else u->once = NULL;
            }
        }
        if (u->step) {
            u->step += dt_ms;
            if (u->step >= STEP_MS) {
                u->step = 0;
                u->fc = u->c;
                u->fr = u->r;
            }
        }
        if (!u->step && u->ipath < u->npath) {
            const int d = u->path[u->ipath++];
            if (!iso_step(m, u, d)) u->ipath = u->npath;        // blocked: stop there
        }
    }
}

void iso_reach(iso_map_t *m, const iso_unit_t *u, int range, int jump, uint8_t *dist, uint8_t *from)
{
    const int n = m->cols * m->rows;
    memset(dist, 255, n);
    memset(from, 255, n);
    static int queue[ISO_PATH * 64];                  // not on the stack: frame() runs on an 8 KB one
    int head = 0, tail = 0;
    dist[u->r * m->cols + u->c] = 0;
    queue[tail++] = u->r * m->cols + u->c;
    while (head < tail) {
        const int i = queue[head++], c = i % m->cols, r = i / m->cols;
        if (dist[i] >= range) continue;
        for (int d = 0; d < FART_DIRS; d++) {
            const int nc = c + s_dc[d], nr = r + s_dr[d], j = nr * m->cols + nc;
            if (!iso_inside(m, nc, nr) || dist[j] != 255) continue;
            const iso_cell_t *a = &m->cells[i], *b = &m->cells[j];
            if (b->h == 0 || abs((int)b->h - (int)a->h) > jump) continue;
            const iso_unit_t *o = iso_unit_at(m, nc, nr);
            if (o && o != u) continue;
            dist[j] = dist[i] + 1;
            from[j] = d;
            if (tail < (int)(sizeof(queue) / sizeof(queue[0]))) queue[tail++] = j;
        }
    }
}

int iso_path_to(iso_map_t *m, const iso_unit_t *u, const uint8_t *dist, const uint8_t *from, int c, int r, uint8_t *out)
{
    if (!iso_inside(m, c, r)) return 0;
    int i = r * m->cols + c;
    const int n = dist[i];
    if (n == 255 || n == 0 || n > ISO_PATH) return 0;
    for (int k = n - 1; k >= 0; k--) {
        const int d = from[i];
        out[k] = d;
        c -= s_dc[d];
        r -= s_dr[d];
        i = r * m->cols + c;
    }
    return n;
}

// Where a unit's feet are now: between its two cells while stepping, with a hop when the height changes.
static void unit_world(iso_map_t *m, const iso_unit_t *u, int *x, int *y)
{
    int x1, y1;
    iso_world(m, u->c, u->r, &x1, &y1);
    if (!u->step) {
        *x = x1;
        *y = y1;
        return;
    }
    int x0, y0;
    iso_world(m, u->fc, u->fr, &x0, &y0);
    const int p = (int)u->step * 256 / STEP_MS;               // 0..255
    *x = x0 + (x1 - x0) * p / 256;
    *y = y0 + (y1 - y0) * p / 256;
    const int dh = iso_cell(m, u->c, u->r)->h - iso_cell(m, u->fc, u->fr)->h;
    const int hop = (dh ? 6 + abs(dh) * ISO_STEP : 2) * 4 * p * (256 - p) / 65536;
    *y -= hop;
}

void iso_feet(iso_map_t *m, const iso_unit_t *u, int *x, int *y)
{
    unit_world(m, u, x, y);
    *x += u->lunge_x;
    *y += u->lunge_y;
}

// The frame a unit shows now.
static int unit_frame(const iso_unit_t *u, bool *mirror)
{
    int idx = u->once ? fart_frame(u->once, u->dir, u->once_t, mirror)
                      : fart_frame(u->step ? u->walk : u->idle, u->dir, u->still ? 0 : u->t, mirror);
    if (idx < 0 && !u->step) idx = fart_frame(u->walk, u->dir, 0, mirror);    // no idle: the walk's first frame
    return idx;
}

void iso_hand(iso_map_t *m, const iso_unit_t *u, int *x, int *y)
{
    iso_feet(m, u, x, y);
    bool mirror;
    const int idx = unit_frame(u, &mirror);
    if (idx >= 0 && m->art->img[idx].hx != FART_NO_HAND) {
        *x += mirror ? -m->art->img[idx].hx : m->art->img[idx].hx;
        *y += m->art->img[idx].hy;
    } else *y -= 16;
    *x += u->hand_dx;
    *y += u->hand_dy;
}

static void draw_weapon(iso_map_t *m, uint16_t *px, int w, const fart_clip_t *clip, const iso_unit_t *u, int tint)
{
    if (u->wpn < 0) return;
    int x, y;
    iso_hand(m, u, &x, &y);
    const bool right = u->dir == FART_SE || u->dir == FART_NE;
    const int deg = u->wpn_angle != ISO_POSE_REST ? u->wpn_angle : right ? u->wpn_rest : 180 - u->wpn_rest;
    const fart_style_t st = { .tint = tint, .shade = u->wpn_shade };
    fart_draw_style(px, w, clip, &m->art->img[iso_rot(u->wpn, deg)], x - m->camx, y - m->camy, false, &st);
}

void iso_follow(iso_map_t *m, int wx, int wy, int w, int h, uint32_t dt_ms)
{
    const int tx = wx - w / 2, ty = wy - h / 2;
    int k = (int)dt_ms * 256 / 160;                             // reaches the target in ~160 ms steps
    if (k > 256) k = 256;
    const int dx = (tx - m->camx) * k / 256, dy = (ty - m->camy) * k / 256;
    m->camx += dx ? dx : (tx > m->camx) - (tx < m->camx);
    m->camy += dy ? dy : (ty > m->camy) - (ty < m->camy);
}

// One pixel, only when it is on the screen: every write outside the buffer corrupts the memory next to it.
static inline void put(uint16_t *px, int w, int h, int x, int y, uint16_t c)
{
    if (x >= 0 && y >= 0 && x < w && y < h) px[y * w + x] = c;
}

static void draw_cursor(uint16_t *px, int w, int h, int x, int y, uint16_t col)
{
    for (int i = 0; i <= ISO_HW; i++) {
        const int dy = i / 2;
        const int pts[4][2] = { { x - ISO_HW + i, y - dy }, { x + ISO_HW - i, y - dy }, { x - ISO_HW + i, y + dy },
                                { x + ISO_HW - i, y + dy } };
        for (int k = 0; k < 4; k++) {
            const int X = pts[k][0], Y = pts[k][1];
            if (X >= 0 && X < w && Y >= 0 && Y < h) px[Y * w + X] = col;
        }
    }
}

void iso_draw(iso_map_t *m, uint16_t *px, int w, int h, int cc, int cr, uint32_t now_ms)
{
    // The static layer under the camera.
    for (int y = 0; y < h; y++) {
        uint16_t *line = px + y * w;
        const int ly = m->camy + y - m->ly;
        if (ly < 0 || ly >= m->lh) {
            for (int x = 0; x < w; x++) line[x] = m->sky;
            continue;
        }
        const int lx = m->camx - m->lx;
        int x = 0;
        for (; x < w && lx + x < 0; x++) line[x] = m->sky;
        int n = w - x;
        if (lx + x + n > m->lw) n = m->lw - (lx + x);
        if (n > 0) {
            memcpy(line + x, m->layer + ly * m->lw + lx + x, n * 2);
            x += n;
        }
        for (; x < w; x++) line[x] = m->sky;
    }
    const fart_clip_t screen = { 0, 0, w, h };

    // Marked cells: their top face tinted (blue: walk, red: act on, orange: the area hit).
    static const uint16_t tints[4] = { 0, 0x231f, 0xf8a3, 0xfd20 };
    for (int r = 0; r < m->rows; r++)
        for (int c = 0; c < m->cols; c++) {
            const int k = m->mark[r * m->cols + c];
            if (!k) continue;
            int x, y;
            iso_world(m, c, r, &x, &y);
            x -= m->camx;
            y -= m->camy;
            const uint16_t t = (tints[k] >> 1) & 0x7BEF;
            for (int dy = -ISO_HH + 1; dy < ISO_HH; dy++) {
                const int yy = y + dy, half = ISO_HW - 2 * abs(dy) - 1;
                if (yy < 0 || yy >= h || half <= 0) continue;
                for (int xx = x - half; xx <= x + half; xx++)
                    if (xx >= 0 && xx < w) px[yy * w + xx] = ((px[yy * w + xx] >> 1) & 0x7BEF) + t;
            }
        }

    if (cc >= 0 && iso_inside(m, cc, cr)) {
        int x, y;
        iso_world(m, cc, cr, &x, &y);
        const int pulse = (now_ms / 120) % 6;
        draw_cursor(px, w, h, x - m->camx, y - m->camy, pulse < 3 ? rgb565(0xfff3a0) : rgb565(0xffffff));
    }

    // Units, back to front; while stepping, a unit counts as being on the nearer of its two cells.
    int order[ISO_UNITS];
    for (int i = 0; i < m->nunits; i++) order[i] = i;
    for (int i = 1; i < m->nunits; i++)
        for (int j = i; j > 0; j--) {
            const iso_unit_t *a = &m->units[order[j - 1]], *b = &m->units[order[j]];
            const int ka = (a->c + a->r > a->fc + a->fr ? a->c + a->r : a->fc + a->fr) * 256 + a->c;
            const int kb = (b->c + b->r > b->fc + b->fr ? b->c + b->r : b->fc + b->fr) * 256 + b->c;
            if (ka <= kb) break;
            const int t = order[j - 1];
            order[j - 1] = order[j];
            order[j] = t;
        }
    for (int k = 0; k < m->nunits; k++) {
        const iso_unit_t *u = &m->units[order[k]];
        bool mirror;
        if (u->hidden) continue;
        const int idx = unit_frame(u, &mirror);
        if (idx < 0) continue;
        int x, y;
        iso_feet(m, u, &x, &y);
        const fart_img_t *im = &m->art->img[idx];
        // A knocked-out unit fades instead of dimming; a hit flashes white.
        const int tint = u->flash && (u->flash / 60) % 2 ? FART_WHITE : u->dim && !u->still ? FART_DIM : FART_PLAIN;
        // Facing away, the weapon is behind the body; facing the viewer, in front of it.
        const bool away = u->dir == FART_NE || u->dir == FART_NW;
        const bool armed = !u->falling;                       // a fallen hero has let go of the weapon
        if (away && armed) draw_weapon(m, px, w, &screen, u, tint);
        const fart_style_t st = { .tint = tint, .cover = u->cover, .ramp = u->ramp, .shade = u->shade, .zoom = u->zoom,
                                  .tilt = u->tilt, .fade = u->fade };
        fart_draw_style(px, w, &screen, im, x - m->camx, y - m->camy, mirror, &st);
        if (!away && armed) draw_weapon(m, px, w, &screen, u, tint);
        if (u->counter && !u->dim) {                          // ready to strike back: a small sword sign
            const int cx = x - m->camx + 8, cy = y - m->camy - 44 - (int)((now_ms / 200) % 2);
            for (int k = 0; k < 6; k++) put(px, w, h, cx + k, cy + 5 - k, 0xffe0);   // the blade
            put(px, w, h, cx + 1, cy + 3, 0xffe0);                                 // the guard
            put(px, w, h, cx + 3, cy + 5, 0xffe0);
        }
        // The scenery in front of it that is taller than where it stands, drawn again over it (only there).
        // (Its box: twice as big for a boss; a square round its feet while it falls or lies.)
        const int z = u->zoom > 1 ? u->zoom : 1;
        int sx = x - m->camx + (mirror ? -im->ox - im->w : im->ox) * z, sy = y - m->camy + im->oy * z;
        int bw = im->w * z, bh = im->h * z;
        if (u->tilt) {
            const int r = (im->w > im->h ? im->w : im->h) * z;
            sx = x - m->camx - r;
            sy = y - m->camy - r;
            bw = bh = 2 * r;
        }
        fart_clip_t clip = { sx < 0 ? 0 : sx, sy < 0 ? 0 : sy, sx + bw > w ? w : sx + bw, sy + bh > h ? h : sy + bh };
        if (clip.x0 >= clip.x1 || clip.y0 >= clip.y1) continue;
        const int bc = u->c > u->fc ? u->c : u->fc, br = u->r > u->fr ? u->r : u->fr;
        const int level = iso_cell(m, u->c, u->r)->h > iso_cell(m, u->fc, u->fr)->h ? iso_cell(m, u->c, u->r)->h
                                                                                    : iso_cell(m, u->fc, u->fr)->h;
        for (int r = br; r < br + 3 && r < m->rows; r++)
            for (int c = bc; c < bc + 3 && c < m->cols; c++) {
                if ((c == u->c && r == u->r) || (c == u->fc && r == u->fr)) continue;
                const iso_cell_t *cell = iso_cell(m, c, r);
                if (cell->h > level || (cell->obj != ISO_NONE && cell->h + 1 > level))
                    draw_column(m, px, w, &clip, c, r, m->camx, m->camy);
            }
    }
}
