// FH1 art packs (.fart, made by tools/fh1art.py): images stored as rows of opaque runs (RGB565), and
// animations (frames per facing). The whole file is loaded into PSRAM; images are drawn straight from it.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

enum { FART_SE, FART_SW, FART_NW, FART_NE, FART_DIRS };
#define FART_NONE 0xFFFF

typedef struct {
    const char *name;
    int16_t ox, oy;                  // top-left corner from the anchor
    uint16_t w, h;
    const uint16_t *spans;           // per row: runs, then (skip, len, len pixels) per run
    int16_t hx, hy;                  // the hand from the anchor (FART_NO_HAND: none)
    const uint8_t *parts;            // 2 bits a pixel over w x h: FART_PART_*, or NULL
} fart_img_t;
#define FART_NO_HAND (-32768)
enum { FART_PART_NONE, FART_PART_TORSO, FART_PART_ARMS, FART_PART_LEGS };

typedef struct {
    const char *name;
    uint8_t dirs, frames;
    uint16_t ms;                     // per frame
    const uint16_t *idx;             // dirs x frames image indexes, FART_NONE where missing
} fart_anim_t;

typedef struct {
    uint8_t *data;
    int nimg, nanim;
    fart_img_t *img;
    fart_anim_t *anim;
    uint16_t **orig;                 // per image: its pixels as loaded, once recoloured (fart_recolor), else NULL
    uint8_t *pool;                   // where those copies live: 16 KB blocks (PSRAM), chained
    size_t pool_used;
} fart_t;

typedef struct { int x0, y0, x1, y1; } fart_clip_t;   // x0 <= x < x1, y0 <= y < y1

bool fart_load(fart_t *a, const char *path);
void fart_free(fart_t *a);
int fart_image(const fart_t *a, const char *name);                       // its index, or -1
const fart_anim_t *fart_anim(const fart_t *a, const char *name);         // NULL if missing
// Dissolving: only the pixels whose place on screen passes an ordered 4 x 4 dither at level 0..16 (0: none, 16: all).
void fart_draw_dither(uint16_t *dst, int stride, const fart_clip_t *clip, const fart_img_t *im, int ax, int ay,
                      bool mirror, int level);
// An image unpacked into w x h pixels and a mask (1 byte a pixel), row by row; false if it is bigger than max.
bool fart_unpack(const fart_img_t *im, uint16_t *px, uint8_t *mask, int max);
// Recolours an image's pixels in place through fn (a realm's colours over the scenery); fn NULL puts back the colours
// it was loaded with. The first call keeps a copy of the original pixels, so each call starts from them. false: no
// memory for the copy (the image is unchanged).
bool fart_recolor(fart_t *a, int idx, uint16_t (*fn)(uint16_t v, void *user), void *user);
// The image to show for a facing at a time (looping); a missing facing is its mirror (se/sw, nw/ne): *mirror.
int fart_frame(const fart_anim_t *an, int dir, uint32_t t_ms, bool *mirror);
// Draws an image with its anchor at (ax, ay) into a w-wide RGB565 buffer, clipped.
void fart_draw(uint16_t *dst, int stride, const fart_clip_t *clip, const fart_img_t *im, int ax, int ay, bool mirror);
// The same with a tint: FART_DIM halves every colour, FART_WHITE draws the shape white (a hit).
enum { FART_PLAIN, FART_DIM, FART_WHITE };
void fart_draw_tint(uint16_t *dst, int stride, const fart_clip_t *clip, const fart_img_t *im, int ax, int ay, bool mirror,
                    int tint);
// Dressed: the pixels of the parts in cover (bit 1 << FART_PART_*) take the ramp's shade for their brightness
// (4 RGB565 shades, dark to light; outlines keep their colour): armour over the body.
void fart_draw_dressed(uint16_t *dst, int stride, const fart_clip_t *clip, const fart_img_t *im, int ax, int ay,
                       bool mirror, int tint, uint8_t cover, const uint16_t ramp[4]);
// Everything a draw can do to an image: the tint, armour, a colour for all of it (32 shades by brightness, dark to
// light: a realm's creatures, an elemental weapon; outlines keep theirs), twice the size (bosses), turned about
// its anchor (degrees, clockwise: falling over).
typedef struct {
    int tint;
    uint8_t cover;
    const uint16_t *ramp;
    const uint16_t *shade;
    uint8_t zoom;
    int16_t tilt;
    uint8_t fade;                    // 0..255: toward a dim grey (knocked out)
} fart_style_t;
void fart_draw_style(uint16_t *dst, int stride, const fart_clip_t *clip, const fart_img_t *im, int ax, int ay,
                     bool mirror, const fart_style_t *st);
