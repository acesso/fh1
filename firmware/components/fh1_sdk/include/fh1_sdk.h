// The FH1 SDK: the only API a packaged app may call (firmware/PLAN.md, "App packages").
// Apps include this header and the standard C headers. The firmware exports exactly these functions, plus
// this part of the C library: malloc calloc realloc free, printf snprintf vsnprintf sscanf, fopen fclose
// fread fwrite fgets fprintf fputs fseek ftell fflush remove rename, memcpy memset memmove memcmp, strlen
// strcmp strncmp strchr strrchr strstr strncpy strlcpy strlcat strncat, atoi strtol strtoul strtod, qsort
// rand srand time clock_gettime, sinf cosf sqrtf floorf atan2f fabsf asinf. Anything else fails to load
// ("can't find symbol").
//
// Versioning: a minor version only adds functions; a major version means apps must be rebuilt.
// The installer refuses an app built for another major, or for a newer minor than the firmware.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define FH1_SDK_MAJOR 0
#define FH1_SDK_MINOR 4               // 2: gamepads with motion, a canvas to draw on. 3: triangles, text width.
                                      // 4: users (who plays, their face), notifications, settings, saved states

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fh1_win fh1_win_t;
typedef struct _lv_obj_t lv_obj_t;        // a widget; apps only pass it around
typedef bool (*fh1_win_key_cb)(fh1_win_t *win, int key, void *user);   // return true if handled
typedef void (*fh1_win_close_cb)(fh1_win_t *win, void *user);

// ── App lifecycle ──
uint32_t fh1_sdk_version(void);           // (major << 16) | minor of the running firmware
const char *fh1_app_id(void);
const char *fh1_app_data_dir(void);       // the app's own folder for saved data (player 1's user's, SDK 0.4)
void fh1_app_exit(void);                  // close the app's windows and unload it
uint32_t fh1_millis(void);
int fh1_app_timer(uint32_t period_ms, void (*cb)(void *user), void *user);   // id, or -1; freed on unload
void fh1_app_timer_stop(int id);
void fh1_log(const char *fmt, ...);

// ── Windows and widgets ──
fh1_win_t *fh1_app_window(const char *title, int w, int h);    // owned by the app, closed when it unloads
lv_obj_t *fh1_win_body(fh1_win_t *win);
void fh1_win_set_title(fh1_win_t *win, const char *title);
void fh1_win_close(fh1_win_t *win);
void fh1_win_on_key(fh1_win_t *win, fh1_win_key_cb cb, void *user);
void fh1_win_on_close(fh1_win_t *win, fh1_win_close_cb cb, void *user);
lv_obj_t *fh1_ui_label(lv_obj_t *parent, const char *text);
void fh1_ui_label_set(lv_obj_t *label, const char *text);
lv_obj_t *fh1_ui_button(fh1_win_t *win, lv_obj_t *parent, const char *text, void (*cb)(void *user), void *user);
void fh1_ui_button_set_text(lv_obj_t *btn, const char *text);
void fh1_ui_dim(lv_obj_t *obj, bool dim);
void fh1_ui_message(const char *title, const char *text);
void fh1_ui_confirm(const char *title, const char *text, void (*cb)(bool yes, void *user), void *user);

#ifndef FH1_FIRMWARE      // the firmware has these in its own headers
// ── Keys (as passed to a window's key callback) ──
enum { FH1_KEY_UP, FH1_KEY_DOWN, FH1_KEY_LEFT, FH1_KEY_RIGHT, FH1_KEY_A, FH1_KEY_B, FH1_KEY_X, FH1_KEY_Y,
       FH1_KEY_START, FH1_KEY_SELECT, FH1_KEY_MENU };
bool fh1_input_is_down(int key);

// ── Capabilities ──
enum {
    FH1_CAP_DISPLAY = 1u << 0, FH1_CAP_BACKLIGHT = 1u << 1, FH1_CAP_BUTTONS = 1u << 2, FH1_CAP_STICK_ANALOG = 1u << 3,
    FH1_CAP_TOUCH = 1u << 4, FH1_CAP_AUDIO_OUT = 1u << 5, FH1_CAP_AUDIO_IN = 1u << 6, FH1_CAP_SD = 1u << 7,
    FH1_CAP_WIFI = 1u << 8, FH1_CAP_BLE = 1u << 9, FH1_CAP_LORA = 1u << 10, FH1_CAP_IMU = 1u << 11,
    FH1_CAP_RTC = 1u << 12, FH1_CAP_BATTERY = 1u << 13, FH1_CAP_BUTTON_LIGHTS = 1u << 14,
};
uint32_t fh1_caps(void);
const char *fh1_cap_name(uint32_t cap_bit);

// ── Audio out: always safe to call; FH1_UNAVAILABLE when there is no sound device ──
#define FH1_UNAVAILABLE (-2)
typedef struct fh1_audio fh1_audio_t;
fh1_audio_t *fh1_audio_open(uint32_t sample_rate);
int fh1_audio_write(fh1_audio_t *a, const int16_t *frames, size_t count);
void fh1_audio_set_volume(fh1_audio_t *a, uint8_t pct);
bool fh1_audio_available(void);
void fh1_audio_close(fh1_audio_t *a);
#endif

// ── Gamepads (SDK 0.2) ──
// The players' controllers, as System > Inputs and outputs assigns them (player 0 is player 1). Their full
// state: buttons by position, sticks, analog triggers and motion. Motion streams fully only while asked for
// (over a bridge's Wi-Fi it costs link capacity); asking is undone when the app unloads, as is holding the
// keys (while held, player 1's controller does not drive the menus: a game reads it instead).
typedef struct {
    uint32_t keys;              // as FH1 keys (1 << FH1_KEY_*)
    uint32_t buttons;           // FH1_PAD_BTN_*
    int16_t lx, ly, rx, ry;     // -512..511, y down positive
    uint16_t l2, r2;            // 0..1023
    int16_t gyro[3];            // rad/s x 1000: X pitch, Y yaw, Z roll
    int16_t accel[3];           // m/s^2 x 100: X right, Y up (+981 at rest), Z back
    uint32_t time_ms;           // when it arrived
} fh1_pad_t;
#ifndef FH1_FIRMWARE
enum {
    FH1_PAD_BTN_A = 1 << 0, FH1_PAD_BTN_B = 1 << 1, FH1_PAD_BTN_X = 1 << 2, FH1_PAD_BTN_Y = 1 << 3,
    FH1_PAD_BTN_L1 = 1 << 4, FH1_PAD_BTN_R1 = 1 << 5, FH1_PAD_BTN_L2 = 1 << 6, FH1_PAD_BTN_R2 = 1 << 7,
    FH1_PAD_BTN_L3 = 1 << 8, FH1_PAD_BTN_R3 = 1 << 9,
    FH1_PAD_BTN_SYSTEM = 1 << 10, FH1_PAD_BTN_SELECT = 1 << 11, FH1_PAD_BTN_START = 1 << 12, FH1_PAD_BTN_CAPTURE = 1 << 13,
    FH1_PAD_BTN_UP = 1 << 14, FH1_PAD_BTN_DOWN = 1 << 15, FH1_PAD_BTN_LEFT = 1 << 16, FH1_PAD_BTN_RIGHT = 1 << 17,
};
#endif
bool fh1_pad(int player, fh1_pad_t *out);                          // false: no controller for that player
bool fh1_pad_info(int player, char *name, size_t n, uint32_t *rgb, bool *motion);   // its name, colour (0xRRGGBB), motion
void fh1_pad_motion(bool on);
void fh1_pad_hold_keys(bool hold);

// ── Canvas (SDK 0.2) ──
// A picture to draw on, in the same units as windows (scaled to the screen): fill, rectangles, lines, filled
// circles and text, then show. Colours 0xRRGGBB. Freed with its window.
lv_obj_t *fh1_canvas(lv_obj_t *parent, int w, int h);
void fh1_canvas_fill(lv_obj_t *c, uint32_t rgb);
void fh1_canvas_rect(lv_obj_t *c, int x, int y, int w, int h, uint32_t rgb);
void fh1_canvas_line(lv_obj_t *c, int x0, int y0, int x1, int y1, int width, uint32_t rgb);
void fh1_canvas_circle(lv_obj_t *c, int x, int y, int r, uint32_t rgb);
void fh1_canvas_text(lv_obj_t *c, int x, int y, const char *text, uint32_t rgb);   // the interface's font
void fh1_canvas_show(lv_obj_t *c);
// SDK 0.3: a filled triangle, and how wide a text is (units) to place it.
void fh1_canvas_tri(lv_obj_t *c, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t rgb);
int fh1_canvas_text_width(lv_obj_t *c, const char *text);

// ── Users, settings and saved states (SDK 0.4) ──
// Apps need nothing to be per user: the system shows who plays when an app starts (their faces, over it), and
// the app's data folder belongs to player 1's user (every user keeps their own saves). These are for apps that
// want more, and they cost nothing to the ones that do not call them.
//
// Who plays: the user of a player's controller (player 0 is player 1, who also uses the menus). false: nobody
// (the FH1 has no users, or no one plays as that player).
bool fh1_user(int player, char *name, size_t n);
// Their face, animated (size 16 or 32 units), in a window; NULL when nobody plays as that player.
lv_obj_t *fh1_user_face(lv_obj_t *parent, int player, int size);
// A notification with player 1's face (a small box at the top right for 4 s).
void fh1_notify(const char *text);
// Settings: short texts by key (letters, digits, _ . -; up to 31), kept in the data folder. get: false if unset.
bool fh1_setting_get(const char *key, char *value, size_t n);
bool fh1_setting_set(const char *key, const char *value);       // up to 127 characters, no line breaks
// Saved states (a game's save slots): whole blobs by name (like the keys), checked when loaded.
int fh1_state_save(const char *name, const void *data, size_t len);   // 0, or -1
int fh1_state_load(const char *name, void *data, size_t max);        // its size, or -1: none, damaged, too big

#ifdef __cplusplus
}
#endif
