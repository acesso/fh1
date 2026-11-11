// The looks' pictures: desktops, window frames, corners, title plates and cursors, drawn pixel by pixel from the
// theme's colours. Plain C (no LVGL): ui.c turns them into images, and tools/lookpreview.c renders them on a computer.
//
// Coordinates are in units; `u` is pixels per unit (1, or 2 on a 480 x 480 screen). A window's state is 0 out of
// focus, 1 in focus, 2 an alert (a message or a question). `grow` (0..256) is how far a window's opening has got;
// `phase` counts the running decoration's steps (125 ms each).
#pragma once
#include <stdint.h>

enum { LOOK_GEN, LOOK_HEX, LOOK_GARDEN, LOOK_SCIFI };

typedef struct {
    int u;
    uint32_t paper, ink, dim, alert, text, inset, desk_a, desk_b;   // 0xRRGGBB
} look_pal_t;

// The desktop's tile, RGB565, at most LOOK_DESK_MAX pixels a side; its size in *w, *h.
#define LOOK_DESK_MAX 96
void look_desk(int look, const look_pal_t *p, uint16_t *px, int *w, int *h);

// Looks that draw their frame (garden, sci-fi): its thickness in units, from the box's outer edge inwards (0: the
// box's own border, Gen and Hex). A strip is one side's frame, ARGB8888: side 0 top, 1 bottom (len x thick), 2 left,
// 3 right (thick x len), each `out` units thicker reaching outside the box, row (or column) 0 the outermost;
// len in pixels along the side; perimeter positions run clockwise from the top-left, so a
// vine can grow round the window (grow) and sci-fi plates close from the corners.
int look_strip_thick(int look);
int look_strip_out(int look);         // units of each strip outside the box (leaves hang over the edge)
void look_strip(int look, const look_pal_t *p, int side, int len, int w, int h, int state, int grow, int phase, uint32_t *px);

// Corner pieces, ARGB8888, size x size units, `out` units of it reaching past the box's corner; i 0 top-left,
// 1 top-right, 2 bottom-left, 3 bottom-right.
int look_corner_size(int look);
int look_corner_out(int look);
void look_corner(int look, const look_pal_t *p, int i, int state, int grow, int phase, uint32_t *px);

// The title's plate: its size and where it goes for a title tw pixels wide in a content area cw wide (x, y of the
// plate and of the text, in the window's content coordinates), then the picture, ARGB8888.
void look_title_box(int look, int u, int cw, int tw, int th, int *w, int *h, int *x, int *y, int *tx, int *ty);
void look_title(int look, const look_pal_t *p, int w, int h, int tw, int state, int phase, uint32_t *px);
uint32_t look_title_ink(int look, const look_pal_t *p, int state);

// Where a window's body starts, in units (under the title).
int look_body_top(int look);

// Cursors in colour (garden, sci-fi), ARGB8888, at most 16 x 16 pixels: solid in the window in focus, `hollow`
// in the others. Returns 0 for looks whose cursor is one colour (drawn by ui.c, tinted with the ink).
int look_cursor(int look, const look_pal_t *p, int hollow, uint32_t *px, int *w, int *h);

// How the item in focus reads, and the others.
uint32_t look_focus_text(int look, const look_pal_t *p);

// The mote: a small sprite moving round the window in focus (garden: a butterfly, sci-fi: a spark), ARGB8888, at
// most 16 x 16 pixels; 0 for looks without one. look_mote_at: where it is at t ms on a box w x h pixels (the
// sprite's top-left from the box's outer top-left) and which picture; 0 while out of sight.
int look_mote(int look, const look_pal_t *p, int frame, uint32_t *px, int *w, int *h);
int look_mote_at(int look, int u, int w, int h, uint32_t t, int *x, int *y, int *frame);
