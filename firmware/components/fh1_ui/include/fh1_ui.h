// The FH1 interface: a desktop with cascading windows, driven by buttons (firmware/PLAN.md, "Interface").
// All LVGL work happens in the UI task. Other tasks hand work over with fh1_ui_async().
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "lvgl.h"
#include "fh1_input.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t fh1_ui_start(void);

typedef void (*fh1_ui_fn)(void *arg);
void fh1_ui_async(fh1_ui_fn fn, void *arg);     // run fn(arg) in the UI task
// Called in the UI task whenever any device changes state (up to 8 watchers).
void fh1_ui_on_devices_changed(fh1_ui_fn fn, void *arg);
// What Menu does (the launcher sets this).
void fh1_ui_set_menu_handler(fh1_ui_fn fn, void *arg);

// The top bar shows the top-level menus as tabs, each with its icon (fh1_ui_icon_t, -1 none; icons may be NULL);
// the current one is highlighted (current < 0: none).
void fh1_ui_set_tabs(const char *const *labels, const int *icons, int n, int current);

// The bottom bar shows live status, read once a second from this function.
typedef struct {
    bool wifi_up;
    int wifi_rssi;          // dBm
    bool sd;                // a card is mounted
    bool battery;           // the device has a battery
    int battery_pct;
    bool charging;
    // The tray: gamepads (an icon each, shaped by kind, in its colour, its battery as a gauge under it) and the
    // sound (a speaker whose waves show the volume).
    uint8_t pads;
    struct {
        uint8_t kind;               // FH1_PAD_* (fh1_gamepad.h)
        uint32_t rgb;               // 0xRRGGBB
        int8_t battery;             // 0..100 %, -1 unknown
    } pad[4];
    bool sound;                     // an audio output is ready
    uint8_t volume;                 // 0..100
} fh1_ui_status_t;
typedef void (*fh1_ui_status_fn)(fh1_ui_status_t *st);
void fh1_ui_set_status_fn(fh1_ui_status_fn fn);

// Stop drawing and handling keys, so a full-screen program (the emulator) can own the display. Resuming
// with repaint_all redraws the whole screen; without, only what changes (a menu over a game picture).
// Queued calls still run while paused.
void fh1_ui_pause(bool paused, bool repaint_all);

// The FPS overlay (screens drawn per second, top right); the choice is saved.
void fh1_ui_set_show_fps(bool on);
bool fh1_ui_show_fps(void);

// Screen: the size in layout units (pixels divided by the scale), and the scale (1 or 2).
int fh1_ui_scale(void);
int fh1_ui_width(void);
int fh1_ui_height(void);

// Themes (palettes and box decoration); the choice is saved and applies at once.
int fh1_ui_themes(const char *const **ids, const char *const **names);
const char *fh1_ui_theme(void);
void fh1_ui_set_theme(const char *id);

// ── Windows ──
typedef struct fh1_win fh1_win_t;
typedef bool (*fh1_win_key_cb)(fh1_win_t *win, int key, void *user);   // true if handled
typedef void (*fh1_win_close_cb)(fh1_win_t *win, void *user);

fh1_win_t *fh1_win_create(const char *title, int w, int h);   // w, h in layout units
lv_obj_t *fh1_win_body(fh1_win_t *win);
void fh1_win_set_title(fh1_win_t *win, const char *title);
void fh1_win_close(fh1_win_t *win);
void fh1_win_focus(fh1_win_t *win);
fh1_win_t *fh1_win_top(void);                   // the focused window, or NULL
bool fh1_win_is_open(fh1_win_t *win);
void fh1_win_on_key(fh1_win_t *win, fh1_win_key_cb cb, void *user);
void fh1_win_on_close(fh1_win_t *win, fh1_win_close_cb cb, void *user);
void fh1_win_clear(fh1_win_t *win);              // empty the body, keeping the window
// Ownership, so a packaged app's windows can be found and closed with it.
void fh1_win_set_owner(fh1_win_t *win, void *owner);
size_t fh1_win_count_owned(void *owner);
void fh1_win_close_owned(void *owner);

// ── Widgets ──
lv_obj_t *fh1_ui_label(lv_obj_t *parent, const char *text);      // prose: wraps
// Values keep to one line and slide sideways, back and forth, when they do not fit (space is short).
lv_obj_t *fh1_ui_line(lv_obj_t *parent, const char *text);
lv_obj_t *fh1_ui_value(lv_obj_t *parent, const char *name);       // "name  value", in the meters' column
void fh1_ui_value_set(lv_obj_t *row, const char *text);
lv_obj_t *fh1_ui_button(fh1_win_t *win, lv_obj_t *parent, const char *text, void (*cb)(void *user), void *user);
void fh1_ui_button_set_text(lv_obj_t *btn, const char *text);
// Rich text: parts with their own colour (0xRRGGBB, 0 = theme) and font (NULL = theme). Parts marked
// hop jump up a pixel in a wave, one every 100 ms, then rest 300 ms (one shared timer for the whole UI).
typedef struct {
    const char *text;
    uint32_t color;
    const lv_font_t *font;
    bool hop;
} fh1_ui_span_t;
lv_obj_t *fh1_ui_rich_text(lv_obj_t *parent, const fh1_ui_span_t *spans, int n);
// A window title in rich text (an app's title can match its menu entry).
void fh1_win_set_title_rich(fh1_win_t *win, const fh1_ui_span_t *spans, int n);
// A button with rich text instead of a plain label.
lv_obj_t *fh1_ui_button_spans(fh1_win_t *win, lv_obj_t *parent, const fh1_ui_span_t *spans, int n,
                              void (*cb)(void *user), void *user);
void fh1_ui_dim(lv_obj_t *obj, bool dim);         // greyed look; still focusable
int fh1_ui_focused_index(fh1_win_t *win);         // index of the focused widget in the window, or -1
void fh1_ui_focus_index(fh1_win_t *win, int index);

// ── Button chips and footers ──
// A chip is a physical button drawn in its fixed colour (A red, B yellow, X blue, Y green, Start/Select
// white pills, Menu a dark chip with a "three bars" icon). Windows list their actions in a footer of
// chips with a word each, apart from the content: (A) play  (X) test  (B) close.
#define FH1_HINT_LEFTRIGHT 100        // the D-pad's left and right, as one chip
#define FH1_HINT_CHARS 9              // a hint's word is cut to this many characters: keep them short
// Footers are a grid of two columns: chip + word per cell, in the order given (left, right, next row...).
typedef struct {
    int key;                          // FH1_KEY_* or FH1_HINT_LEFTRIGHT
    const char *text;                 // already translated
} fh1_hint_t;
lv_obj_t *fh1_ui_key_chip(lv_obj_t *parent, int key);
void fh1_win_set_hints(fh1_win_t *win, const fh1_hint_t *hints, int n);
// ── Icons ──
// The common icons, pixel art in colour. Animated, each has its own motion: an arrow drops into a tray
// (download: something arrives) or rises out of it (upload), a gold arrow sinks into a chip (firmware),
// a letter bobs, a battery's last bar or a warning's mark blinks. big: twice the size.
typedef enum {
    FH1_ICON_SD, FH1_ICON_INTERNAL,
    FH1_ICON_DOWNLOAD, FH1_ICON_UPLOAD, FH1_ICON_FIRMWARE,
    FH1_ICON_MESSAGE, FH1_ICON_BATTERY_LOW, FH1_ICON_WARNING,
    FH1_ICON_PACKAGE,                 // a parcel, an arrow dropping in
    FH1_ICON_GAMEBOY,                 // the GameBoy Color app: a cartridge slides into the handheld
    FH1_ICON_NES,                     // the NES app: a cartridge slides into the console
    FH1_ICON_SMS,                     // the Master System app: a cartridge slides into the console
    FH1_ICON_MUSIC,                   // music files: a cassette tape, its reels turning
    // Small still icons for menu entries (10 x 9): System's and a bridge's.
    FH1_ICON_MENU_INSTALL, FH1_ICON_MENU_STORAGE, FH1_ICON_MENU_DEVICES, FH1_ICON_MENU_IO, FH1_ICON_MENU_BRIDGE,
    FH1_ICON_MENU_ABOUT, FH1_ICON_MENU_NETWORK, FH1_ICON_MENU_RENAME, FH1_ICON_MENU_FORGET, FH1_ICON_MENU_USERS,
    // The top-level menus' tabs, and the apps in the Apps menu (10 x 9, still).
    FH1_ICON_MENU_APPS, FH1_ICON_MENU_SYSTEM, FH1_ICON_MENU_GAMEBOY, FH1_ICON_MENU_NES, FH1_ICON_MENU_SMS, FH1_ICON_MENU_MUSIC,
    FH1_ICON_MENU_PACKAGE,
    FH1_ICON_COUNT,
} fh1_ui_icon_t;
lv_obj_t *fh1_ui_icon(lv_obj_t *parent, fh1_ui_icon_t icon, bool big, bool animated);
// File types: an uploaded file is shown with its type's icon and words (English, translated when shown).
// The system registers .fpk and .bin; an app registers the files it opens, with its own icon.
void fh1_ui_register_file_type(const char *ext, fh1_ui_icon_t icon, const char *receiving, const char *received);
// A small icon at the right end of a button, e.g. where an item is stored.
lv_obj_t *fh1_ui_button_icon(lv_obj_t *btn, fh1_ui_icon_t icon);
// A small icon before a button's text (menu entries).
lv_obj_t *fh1_ui_button_lead_icon(lv_obj_t *btn, fh1_ui_icon_t icon);

// A gamepad as the tray shows it: its shape by kind (FH1_PAD_*, fh1_gamepad.h) in its colour (0xRRGGBB), so
// two controllers can be told apart wherever they are listed. lead_pad puts it before the text of a button
// or a line (fh1_ui_line); pad_icon_set redraws it when the gamepad's look changes.
lv_obj_t *fh1_ui_pad_icon(lv_obj_t *parent, uint8_t kind, uint32_t rgb);
void fh1_ui_pad_icon_set(lv_obj_t *icon, uint8_t kind, uint32_t rgb);
lv_obj_t *fh1_ui_lead_pad(lv_obj_t *obj, uint8_t kind, uint32_t rgb);

// Pixel pictures made at run time (avatars): w x h pixels, ARGB8888 (0xAARRGGBB, 0 transparent), shown zoom
// times the unit. pixels_set copies a new picture in (an animation frame). lead_pixels puts one before a button's
// text, like a lead icon; the button grows to fit it.
lv_obj_t *fh1_ui_pixels(lv_obj_t *parent, int w, int h, int zoom);
void fh1_ui_pixels_set(lv_obj_t *img, const uint32_t *argb);
lv_obj_t *fh1_ui_button_lead_pixels(lv_obj_t *btn, int w, int h);

// Symbols in the text font (8 x 8, the text's colour), for references inside any text: "DualSense " FH1_SYM_BRIDGE "CBA2".
#define FH1_SYM_BRIDGE "\xEE\x80\x80"     // U+E000: a bridge (an extension board)
#define FH1_SYM_PAD "\xEE\x80\x81"        // U+E001: a gamepad
#define FH1_SYM_WIFI "\xEE\x80\x82"       // U+E002: Wi-Fi
#define FH1_SYM_BT "\xEE\x80\x83"         // U+E003: Bluetooth

// ── Sliders ──
// A value from min to max (a level, a volume) as a bar with the value in it. Focused, Left/Right move it by
// step. cb gets every step with done = false (preview it) and, once the moving stops, the value with
// done = true (save it). fmt prints the value, e.g. "%d%%".
typedef void (*fh1_ui_slider_cb)(int value, bool done, void *user);
lv_obj_t *fh1_ui_slider(fh1_win_t *win, lv_obj_t *parent, const char *name, const char *fmt, int value, int min,
                        int max, int step, fh1_ui_slider_cb cb, void *user);

// ── Views ──
// A window with several pages side by side, e.g. one per device of a kind: a line on top names the page
// shown ("Tela 1/3"), Left/Right slide between pages, only the page shown takes focus. Add pages, fill each
// (its returned object is the parent for widgets), then show the first with fh1_win_show_view(win, 0).
lv_obj_t *fh1_win_add_view(fh1_win_t *win, const char *title);
void fh1_win_show_view(fh1_win_t *win, int i);
int fh1_win_view(fh1_win_t *win);

// ── Paged lists ──
// For long lists (a thousand games): only the rows that fit on a page exist, filled by item() when the page
// turns. Up/Down move and cross pages, Left/Right turn pages, A calls pick(). One per window, created after
// the footer (it fills the body). item() writes the text and an icon (fh1_ui_icon_t, or -1 for none).
typedef void (*fh1_ui_list_item_cb)(int index, char *text, size_t len, int *icon, void *user);
typedef void (*fh1_ui_list_pick_cb)(int index, void *user);
lv_obj_t *fh1_ui_list(fh1_win_t *win, int count, fh1_ui_list_item_cb item, fh1_ui_list_pick_cb pick, void *user);
void fh1_ui_list_refresh(fh1_win_t *win, int count);      // the items changed
int fh1_ui_list_index(fh1_win_t *win);                    // the focused item, or -1
void fh1_ui_list_go(fh1_win_t *win, int index);           // focus an item (its page shows)

// ── Meters ──
// A bar with its value written inside: storage used, memory free, levels. colour: 0xRRGGBB for the fill;
// fh1_ui_level_colour() gives green / yellow / red for "how good" in percent.
lv_obj_t *fh1_ui_meter(lv_obj_t *parent, const char *name);       // name may be NULL
void fh1_ui_meter_set(lv_obj_t *meter, int pct, const char *text, uint32_t colour);
uint32_t fh1_ui_level_colour(int pct_good);

// ── Full-screen programs ──
// A program that owns the screen (fh1_ui_pause(true)) checks fh1_app_should_yield() each frame; when it is
// set, it stops drawing and calls this: an event's overlay shows over its frozen picture, then this returns
// and the program carries on. From the program's task.
void fh1_ui_lend_screen(void);

// ── Notifications ──
// A small box slides in at the top right for 4 s: a picture, a title, a line. The picture (pic_w x pic_h, up to
// 64 x 64, ARGB8888) is drawn by draw(user, t_ms, px) every 120 ms, so it can move (a face with its
// expression); user is copied (up to 64 bytes). One at a time; the others queue; while a full-screen program
// owns the screen they wait. From the UI task.
typedef void (*fh1_ui_pic_fn)(const void *user, uint32_t t_ms, uint32_t *px);
void fh1_ui_notify(const char *title, const char *text, int pic_w, int pic_h, fh1_ui_pic_fn draw, const void *user, size_t user_len);

// ── Dialogs ──
// The on-screen keyboard. done(text) gets the text, or NULL if cancelled.
void fh1_ui_keyboard(const char *title, const char *initial, size_t max_len, void (*done)(const char *text, void *user), void *user);
// The same for a PIN or a passphrase: what is typed shows as *.
void fh1_ui_keyboard_secret(const char *title, size_t max_len, void (*done)(const char *text, void *user), void *user);
void fh1_ui_message(const char *title, const char *text);
void fh1_ui_confirm(const char *title, const char *text, void (*cb)(bool yes, void *user), void *user);

#ifdef __cplusplus
}
#endif
