#include "iso.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define STEP_MS 360                  // one cell's walk

// Blocks of up to 4 KB come from internal RAM (ESP-IDF: size <= CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL, 4096), which
// Wi-Fi needs; the engine's own small tables ask for a little more, so they come from PSRAM.
#define PS_MIN 4160
static void *ps_calloc(size_t n) { return calloc(n < PS_MIN ? PS_MIN : n, 1); }
static void *ps_malloc(size_t n) { return malloc(n < PS_MIN ? PS_MIN : n); }

static inline uint16_t rgb565(uint32_t rgb)
{
    return (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
}

bool iso_init(iso_map_t *m, fart_t *art, int cols, int rows)
{
    memset(m, 0, sizeof(*m));
    m->art = art;
    m->cols = cols;
    m->rows = rows;
    m->sky = rgb565(0x1a1c2c);
    m->cells = ps_calloc(cols * rows * sizeof(iso_cell_t));
    m->mark = ps_calloc(cols * rows);
    m->chg_of = ps_calloc(cols * rows);
    if (!m->cells || !m->mark || !m->chg_of) return false;
    for (int i = 0; i < cols * rows; i++) m->cells[i] = (iso_cell_t){ 0, ISO_NONE, ISO_NONE, ISO_NONE, 0, 0 };
    return true;
}

void iso_free(iso_map_t *m)
{
    if (m->cells) iso_grade(m, NULL, NULL);                       // the shared art back to its own colours
    free(m->cells);
    free(m->layer);
    free(m->mark);
    free(m->tanim_of);
    free(m->acells);
    free(m->aframe);
    free(m->chg_of);
    free(m->bcells);
    free(m->scratch);
    m->chg_of = NULL;
    m->bcells = NULL;
    m->scratch = NULL;
    m->nbcells = 0;
    m->cells = NULL;
    m->layer = NULL;
    m->mark = NULL;
    m->tanim_of = NULL;
    m->acells = NULL;
    m->aframe = NULL;
    m->ntanim = m->nacells = 0;
}

// ── The view: map cells to the view's cells and back ──

static inline int view_cols(const iso_map_t *m) { return m->view & 1 ? m->rows : m->cols; }
static inline int view_rows(const iso_map_t *m) { return m->view & 1 ? m->cols : m->rows; }

static inline void to_view(const iso_map_t *m, int c, int r, int *vc, int *vr)
{
    switch (m->view) {
    case 1: *vc = m->rows - 1 - r; *vr = c; break;
    case 2: *vc = m->cols - 1 - c; *vr = m->rows - 1 - r; break;
    case 3: *vc = r; *vr = m->cols - 1 - c; break;
    default: *vc = c; *vr = r; break;
    }
}

static inline void from_view(const iso_map_t *m, int vc, int vr, int *c, int *r)
{
    switch (m->view) {
    case 1: *c = vr; *r = m->rows - 1 - vc; break;
    case 2: *c = m->cols - 1 - vc; *r = m->rows - 1 - vr; break;
    case 3: *c = m->cols - 1 - vr; *r = vc; break;
    default: *c = vc; *r = vr; break;
    }
}

// The world position of a map cell's top face at a level, in the view shown.
static void pos(const iso_map_t *m, int c, int r, int level, int *x, int *y)
{
    int vc, vr;
    to_view(m, c, r, &vc, &vr);
    *x = (vc - vr) * ISO_HW;
    *y = (vc + vr) * ISO_HH - level * ISO_STEP;
}

void iso_world(const iso_map_t *m, int c, int r, int *x, int *y)
{
    pos(m, c, r, m->cells[r * m->cols + c].h, x, y);
}

// ── Scenery animations ──

static inline uint32_t cell_hash(int c, int r)
{
    uint32_t h = (uint32_t)(c * 73856093) ^ (uint32_t)(r * 19349663);
    h ^= h >> 13;
    return h * 0x5bd1e995u;
}

// The frame a cell shows now of an animated image (0..frames-1).
static int tframe(const iso_map_t *m, const iso_tanim_t *t, int c, int r)
{
    const uint32_t ms = t->anim->ms ? t->anim->ms : 1, loop = ms * t->anim->frames;
    uint32_t off = 0;
    if (t->mode == ISO_TA_SCATTER) off = cell_hash(c, r) % loop;
    else if (t->mode == ISO_TA_WAVE) off = loop * 64 - (uint32_t)(c + r) * ms / 2 % loop;
    return (int)(((m->now + off) / ms) % t->anim->frames);
}

// The image a cell shows for one of its parts (top, fill, object), its animation's frame if it has one.
static int scenery(const iso_map_t *m, int idx, int c, int r)
{
    if (idx == ISO_NONE || !m->tanim_of || idx >= m->art->nimg || !m->tanim_of[idx]) return idx;
    const iso_tanim_t *t = &m->tanim[m->tanim_of[idx] - 1];
    const int i = t->anim->idx[tframe(m, t, c, r)];
    return i == FART_NONE ? idx : i;
}

bool iso_tile_anim(iso_map_t *m, int base, const fart_anim_t *a, int mode)
{
    if (base < 0 || base >= m->art->nimg || !a || !a->frames || m->ntanim >= ISO_TANIMS) return false;
    if (!m->tanim_of && !(m->tanim_of = ps_calloc(m->art->nimg))) return false;
    m->tanim[m->ntanim] = (iso_tanim_t){ (uint16_t)base, (uint8_t)mode, a };
    m->tanim_of[base] = (uint8_t)++m->ntanim;
    return true;
}

static void grade_one(fart_t *art, uint8_t *done, int idx, uint16_t (*fn)(uint16_t, void *), void *user)
{
    if (idx == ISO_NONE || idx < 0 || idx >= art->nimg || done[idx]) return;
    done[idx] = 1;
    fart_recolor(art, idx, fn, user);
}

void iso_grade(iso_map_t *m, uint16_t (*fn)(uint16_t v, void *user), void *user)
{
    fart_t *art = m->art;
    m->grade_fn = fn;
    m->grade_user = user;
    if (!fn) {                                                    // every image back, whoever recoloured it
        if (art->orig)
            for (int i = 0; i < art->nimg; i++)
                if (art->orig[i]) fart_recolor(art, i, NULL, NULL);
        return;
    }
    uint8_t *done = ps_calloc(art->nimg);
    if (!done) return;
    for (int i = 0; i < m->cols * m->rows; i++) {
        grade_one(art, done, m->cells[i].top, fn, user);
        grade_one(art, done, m->cells[i].fill, fn, user);
        grade_one(art, done, m->cells[i].obj, fn, user);
    }
    for (int k = 0; k < m->ntanim; k++)
        for (int f = 0; f < m->tanim[k].anim->frames; f++) grade_one(art, done, m->tanim[k].anim->idx[f], fn, user);
    free(done);
}

// ── The static layer ──

// What the layer shows of a cell: its state, or, while it changes, the part that stays put.
static const iso_cell_t *shown_cell(const iso_map_t *m, int c, int r)
{
    const int i = r * m->cols + c;
    return m->chg_of && m->chg_of[i] ? &m->chg[m->chg_of[i] - 1].shown : &m->cells[i];
}

static void draw_img(const iso_map_t *m, uint16_t *dst, int stride, const fart_clip_t *clip, int idx, int x, int y,
                     bool mirror)
{
    if (idx == ISO_NONE || idx >= m->art->nimg) return;
    fart_draw(dst, stride, clip, &m->art->img[idx], x, y, mirror);
}

// One cell's blocks, bottom up, and its object; (ox, oy) is the world position of dst's top-left. Objects turn
// with the view by mirroring (a log lying along the columns lies along the rows a quarter turn later); blocks keep
// their light from the same side on screen.
static void draw_column_at(const iso_map_t *m, uint16_t *dst, int stride, const fart_clip_t *clip, int c, int r,
                           int ox, int oy, const iso_cell_t *cell)
{
    int x, y;
    for (int l = cell->base; l <= cell->h; l++) {
        pos(m, c, r, l, &x, &y);
        draw_img(m, dst, stride, clip, scenery(m, l == cell->h ? cell->top : cell->fill, c, r), x - ox, y - oy, false);
    }
    if (cell->obj != ISO_NONE) {
        pos(m, c, r, cell->h + 1, &x, &y);
        draw_img(m, dst, stride, clip, scenery(m, cell->obj, c, r), x - ox, y - oy, m->view & 1);
    }
}

// A column as the layer has it: a bobbing one is not there (it is drawn each frame, where it is then).
static void draw_column(const iso_map_t *m, uint16_t *dst, int stride, const fart_clip_t *clip, int c, int r, int ox,
                        int oy)
{
    const iso_cell_t *cell = shown_cell(m, c, r);
    if (!cell->bob) draw_column_at(m, dst, stride, clip, c, r, ox, oy, cell);
}

// How far a bobbing cell is from its place now (pixels, down positive): each group in its own phase.
static int bob_offset(const iso_map_t *m, int c, int r)
{
    const iso_cell_t *cell = &m->cells[r * m->cols + c];
    if (!cell->bob) return 0;
    return (int)floorf(0.5f + 2.6f * sinf(6.2831853f * ((float)(m->now % 1800) / 1800.0f + cell->bob * 0.29f)));
}

// How far the scenery's images reach from their anchors (to know which columns a redraw meets).
static void reach_of(iso_map_t *m, int idx)
{
    if (idx == ISO_NONE || idx >= m->art->nimg) return;
    const fart_img_t *im = &m->art->img[idx];
    const int l = -im->ox > im->ox + im->w ? -im->ox : im->ox + im->w;    // either side: objects mirror
    if (l > m->col_l) m->col_l = (int16_t)l;
    if (-im->oy > m->col_up) m->col_up = (int16_t)-im->oy;
    if (im->oy + im->h > m->col_dn) m->col_dn = (int16_t)(im->oy + im->h);
}

// The frames a cell's layer shows of its animated parts (0xff: not animated).
static void cell_frames(const iso_map_t *m, int c, int r, uint8_t f[3])
{
    const iso_cell_t *cell = &m->cells[r * m->cols + c];
    const uint16_t parts[3] = { cell->top, cell->fill, cell->obj };
    for (int k = 0; k < 3; k++) {
        f[k] = 0xff;
        if (parts[k] != ISO_NONE && m->tanim_of && parts[k] < m->art->nimg && m->tanim_of[parts[k]])
            f[k] = (uint8_t)tframe(m, &m->tanim[m->tanim_of[parts[k]] - 1], c, r);
    }
}

// The animated cells, and the frames the layer is about to show of them.
static bool collect_anim(iso_map_t *m)
{
    free(m->bcells);
    m->bcells = NULL;
    m->nbcells = 0;
    int nb = 0;
    for (int i = 0; i < m->cols * m->rows; i++) nb += m->cells[i].bob != 0;
    if (nb > ISO_BOBS) nb = ISO_BOBS;
    if (nb) {
        if (!(m->bcells = ps_malloc(nb * sizeof(uint16_t)))) return false;
        for (int i = 0; i < m->cols * m->rows && m->nbcells < nb; i++)
            if (m->cells[i].bob) m->bcells[m->nbcells++] = (uint16_t)i;
    }
    free(m->acells);
    free(m->aframe);
    m->acells = NULL;
    m->aframe = NULL;
    m->nacells = 0;
    if (m->ntanim) {
        int n = 0;
        uint8_t f[3];
        for (int r = 0; r < m->rows; r++)
            for (int c = 0; c < m->cols; c++) {
                cell_frames(m, c, r, f);
                n += f[0] != 0xff || f[1] != 0xff || f[2] != 0xff;
            }
        m->acells = n ? ps_malloc(n * sizeof(uint16_t)) : NULL;
        m->aframe = n ? ps_malloc(n * 3) : NULL;
        if (n && (!m->acells || !m->aframe)) return false;
        for (int r = 0; r < m->rows; r++)
            for (int c = 0; c < m->cols; c++) {
                cell_frames(m, c, r, f);
                if (f[0] == 0xff && f[1] == 0xff && f[2] == 0xff) continue;
                m->acells[m->nacells] = (uint16_t)(r * m->cols + c);
                memcpy(m->aframe + m->nacells * 3, f, 3);
                m->nacells++;
            }
    }
    return true;
}

bool iso_build_layer(iso_map_t *m)
{
    int maxh = 0;
    m->col_l = ISO_HW;
    m->col_up = m->col_dn = ISO_HH;
    for (int i = 0; i < m->cols * m->rows; i++) {
        if (m->cells[i].h > maxh) maxh = m->cells[i].h;
        reach_of(m, m->cells[i].top);
        reach_of(m, m->cells[i].fill);
        reach_of(m, m->cells[i].obj);
    }
    m->maxh = maxh;
    const int vcols = view_cols(m), vrows = view_rows(m);
    const int lx = -vrows * ISO_HW, ly = -(maxh + 1) * ISO_STEP - 2 * ISO_HH;
    const int lw = (vcols + vrows) * ISO_HW, lh = (vcols + vrows) * ISO_HH + 2 * ISO_HH - ly;
    if (!m->layer || lw * lh > m->lw * m->lh) {
        free(m->layer);
        m->layer = malloc((size_t)lw * lh * 2);
        if (!m->layer) return false;
    }
    m->lx = lx;
    m->ly = ly;
    m->lw = lw;
    m->lh = lh;
    if (!collect_anim(m)) return false;
    for (int i = 0; i < lw * lh; i++) m->layer[i] = m->sky;
    const fart_clip_t clip = { 0, 0, lw, lh };
    for (int vr = 0; vr < vrows; vr++)               // back to front: a cell is drawn after the ones behind it
        for (int vc = 0; vc < vcols; vc++) {
            int c, r;
            from_view(m, vc, vr, &c, &r);
            draw_column(m, m->layer, lw, &clip, c, r, lx, ly);
        }
    return true;
}

// Draws the layer again inside a box (world pixels): the sky, then every column that reaches into it, back to front.
static void redraw_box(iso_map_t *m, int x0, int y0, int x1, int y1)
{
    x0 -= m->lx, x1 -= m->lx, y0 -= m->ly, y1 -= m->ly;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > m->lw) x1 = m->lw;
    if (y1 > m->lh) y1 = m->lh;
    if (x0 >= x1 || y0 >= y1) return;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) m->layer[y * m->lw + x] = m->sky;
    const fart_clip_t clip = { x0, y0, x1, y1 };
    const int vcols = view_cols(m), vrows = view_rows(m);
    for (int vr = 0; vr < vrows; vr++)
        for (int vc = 0; vc < vcols; vc++) {
            const int cx = (vc - vr) * ISO_HW - m->lx, by = (vc + vr) * ISO_HH - m->ly;
            if (cx + m->col_l <= x0 || cx - m->col_l >= x1 || by + m->col_dn <= y0) continue;
            int c, r;
            from_view(m, vc, vr, &c, &r);
            const iso_cell_t *cell = &m->cells[r * m->cols + c];
            if (by - (cell->h + 1) * ISO_STEP - m->col_up >= y1) continue;
            draw_column(m, m->layer, m->lw, &clip, c, r, m->lx, m->ly);
        }
}

// The animated cells on screen whose frame changed: drawn again into the layer (the rest wait until they show).
static void tick_scenery(iso_map_t *m, int w, int h)
{
    for (int k = 0; k < m->nacells; k++) {
        const int c = m->acells[k] % m->cols, r = m->acells[k] / m->cols;
        int x, y;
        pos(m, c, r, 0, &x, &y);
        const int top = y - (m->cells[m->acells[k]].h + 1) * ISO_STEP - m->col_up;
        if (x + m->col_l < m->camx || x - m->col_l >= m->camx + w || top >= m->camy + h || y + m->col_dn < m->camy)
            continue;
        uint8_t f[3];
        cell_frames(m, c, r, f);
        if (!memcmp(f, m->aframe + k * 3, 3)) continue;
        memcpy(m->aframe + k * 3, f, 3);
        redraw_box(m, x - m->col_l, top, x + m->col_l, y + m->col_dn);
    }
}

int iso_check_layer(iso_map_t *m, int w, int h, int box[4])
{
    uint16_t *kept = m->layer, *fresh = malloc((size_t)m->lw * m->lh * 2);
    if (!fresh) return -1;
    const int lw = m->lw, lh = m->lh, lx = m->lx, ly = m->ly;
    m->layer = fresh;
    const bool built = iso_build_layer(m) && m->lw == lw && m->lh == lh && m->lx == lx && m->ly == ly;
    m->layer = kept;
    int n = 0;
    box[0] = box[1] = 1 << 30;
    box[2] = box[3] = -1;
    for (int y = 0; built && y < h; y++) {
        const int yy = m->camy + y - ly;
        if (yy < 0 || yy >= lh) continue;
        for (int x = 0; x < w; x++) {
            const int xx = m->camx + x - lx;
            if (xx < 0 || xx >= lw || kept[yy * lw + xx] == fresh[yy * lw + xx]) continue;
            n++;
            if (x < box[0]) box[0] = x;
            if (y < box[1]) box[1] = y;
            if (x > box[2]) box[2] = x;
            if (y > box[3]) box[3] = y;
        }
    }
    free(fresh);
    return built ? n : -1;
}

void iso_redraw_animated(iso_map_t *m)
{
    for (int k = 0; k < m->nacells; k++) {
        const int c = m->acells[k] % m->cols, r = m->acells[k] / m->cols;
        int x, y;
        pos(m, c, r, 0, &x, &y);
        const int top = y - (m->cells[m->acells[k]].h + 1) * ISO_STEP - m->col_up;
        redraw_box(m, x - m->col_l, top, x + m->col_l, y + m->col_dn);
    }
}

// ── Scenery changes ──

#define PIECE_MS 700                 // pieces flying
#define PIECE_G 700                  // their gravity, pixels/s²
#define DROP_FROM 64                 // a drop starts this many pixels up
#define DROP_G 1400
#define BOUNCE_MS 120
#define DUST_MS 380
#define LEVEL_MS 170                 // rising or sinking, a level

static inline bool same_cell(const iso_cell_t *a, const iso_cell_t *b)
{
    return a->h == b->h && a->top == b->top && a->fill == b->fill && a->obj == b->obj && a->base == b->base && a->bob == b->bob;
}

static inline float ease(float p) { return p <= 0 ? 0 : p >= 1 ? 1 : p * p * (3 - 2 * p); }

// When a drop lands (ms).
static uint32_t drop_land_ms(void) { return (uint32_t)(sqrtf(2.0f * DROP_FROM / DROP_G) * 1000); }

// The layer drawn again round a cell's column, up to level top (and the object over it).
static void refresh_column(iso_map_t *m, int c, int r, int top)
{
    int x, y;
    pos(m, c, r, 0, &x, &y);
    redraw_box(m, x - m->col_l, y - (top + 1) * ISO_STEP - m->col_up, x + m->col_l, y + m->col_dn);
}

// A new image in the scenery: the grade on it, and room for it in redraws.
static void bring_in(iso_map_t *m, int idx)
{
    if (idx == ISO_NONE || idx < 0 || idx >= m->art->nimg) return;
    reach_of(m, idx);
    if (m->grade_fn && !(m->art->orig && m->art->orig[idx])) fart_recolor(m->art, idx, m->grade_fn, m->grade_user);
}

// Cuts an image (its anchor at world ax, ay) into pieces that fly out from its middle.
static void add_pieces(iso_map_t *m, iso_change_t *ch, int idx, int ax, int ay, int size)
{
    if (idx == ISO_NONE || idx < 0 || idx >= m->art->nimg) return;
    const fart_img_t *im = &m->art->img[idx];
    const int mx = im->ox + im->w / 2, my = im->oy + im->h / 2;
    for (int y = im->oy; y < im->oy + im->h; y += size)
        for (int x = im->ox; x < im->ox + im->w; x += size) {
            if (ch->npieces >= ISO_PIECES) return;
            iso_piece_t *p = &ch->pieces[ch->npieces++];
            const uint32_t hs = cell_hash(ch->c * 64 + x, ch->r * 64 + y + ch->npieces * 7);
            p->img = (int16_t)idx;
            p->x0 = (int8_t)x;
            p->y0 = (int8_t)y;
            p->w = (int8_t)(x + size > im->ox + im->w ? im->ox + im->w - x : size);
            p->h = (int8_t)(y + size > im->oy + im->h ? im->oy + im->h - y : size);
            p->ax = (int16_t)ax;
            p->ay = (int16_t)ay;
            p->vx = (int16_t)((x + p->w / 2 - mx) * 5 + (int)(hs % 41) - 20);
            p->vy = (int16_t)(-80 - (int)((hs >> 8) % 70) + (y + p->h / 2 - my) * 2);   // the top flies highest
        }
}

// Pieces of a cell's images going: levels lo..hi of a column (the top block's image at level top_at) and an
// object, sized so they fit in ISO_PIECES.
static void shatter(iso_map_t *m, iso_change_t *ch, const iso_cell_t *cell, int lo, int hi, bool obj)
{
    int n = (hi >= lo ? hi - lo + 1 : 0) + (obj && cell->obj != ISO_NONE);
    const int size = n <= 2 ? 8 : n <= 4 ? 12 : 16;
    int x, y;
    for (int l = lo; l <= hi; l++) {
        pos(m, ch->c, ch->r, l, &x, &y);
        add_pieces(m, ch, l == cell->h ? cell->top : cell->fill, x, y, size);
    }
    if (obj && cell->obj != ISO_NONE) {
        pos(m, ch->c, ch->r, cell->h + 1, &x, &y);
        add_pieces(m, ch, cell->obj, x, y, size);
    }
}

#define FADE_MS 520
#define PORT_MS 760
#define ISO_SCRATCH 96

// An image a fade or a teleport draws (its anchor at a level of the cell).
static void add_part(iso_map_t *m, iso_change_t *ch, int img, int level, bool coming, bool obj)
{
    if (img == ISO_NONE || img < 0 || img >= m->art->nimg || ch->nparts >= ISO_PARTS) return;
    int x, y;
    pos(m, ch->c, ch->r, level, &x, &y);
    ch->parts[ch->nparts++] = (iso_part_t){ (int16_t)img, (int16_t)x, (int16_t)y, (uint8_t)(obj && (m->view & 1)),
                                            (uint8_t)coming };
}

// A column's blocks from level lo up, and its object.
static void column_parts(iso_map_t *m, iso_change_t *ch, const iso_cell_t *cell, int lo, bool coming)
{
    for (int l = lo > cell->base ? lo : cell->base; l <= cell->h; l++)
        add_part(m, ch, l == cell->h ? cell->top : cell->fill, l, coming, false);
    add_part(m, ch, cell->obj, cell->h + 1, coming, true);
}

static bool change(iso_map_t *m, int c, int r, int what, int fx, const iso_cell_t *to)
{
    if (!iso_inside(m, c, r) || !m->chg_of || iso_changing(m, c, r) || m->turn_ms) return false;
    const int i = r * m->cols + c;
    const iso_cell_t from = m->cells[i];
    if (same_cell(&from, to)) return false;
    int k = 0;
    while (k < ISO_CHANGES && m->chg[k].len) k++;
    if (k == ISO_CHANGES) return false;
    iso_change_t *ch = &m->chg[k];
    memset(ch, 0, sizeof(*ch));
    *ch = (iso_change_t){ .c = (int16_t)c, .r = (int16_t)r, .fx = (uint8_t)fx, .what = (uint8_t)what, .from = from,
                          .to = *to, .shown = *to, .len = PIECE_MS };
    m->cells[i] = *to;
    bring_in(m, to->top);
    bring_in(m, to->fill);
    bring_in(m, to->obj);
    const int top = from.h > to->h ? from.h : to->h;
    const uint32_t drop = drop_land_ms() + BOUNCE_MS + DUST_MS;
    if (fx == ISO_FX_FADE || fx == ISO_FX_TELEPORT) {             // what goes and what comes, drawn dissolving
        ch->len = fx == ISO_FX_FADE ? FADE_MS : PORT_MS;
        if (what == 0) {
            ch->shown.obj = ISO_NONE;
            add_part(m, ch, from.obj, from.h + 1, false, true);
            add_part(m, ch, to->obj, to->h + 1, true, true);
        } else if (what == 1) {
            const int base = from.h < to->h ? from.h : to->h;
            ch->shown = (iso_cell_t){ (uint8_t)(base ? base - 1 : 0), base ? to->fill : ISO_NONE, to->fill, ISO_NONE,
                                      to->base, 0 };
            column_parts(m, ch, &from, base, false);
            column_parts(m, ch, to, base, true);
        } else {
            ch->shown.top = ISO_NONE;
            add_part(m, ch, from.top, from.h, false, false);
            add_part(m, ch, to->top, to->h, true, false);
        }
    } else if (what == 0) {                                       // the object
        const bool going = from.obj != ISO_NONE, coming = to->obj != ISO_NONE;
        if (fx == ISO_FX_BREAK || fx == ISO_FX_DROP) {
            if (going) shatter(m, ch, &from, 1, 0, true);
            if (fx == ISO_FX_DROP && coming) ch->shown.obj = ISO_NONE, ch->len = (uint16_t)drop;
        } else if (fx == ISO_FX_RISE) {
            ch->len = 2 * LEVEL_MS + DUST_MS / 2;
            if (coming) ch->shown.obj = ISO_NONE;
        }
    } else if (what == 1) {                                       // the height
        const int base = from.h < to->h ? from.h : to->h;
        if (fx == ISO_FX_RISE) {                                  // the column moves whole: the layer keeps below it
            ch->shown = (iso_cell_t){ (uint8_t)(base ? base - 1 : 0), base ? to->fill : ISO_NONE, to->fill, ISO_NONE,
                                      to->base, 0 };
            ch->len = (uint16_t)(abs(to->h - from.h) * LEVEL_MS + DUST_MS / 2);
        } else if (fx == ISO_FX_DROP && to->h > from.h) {         // new blocks fall onto the old top
            ch->shown = from;
            ch->shown.obj = ISO_NONE;
            ch->len = (uint16_t)drop;
        } else if (fx != ISO_FX_NONE && to->h < from.h) {         // the blocks above the new top shatter
            shatter(m, ch, &from, to->h + 1, from.h, false);
        } else fx = ch->fx = ISO_FX_NONE;
    } else {                                                      // the top block's image
        if (fx != ISO_FX_NONE) {
            ch->fx = ISO_FX_BREAK;
            shatter(m, ch, &from, from.h, from.h, false);
        }
    }
    if (ch->fx == ISO_FX_BREAK && !ch->npieces && what != 1) ch->len = 0;      // nothing went: nothing to show
    if (ch->fx == ISO_FX_NONE) ch->len = 0;
    if (to->h > m->maxh) iso_build_layer(m);                      // no room in the layer: build it again
    if (ch->len) m->chg_of[i] = (uint8_t)(k + 1);
    if (to->h <= m->maxh) refresh_column(m, c, r, top + 1);
    if (!ch->len) collect_anim(m);
    if (m->on_change) m->on_change(m, c, r, ch->fx, ch->len ? ISO_EV_START : ISO_EV_DONE);
    return true;
}

bool iso_set_obj(iso_map_t *m, int c, int r, int img, int fx)
{
    if (!iso_inside(m, c, r)) return false;
    iso_cell_t to = m->cells[r * m->cols + c];
    to.obj = (uint16_t)img;
    return change(m, c, r, 0, fx, &to);
}

bool iso_set_height(iso_map_t *m, int c, int r, int h, int fx)
{
    if (!iso_inside(m, c, r) || h < 0 || h > 30) return false;
    iso_cell_t to = m->cells[r * m->cols + c];
    to.h = (uint8_t)h;
    return change(m, c, r, 1, fx, &to);
}

bool iso_set_float(iso_map_t *m, int c, int r, int base, int bob)
{
    if (!iso_inside(m, c, r) || iso_changing(m, c, r) || base < 0 || bob < 0) return false;
    iso_cell_t *cell = &m->cells[r * m->cols + c];
    cell->base = (uint8_t)(base > cell->h ? cell->h : base);
    cell->bob = (uint8_t)bob;
    collect_anim(m);
    refresh_column(m, c, r, cell->h + 1);
    return true;
}

// A slot for a change or an effect: false while turning, or when all ISO_CHANGES run.
static iso_change_t *free_slot(iso_map_t *m)
{
    if (m->turn_ms) return NULL;
    for (int k = 0; k < ISO_CHANGES; k++)
        if (!m->chg[k].len) return &m->chg[k];
    return NULL;
}

bool iso_crack(iso_map_t *m, int c, int r, int power, int delay_ms)
{
    iso_change_t *ch = iso_inside(m, c, r) ? free_slot(m) : NULL;
    if (!ch) return false;
    const iso_cell_t *cell = &m->cells[r * m->cols + c];
    memset(ch, 0, sizeof(*ch));
    *ch = (iso_change_t){ .c = (int16_t)c, .r = (int16_t)r, .fx = ISO_FX_BREAK, .what = 3, .from = *cell, .to = *cell,
                          .shown = *cell, .start = (uint16_t)delay_ms, .pieces_at = (uint16_t)delay_ms,
                          .len = (uint16_t)(delay_ms + PIECE_MS) };
    int x, y;
    pos(m, c, r, cell->h, &x, &y);
    add_pieces(m, ch, cell->top, x, y, 6);
    if (power < 1) power = 1;
    int n = 0;
    for (int i = 0; i < ch->npieces; i++) {                       // the tile stays: what flies is its top face's
        iso_piece_t p = ch->pieces[i];
        if (p.y0 + p.h <= -ISO_HH || p.y0 >= ISO_HH) continue;
        p.vx = (int16_t)(p.vx * (1 + power) / 3);
        p.vy = (int16_t)(p.vy * (2 + power) / 4);
        ch->pieces[n++] = p;
    }
    ch->npieces = (uint8_t)n;
    return true;
}

bool iso_drop_on(iso_map_t *m, int c, int r, int img, int delay_ms)
{
    iso_change_t *ch = iso_inside(m, c, r) && img >= 0 && img < m->art->nimg ? free_slot(m) : NULL;
    if (!ch) return false;
    const iso_cell_t *cell = &m->cells[r * m->cols + c];
    bring_in(m, img);
    memset(ch, 0, sizeof(*ch));
    const uint32_t land = delay_ms + drop_land_ms();
    *ch = (iso_change_t){ .c = (int16_t)c, .r = (int16_t)r, .fx = ISO_FX_DROP, .what = 4, .from = *cell, .to = *cell,
                          .shown = *cell, .start = (uint16_t)delay_ms, .pieces_at = (uint16_t)land, .img = (int16_t)img,
                          .len = (uint16_t)(land + PIECE_MS) };
    int x, y;
    pos(m, c, r, cell->h + 1, &x, &y);
    add_pieces(m, ch, img, x, y, 6);
    return true;
}

bool iso_set_top(iso_map_t *m, int c, int r, int img, int fx)
{
    if (!iso_inside(m, c, r)) return false;
    iso_cell_t to = m->cells[r * m->cols + c];
    to.top = (uint16_t)img;
    return change(m, c, r, 2, fx, &to);
}

// The changes' clocks: a drop's landing, and the end (the layer takes the new state).
static void changes_step(iso_map_t *m, uint32_t dt_ms)
{
    for (int k = 0; k < ISO_CHANGES; k++) {
        iso_change_t *ch = &m->chg[k];
        if (!ch->len) continue;
        const uint32_t t = ch->t + dt_ms;
        const bool dropping = ch->fx == ISO_FX_DROP && (ch->what == 0 ? ch->to.obj != ISO_NONE : ch->to.h > ch->from.h);
        const uint32_t lands = ch->what == 3 ? ch->start : ch->what == 4 ? ch->pieces_at : dropping ? drop_land_ms() : 0;
        if ((dropping || ch->what >= 3) && !ch->landed && t >= lands) {
            ch->landed = true;
            if (m->on_change) m->on_change(m, ch->c, ch->r, ch->fx, ISO_EV_LAND);
        }
        if (t < ch->len) {
            ch->t = (uint16_t)t;
            continue;
        }
        ch->len = 0;
        if (ch->what >= 3) {                                      // looks only: nothing to put back
            if (m->on_change) m->on_change(m, ch->c, ch->r, ch->fx, ISO_EV_DONE);
            continue;
        }
        const int top = ch->from.h > ch->to.h ? ch->from.h : ch->to.h;
        m->chg_of[ch->r * m->cols + ch->c] = 0;
        refresh_column(m, ch->c, ch->r, top + 1);
        collect_anim(m);
        if (m->on_change) m->on_change(m, ch->c, ch->r, ch->fx, ISO_EV_DONE);
    }
}

// How far below its final place a rising (or above, sinking) column's top is now, in pixels.
static int change_lift(const iso_map_t *m, int c, int r)
{
    if (!iso_changing(m, c, r)) return 0;
    const iso_change_t *ch = &m->chg[m->chg_of[r * m->cols + c] - 1];
    if (ch->what != 1 || ch->fx != ISO_FX_RISE) return 0;
    const int span = abs(ch->to.h - ch->from.h) * LEVEL_MS;
    const float e = ease(span ? (float)ch->t / span : 1);
    return (int)((ch->to.h - ch->from.h) * ISO_STEP * (1 - e));
}

bool iso_set_view(iso_map_t *m, int view)
{
    const uint8_t old = m->view;
    m->view = (uint8_t)(view & 3);
    if (m->view == old) return true;
    if (iso_build_layer(m)) return true;
    m->view = old;
    return false;
}

void iso_turn(iso_map_t *m, int by, int c, int r)
{
    if (m->turn_ms || !by || !iso_inside(m, c, r)) return;
    for (int k = 0; k < ISO_CHANGES; k++)
        if (m->chg[k].len) return;                                // the scenery changing: turn once it is done
    m->turn_ms = 1;
    m->turn_len = m->turn_style == ISO_TURN_NARROW ? ISO_TURN_MS : ISO_SPIN_MS;
    m->turn_by = (int8_t)(by > 0 ? 1 : -1);
    m->turned = false;
    m->turn_c = (int16_t)c;
    m->turn_r = (int16_t)r;
}

// Half way through a turn: the new view, the camera moved so the kept cell stays where it was on screen.
static void turn_step(iso_map_t *m, uint32_t dt_ms)
{
    if (!m->turn_ms) return;
    const uint32_t t = m->turn_ms + dt_ms;
    if (!m->turned && t >= m->turn_len / 2u) {
        int x0, y0, x1, y1;
        iso_world(m, m->turn_c, m->turn_r, &x0, &y0);
        m->turned = true;
        if (iso_set_view(m, m->view + 4 + m->turn_by)) {
            iso_world(m, m->turn_c, m->turn_r, &x1, &y1);
            m->camx += x1 - x0;
            m->camy += y1 - y0;
        }
    }
    m->turn_ms = t >= m->turn_len ? 0 : (uint16_t)t;
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
    for (int i = 0; i < m->nunits; i++)                          // a hidden unit is not on the field
        if (m->units[i].c == c && m->units[i].r == r && !m->units[i].hidden) return &m->units[i];
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
    turn_step(m, dt_ms);
    changes_step(m, dt_ms);
    for (int i = 0; i < m->nunits; i++) {
        iso_unit_t *u = &m->units[i];
        u->t += dt_ms;
        if (u->still && u->fade < 230) {                       // a knocked-out unit's colour drains in ~0.7 s
            const int f = u->fade + (int)dt_ms * 230 / 700 + 1;
            u->fade = (uint8_t)(f > 230 ? 230 : f);
        } else if (!u->still) u->fade = 0;
        if (u->falling && u->tilt != 90 && u->tilt != -90) {
            // Backward, away from where it faces: speeding up as it goes, like a body tipping over.
            const int sd = iso_screen_dir(m, u->dir), sign = (sd == FART_SE || sd == FART_NE) ? -1 : 1;
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
        *y = y1 + change_lift(m, u->c, u->r) + bob_offset(m, u->c, u->r);   // riding a moving column
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

// The frame a unit shows now (facing as it does on screen).
static int unit_frame(const iso_map_t *m, const iso_unit_t *u, bool *mirror)
{
    const int d = iso_screen_dir(m, u->dir);
    int idx = u->once ? fart_frame(u->once, d, u->once_t, mirror)
                      : fart_frame(u->step ? u->walk : u->idle, d, u->still ? 0 : u->t, mirror);
    if (idx < 0 && !u->step) idx = fart_frame(u->walk, d, 0, mirror);          // no idle: the walk's first frame
    return idx;
}

void iso_hand(iso_map_t *m, const iso_unit_t *u, int *x, int *y)
{
    iso_feet(m, u, x, y);
    bool mirror;
    const int idx = unit_frame(m, u, &mirror);
    if (idx >= 0 && m->art->img[idx].hx != FART_NO_HAND) {
        *x += mirror ? -m->art->img[idx].hx : m->art->img[idx].hx;
        *y += m->art->img[idx].hy;
    } else *y -= 16;
    *x += u->hand_dx;
    *y += u->hand_dy;
}

static void draw_weapon(iso_map_t *m, uint16_t *px, int w, const fart_clip_t *clip, const iso_unit_t *u, int tint,
                        int dx, int dy)
{
    if (u->wpn < 0) return;
    int x, y;
    iso_hand(m, u, &x, &y);
    x += dx;
    y += dy;
    const int sd = iso_screen_dir(m, u->dir);
    const bool right = sd == FART_SE || sd == FART_NE;
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

// ── Drawing scenery changes ──

// An image over a floor: nothing below the V of a block's lower edges (vertex at vx, vy, rising 1 in 2 sideways).
static void draw_on_floor(const iso_map_t *m, uint16_t *px, int w, int h, int idx, int x, int y, bool mirror, int vx,
                          int vy)
{
    if (idx == ISO_NONE || idx < 0 || idx >= m->art->nimg) return;
    const fart_img_t *im = &m->art->img[idx];
    fart_clip_t c = { 0, 0, w, vy - 8 < h ? vy - 8 : h };
    if (c.y1 > 0) fart_draw(px, w, &c, im, x, y, mirror);
    for (int yy = vy - 8 < 0 ? 0 : vy - 8; yy < vy && yy < h; yy++) {
        const int half = (vy - yy) * 2;
        c = (fart_clip_t){ vx - half < 0 ? 0 : vx - half, yy, vx + half > w ? w : vx + half, yy + 1 };
        if (c.x0 < c.x1) fart_draw(px, w, &c, im, x, y, mirror);
    }
}

// A colour of an image (its middle pixel): dust the colour of the ground.
static uint16_t img_colour(const iso_map_t *m, int idx)
{
    if (idx == ISO_NONE || idx < 0 || idx >= m->art->nimg) return 0x8410;
    const fart_img_t *im = &m->art->img[idx];
    const uint16_t *p = im->spans;
    for (int row = 0; row < im->h; row++) {
        int runs = *p++;
        while (runs--) {
            const int len = p[1];
            if (row >= im->h / 3 && len) return p[2 + len / 2];
            p += 2 + len;
        }
    }
    return 0x8410;
}

// Dust puffing out round a point (screen), age ms into its life.
static void draw_dust(uint16_t *px, int w, int h, int x, int y, uint32_t age, uint32_t life, uint16_t col, int seed)
{
    if (age >= life) return;
    const uint16_t c = age > life / 2 ? (uint16_t)((col >> 1) & 0x7BEF) : col;
    for (int i = 0; i < 10; i++) {
        const uint32_t hs = cell_hash(seed + i, i * 3 + 1);
        const int side = i & 1 ? -1 : 1, spread = 6 + (int)(hs % 11);
        const int dx = side * (spread + (int)(age * (12 + hs % 9) / life));
        const int dy = (int)((hs >> 4) % 7) - 3 - (int)(age * (6 + (hs >> 8) % 8) / life);
        const int X = x + dx, Y = y + dy + abs(dx) / 3;
        for (int k = 0; k < 4; k++) {
            const int xx = X + (k & 1), yy = Y + (k >> 1);
            if (xx >= 0 && yy >= 0 && xx < w && yy < h) px[yy * w + xx] = c;
        }
    }
}

// How far above its place a dropping thing is now (pixels, negative: up), with its bounce.
static int drop_offset(uint32_t t)
{
    const uint32_t tl = drop_land_ms();
    if (t < tl) {
        const float s = t / 1000.0f;
        return (int)(-DROP_FROM + DROP_G * s * s / 2);
    }
    if (t < tl + BOUNCE_MS) return (int)(-3 * sinf(3.1415926f * (t - tl) / BOUNCE_MS));
    return 0;
}

static const uint8_t s_bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

// An image in blocks of colour (each block its middle pixel's), only where the dither at level 0..16 lets it: a
// teleport's. Unpacked into the map's scratch (ISO_SCRATCH a side at most).
static void draw_mosaic(iso_map_t *m, uint16_t *px, int w, int h, int idx, int ax, int ay, bool mirror, int block,
                        int level)
{
    if (idx == ISO_NONE || idx < 0 || idx >= m->art->nimg || level <= 0) return;
    if (!m->scratch && !(m->scratch = ps_malloc(ISO_SCRATCH * ISO_SCRATCH * 3))) return;
    const fart_img_t *im = &m->art->img[idx];
    uint16_t *pix = m->scratch;
    uint8_t *mask = (uint8_t *)(m->scratch + ISO_SCRATCH * ISO_SCRATCH);
    if (!fart_unpack(im, pix, mask, ISO_SCRATCH)) return;
    if (block < 1) block = 1;
    for (int by = 0; by < im->h; by += block)
        for (int bx = 0; bx < im->w; bx += block) {
            const int sx = bx + block / 2 < im->w ? bx + block / 2 : im->w - 1;
            const int sy = by + block / 2 < im->h ? by + block / 2 : im->h - 1;
            if (!mask[sy * im->w + sx]) continue;
            const uint16_t col = pix[sy * im->w + sx];
            for (int yy = by; yy < by + block && yy < im->h; yy++)
                for (int xx = bx; xx < bx + block && xx < im->w; xx++) {
                    const int X = mirror ? ax - im->ox - 1 - xx : ax + im->ox + xx, Y = ay + im->oy + yy;
                    if (X < 0 || Y < 0 || X >= w || Y >= h || s_bayer[Y & 3][X & 3] >= level) continue;
                    px[Y * w + X] = col;
                }
        }
}

// What moves of a change (a column rising or sinking, an object rising, sinking or dropping, what dissolves or
// teleports, a dropped image falling), in depth order.
static void draw_change(iso_map_t *m, uint16_t *px, int w, int h, const iso_change_t *ch)
{
    const int ox = -m->camx, oy = -m->camy;
    const bool odd = m->view & 1;
    int x, y, vx, vy;
    if (ch->t < ch->start) return;
    if (ch->what == 4) {                                          // falling until it lands, then only its pieces
        const uint32_t t = ch->t - ch->start;
        if (t >= drop_land_ms() || ch->img < 0) return;
        const fart_clip_t screen = { 0, 0, w, h };
        pos(m, ch->c, ch->r, ch->to.h + 1, &x, &y);
        fart_draw(px, w, &screen, &m->art->img[ch->img], x + ox, y + drop_offset(t) + oy, false);
        return;
    }
    if (ch->fx == ISO_FX_FADE || ch->fx == ISO_FX_TELEPORT) {
        const float p = (float)ch->t / ch->len;
        for (int i = 0; i < ch->nparts; i++) {
            const iso_part_t *pt = &ch->parts[i];
            float lv;
            if (ch->fx == ISO_FX_FADE) lv = pt->coming ? p : 1 - p;
            else lv = pt->coming ? (p - 0.45f) / 0.55f : 1 - p / 0.55f;  // a teleport: out, then in
            if (lv <= 0) continue;
            if (lv > 1) lv = 1;
            const int level = (int)(lv * 16 + 0.5f);
            if (ch->fx == ISO_FX_FADE || lv >= 1) {
                const fart_clip_t screen = { 0, 0, w, h };
                fart_draw_dither(px, w, &screen, &m->art->img[pt->img], pt->x + ox, pt->y + oy, pt->mirror, level);
            } else
                draw_mosaic(m, px, w, h, pt->img, pt->x + ox, pt->y + oy, pt->mirror, 1 + (int)((1 - lv) * 7), level);
        }
        return;
    }
    if (ch->what == 1 && ch->fx == ISO_FX_RISE) {               // the column: its top block, fills down to the base
        const int base = ch->from.h < ch->to.h ? ch->from.h : ch->to.h, topl = ch->from.h > ch->to.h ? ch->from.h : ch->to.h;
        pos(m, ch->c, ch->r, ch->to.h, &x, &y);
        y += change_lift(m, ch->c, ch->r);
        pos(m, ch->c, ch->r, base, &vx, &vy);
        vx += ox;
        vy += oy + 16;                                            // the bottom of the block at the base
        for (int k = topl - base; k >= 1; k--)
            draw_on_floor(m, px, w, h, ch->to.fill, x + ox, y + k * ISO_STEP + oy, false, vx, vy);
        draw_on_floor(m, px, w, h, ch->to.top, x + ox, y + oy, false, vx, vy);
        draw_on_floor(m, px, w, h, ch->to.obj, x + ox, y - ISO_STEP + oy, odd, vx, vy);
        return;
    }
    if (ch->what == 0 && ch->fx == ISO_FX_RISE) {               // an object out of (or into) its cell's top
        const bool coming = ch->to.obj != ISO_NONE;
        const int idx = coming ? ch->to.obj : ch->from.obj;
        if (idx == ISO_NONE || idx >= m->art->nimg) return;
        const float e = ease((float)ch->t / (2 * LEVEL_MS));
        const int depth = m->art->img[idx].h, off = (int)(depth * (coming ? 1 - e : e));
        pos(m, ch->c, ch->r, ch->to.h + 1, &x, &y);
        pos(m, ch->c, ch->r, ch->to.h, &vx, &vy);
        draw_on_floor(m, px, w, h, idx, x + ox, y + off + oy, odd, vx + ox, vy + oy + ISO_HH);
        return;
    }
    if (ch->fx != ISO_FX_DROP) return;
    const int off = drop_offset(ch->t);
    const fart_clip_t screen = { 0, 0, w, h };
    if (ch->what == 0) {
        if (ch->to.obj == ISO_NONE || ch->to.obj >= m->art->nimg) return;
        pos(m, ch->c, ch->r, ch->to.h + 1, &x, &y);
        fart_draw(px, w, &screen, &m->art->img[ch->to.obj], x + ox, y + off + oy, odd);
    } else if (ch->to.h > ch->from.h) {
        for (int l = ch->from.h + 1; l <= ch->to.h; l++) {
            pos(m, ch->c, ch->r, l, &x, &y);
            const int idx = l == ch->to.h ? ch->to.top : ch->to.fill;
            if (idx != ISO_NONE && idx < m->art->nimg) fart_draw(px, w, &screen, &m->art->img[idx], x + ox, y + off + oy, false);
        }
        if (ch->to.obj != ISO_NONE && ch->to.obj < m->art->nimg) {
            pos(m, ch->c, ch->r, ch->to.h + 1, &x, &y);
            fart_draw(px, w, &screen, &m->art->img[ch->to.obj], x + ox, y + off + oy, odd);
        }
    }
}

// The pieces flying, the dust and a teleport's sparks, over everything.
static void draw_change_bits(const iso_map_t *m, uint16_t *px, int w, int h, const iso_change_t *ch)
{
    const int ox = -m->camx, oy = -m->camy;
    const uint32_t pt = ch->t >= ch->pieces_at ? ch->t - ch->pieces_at : PIECE_MS;
    if (pt < PIECE_MS) {
        const float s = pt / 1000.0f;
        const int tint = pt > PIECE_MS * 2 / 3 ? FART_DIM : FART_PLAIN;
        for (int i = 0; i < ch->npieces; i++) {
            const iso_piece_t *p = &ch->pieces[i];
            const int ax = p->ax + (int)(p->vx * s) + ox, ay = p->ay + (int)(p->vy * s + PIECE_G * s * s / 2) + oy;
            const bool mirror = (m->view & 1) && p->img == ch->from.obj;
            const int x0 = mirror ? ax - p->x0 - p->w : ax + p->x0, y0 = ay + p->y0;
            fart_clip_t c = { x0 < 0 ? 0 : x0, y0 < 0 ? 0 : y0, x0 + p->w > w ? w : x0 + p->w, y0 + p->h > h ? h : y0 + p->h };
            if (c.x0 < c.x1 && c.y0 < c.y1) fart_draw_tint(px, w, &c, &m->art->img[p->img], ax, ay, mirror, tint);
        }
    }
    int x, y;
    const int seed = ch->c * 131 + ch->r * 7;
    if (ch->what >= 3) {                                          // a crack, a drop: dust where it hits
        if (ch->landed) {
            pos(m, ch->c, ch->r, ch->to.h, &x, &y);
            const uint32_t at = ch->what == 3 ? ch->start : ch->pieces_at;
            draw_dust(px, w, h, x + ox, y + oy + 2, ch->t - at, DUST_MS, img_colour(m, ch->to.top), seed);
        }
    } else if (ch->fx == ISO_FX_TELEPORT) {                       // sparks rising through it
        pos(m, ch->c, ch->r, ch->to.h > ch->from.h ? ch->to.h : ch->from.h, &x, &y);
        for (int i = 0; i < 14; i++) {
            const uint32_t hs = cell_hash(seed + i, i * 5 + 3);
            const int life = 300 + (int)(hs % 300), born = (int)((hs >> 9) % (ch->len > 300 ? ch->len - 300 : 1));
            const int age = (int)ch->t - born;
            if (age < 0 || age >= life) continue;
            const int X = x + ox + (int)(hs % 25) - 12, Y = y + oy + 8 - (int)((hs >> 5) % 10) - age * 40 / life;
            const uint16_t col = age < life / 2 ? 0xffff : 0x7ffd;
            if (X >= 0 && Y >= 0 && X < w && Y < h) px[Y * w + X] = col;
            if (X + 1 < w && X + 1 >= 0 && Y >= 0 && Y < h) px[Y * w + X + 1] = 0x5d7f;
        }
    } else if (ch->fx == ISO_FX_DROP && ch->landed) {
        const int lvl = ch->what == 0 ? ch->to.h : ch->from.h;
        pos(m, ch->c, ch->r, lvl, &x, &y);
        draw_dust(px, w, h, x + ox, y + oy + 2, ch->t - drop_land_ms(), DUST_MS, img_colour(m, ch->to.top), seed);
    } else if (ch->fx == ISO_FX_RISE) {                           // dust while it moves, round its base
        const int lvl = ch->what == 0 ? ch->to.h : (ch->from.h < ch->to.h ? ch->from.h : ch->to.h);
        pos(m, ch->c, ch->r, lvl, &x, &y);
        const uint32_t moving = ch->len > DUST_MS / 2 ? ch->len - DUST_MS / 2 : ch->len;
        draw_dust(px, w, h, x + ox, y + oy + (ch->what == 0 ? 2 : 10), ch->t % 260, 260 + (ch->t >= moving) * 200,
                  img_colour(m, ch->to.top), seed + (int)(ch->t / 260));
    }
}

// A spin's turn now: screen offsets from the pivot on the ground turn by M(a) = [[cos a, -2 sin a], [sin a / 2,
// cos a]] (the projection of the map's plane turned by a), in 16.16. Before half way the old view turns from 0 toward
// a quarter; after, the new view comes from a quarter back to 0. *px, *py: the pivot (world).
typedef struct { int32_t a, b, c, d; } spin_t;
static spin_t spin_now(const iso_map_t *m, int *px, int *py, bool inverse)
{
    iso_world(m, m->turn_c, m->turn_r, px, py);
    float p = (float)m->turn_ms / m->turn_len;
    p = p * p * (3 - 2 * p);                                              // eased in and out
    float a = m->turn_by * 1.5707963f * p;
    if (m->turned) a -= m->turn_by * 1.5707963f;
    if (inverse) a = -a;
    const float co = cosf(a), si = sinf(a);
    return (spin_t){ (int32_t)(co * 65536), (int32_t)(-2 * si * 65536), (int32_t)(si * 32768), (int32_t)(co * 65536) };
}

// The layer drawn turned round the pivot (each screen pixel looks up where it comes from).
static void draw_spun_layer(const iso_map_t *m, uint16_t *px, int w, int h)
{
    int pwx, pwy;
    const spin_t t = spin_now(m, &pwx, &pwy, true);
    const int ox = m->camx - pwx, oy = m->camy - pwy;                     // screen (0, 0) from the pivot
    const int32_t bx = (pwx - m->lx) * 65536 + 32768, by = (pwy - m->ly) * 65536 + 32768;
    for (int y = 0; y < h; y++) {
        uint16_t *line = px + y * w;
        const int dy = oy + y;
        int32_t sx = bx + t.a * ox + t.b * dy, sy = by + t.c * ox + t.d * dy;
        for (int x = 0; x < w; x++, sx += t.a, sy += t.c) {
            const int lx = sx >> 16, ly = sy >> 16;
            line[x] = (unsigned)lx < (unsigned)m->lw && (unsigned)ly < (unsigned)m->lh ? m->layer[ly * m->lw + lx] : m->sky;
        }
    }
}

void iso_draw(iso_map_t *m, uint16_t *px, int w, int h, int cc, int cr, uint32_t now_ms)
{
    m->now = now_ms;
    tick_scenery(m, w, h);
    const bool spinning = m->turn_ms && m->turn_style == ISO_TURN_SPIN;
    if (spinning) draw_spun_layer(m, px, w, h);
    // The static layer under the camera.
    for (int y = 0; y < h && !spinning; y++) {
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
    for (int r = 0; r < m->rows && !spinning; r++)
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

    if (cc >= 0 && iso_inside(m, cc, cr) && !spinning) {
        int x, y;
        iso_world(m, cc, cr, &x, &y);
        const int pulse = (now_ms / 120) % 6;
        draw_cursor(px, w, h, x - m->camx, y - m->camy, pulse < 3 ? rgb565(0xfff3a0) : rgb565(0xffffff));
    }

    // Units and moving scenery, back to front in the view; while stepping, a unit counts as being on the nearer of
    // its two cells; on one cell, the scenery first (a unit rides a rising column), a dropped image last.
    int ent[ISO_UNITS + ISO_CHANGES + ISO_BOBS], key[ISO_UNITS + ISO_CHANGES + ISO_BOBS], n = 0;
    for (int i = 0; i < m->nunits; i++) {
        const iso_unit_t *u = &m->units[i];
        int c0, r0, c1, r1;
        to_view(m, u->c, u->r, &c0, &r0);
        to_view(m, u->fc, u->fr, &c1, &r1);
        ent[n] = i;
        key[n++] = ((c0 + r0 > c1 + r1 ? c0 + r0 : c1 + r1) * 256 + c0) * 4 + 1;
    }
    for (int k = 0; k < ISO_CHANGES && !spinning; k++) {
        if (!m->chg[k].len) continue;
        int vc, vr;
        to_view(m, m->chg[k].c, m->chg[k].r, &vc, &vr);
        ent[n] = ISO_UNITS + k;
        key[n++] = ((vc + vr) * 256 + vc) * 4 + (m->chg[k].what == 4 ? 2 : 0);   // a drop lands on whoever is there
    }
    for (int k = 0; k < m->nbcells; k++) {                       // bobbing columns, where they are now
        int vc, vr;
        to_view(m, m->bcells[k] % m->cols, m->bcells[k] / m->cols, &vc, &vr);
        ent[n] = ISO_UNITS + ISO_CHANGES + k;
        key[n++] = ((vc + vr) * 256 + vc) * 4;
    }
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && key[j - 1] > key[j]; j--) {
            int t = key[j - 1];
            key[j - 1] = key[j];
            key[j] = t;
            t = ent[j - 1];
            ent[j - 1] = ent[j];
            ent[j] = t;
        }
    for (int k = 0; k < n; k++) {
        if (ent[k] >= ISO_UNITS + ISO_CHANGES) {
            const int i = m->bcells[ent[k] - ISO_UNITS - ISO_CHANGES], c = i % m->cols, r = i / m->cols;
            int dx = 0, dy = 0;
            if (spinning) {                                   // carried round like a unit
                int x, y, pwx, pwy;
                iso_world(m, c, r, &x, &y);
                const spin_t t = spin_now(m, &pwx, &pwy, false);
                const int gx = x - pwx, gy = y - pwy;
                dx = pwx + (int)(((int64_t)t.a * gx + (int64_t)t.b * gy) >> 16) - x;
                dy = pwy + (int)(((int64_t)t.c * gx + (int64_t)t.d * gy) >> 16) - y;
            }
            draw_column_at(m, px, w, &screen, c, r, m->camx - dx, m->camy - dy - bob_offset(m, c, r), shown_cell(m, c, r));
            continue;
        }
        if (ent[k] >= ISO_UNITS) {
            draw_change(m, px, w, h, &m->chg[ent[k] - ISO_UNITS]);
            continue;
        }
        const iso_unit_t *u = &m->units[ent[k]];
        bool mirror;
        if (u->hidden) continue;
        const int idx = unit_frame(m, u, &mirror);
        if (idx < 0) continue;
        int x, y, sdx = 0, sdy = 0;
        iso_feet(m, u, &x, &y);
        if (spinning) {                                       // upright, carried round with the ground under it
            int pwx, pwy;
            const spin_t t = spin_now(m, &pwx, &pwy, false);
            const int gx = x - pwx, gy = y - pwy;
            sdx = pwx + (int)(((int64_t)t.a * gx + (int64_t)t.b * gy) >> 16) - x;
            sdy = pwy + (int)(((int64_t)t.c * gx + (int64_t)t.d * gy) >> 16) - y;
            x += sdx;
            y += sdy;
        }
        const fart_img_t *im = &m->art->img[idx];
        // A knocked-out unit fades instead of dimming; a hit flashes white.
        const int tint = u->flash && (u->flash / 60) % 2 ? FART_WHITE : u->dim && !u->still ? FART_DIM : FART_PLAIN;
        // Facing away, the weapon is behind the body; facing the viewer, in front of it.
        const int sd = iso_screen_dir(m, u->dir);
        const bool away = sd == FART_NE || sd == FART_NW;
        const bool armed = !u->falling;                       // a fallen hero has let go of the weapon
        if (away && armed) draw_weapon(m, px, w, &screen, u, tint, sdx, sdy);
        const fart_style_t st = { .tint = tint, .cover = u->cover, .ramp = u->ramp, .shade = u->shade, .zoom = u->zoom,
                                  .tilt = u->tilt, .fade = u->fade };
        fart_draw_style(px, w, &screen, im, x - m->camx, y - m->camy, mirror, &st);
        if (!away && armed) draw_weapon(m, px, w, &screen, u, tint, sdx, sdy);
        if (u->counter && !u->dim) {                          // ready to strike back: a small sword sign
            const int cx = x - m->camx + 8, cy = y - m->camy - 44 - (int)((now_ms / 200) % 2);
            for (int k = 0; k < 6; k++) put(px, w, h, cx + k, cy + 5 - k, 0xffe0);   // the blade
            put(px, w, h, cx + 1, cy + 3, 0xffe0);                                 // the guard
            put(px, w, h, cx + 3, cy + 5, 0xffe0);
        }
        if (spinning) continue;                               // the scenery over it waits for the spin to end
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
        int vc0, vr0, vc1, vr1;
        to_view(m, u->c, u->r, &vc0, &vr0);
        to_view(m, u->fc, u->fr, &vc1, &vr1);
        const int bc = vc0 > vc1 ? vc0 : vc1, br = vr0 > vr1 ? vr0 : vr1;
        const int level = iso_cell(m, u->c, u->r)->h > iso_cell(m, u->fc, u->fr)->h ? iso_cell(m, u->c, u->r)->h
                                                                                    : iso_cell(m, u->fc, u->fr)->h;
        const int vcols = view_cols(m), vrows = view_rows(m);
        for (int vr = br; vr < br + 3 && vr < vrows; vr++)
            for (int vc = bc; vc < bc + 3 && vc < vcols; vc++) {
                int c, r;
                from_view(m, vc, vr, &c, &r);
                if ((c == u->c && r == u->r) || (c == u->fc && r == u->fr)) continue;
                const iso_cell_t *cell = iso_cell(m, c, r);
                if (cell->h > level || (cell->obj != ISO_NONE && cell->h + 1 > level))
                    draw_column(m, px, w, &clip, c, r, m->camx, m->camy);
            }
    }

    for (int k = 0; k < ISO_CHANGES && !spinning; k++)
        if (m->chg[k].len) draw_change_bits(m, px, w, h, &m->chg[k]);

    // Turning: the picture narrows to a line through the kept cell, then widens in the new view, darker near the
    // line (as if seen edge-on).
    if (m->turn_ms && m->turn_style == ISO_TURN_NARROW && w <= 512) {
        static uint16_t row[512];
        int fx, fy;
        iso_world(m, m->turn_c, m->turn_r, &fx, &fy);
        fx -= m->camx;
        if (fx < 0) fx = 0;
        if (fx >= w) fx = w - 1;
        const float p = (float)m->turn_ms / m->turn_len, k = cosf(p * 3.1415926f);
        const float sc = k < 0 ? -k : k;
        const int s = (int)(sc * 256) < 6 ? 6 : (int)(sc * 256);              // 8.8 width, never quite a line
        const int inv = (256 * 256) / s, dark = (256 - s) / 2;               // dark: 0..125 of 256
        for (int y = 0; y < h; y++) {
            uint16_t *line = px + y * w;
            memcpy(row, line, w * 2);
            for (int x = 0; x < w; x++) {
                const int sx = fx + (((x - fx) * inv) >> 8);
                if (sx < 0 || sx >= w) {
                    line[x] = m->sky;
                    continue;
                }
                const uint16_t v = row[sx];
                if (v == m->sky) {                                    // the background stays as it is
                    line[x] = v;
                    continue;
                }
                const int r = v >> 11, g = (v >> 5) & 63, b = v & 31;
                line[x] = (uint16_t)(((r * (256 - dark) >> 8) << 11) | ((g * (256 - dark) >> 8) << 5) | (b * (256 - dark) >> 8));
            }
        }
    }
}
