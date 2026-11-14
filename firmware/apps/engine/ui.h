// In-game windows for full-screen games: translucent boxes, menus with a cursor, bars, banners. Drawn into the
// frame's buffer with the interface's 8 x 8 font (fh1_px_text).
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define UI_ITEMS 10

typedef struct {
    const char *title;
    const char *items[UI_ITEMS];
    const char *notes[UI_ITEMS];     // right-aligned (a cost, a count), or NULL
    int n, sel;
    uint16_t off;                    // bit i: item i greyed (can be moved onto, not chosen)
    uint32_t colors[UI_ITEMS];       // an item's colour (0: the usual), its note's
    uint32_t note_colors[UI_ITEMS];
} ui_menu_t;

typedef struct { uint16_t *px; int w, h; } ui_t;

void ui_box(const ui_t *u, int x, int y, int bw, int bh);
void ui_text(const ui_t *u, int x, int y, const char *s, uint32_t rgb);
// Text with a gentle wave running through its letters once a second (the chosen line of a menu).
void ui_text_wave(const ui_t *u, int x, int y, const char *s, uint32_t rgb, uint32_t now_ms);
// A line of hints with the controller's button icons (fh1_px_key, player 1's controller's style): "{A} lutar
// {B} voltar"; keys {A} {B} {X} {Y} {S} (Start) {s} (Select) {M} (Menu) {LR} {UD}. Returns the x after it;
// ui_hints_width measures it (to place it from the right).
int ui_hints(const ui_t *u, int x, int y, const char *s, uint32_t rgb);
int ui_hints_width(const char *s);
void ui_bar(const ui_t *u, int x, int y, int bw, int value, int max, uint32_t rgb);
// The menu's size for its items; draws it with its top-left at (x, y).
void ui_menu_size(const ui_menu_t *m, int *bw, int *bh);
void ui_menu_draw(const ui_t *u, const ui_menu_t *m, int x, int y, uint32_t now_ms);
// A key for the menu: the item chosen (>= 0), UI_BACK (B), or UI_NONE.
enum { UI_NONE = -1, UI_BACK = -2 };
int ui_menu_key(ui_menu_t *m, int key);
// Sounds for the menus, if the game gives a player: moving, choosing, going back, not allowed.
enum { UI_SND_MOVE, UI_SND_OK, UI_SND_BACK, UI_SND_NO };
extern void (*ui_sound)(int what);
// A line in the middle of the screen, in a box.
void ui_banner(const ui_t *u, const char *s, uint32_t rgb);
