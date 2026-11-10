// Runs packaged apps: loads app.elf into PSRAM with the ELF loader, resolves it against the SDK's
// symbol table and calls its app_main. The app stays loaded while it has windows, timers or the full screen;
// a timer unloads it once its last window closes, or when it calls fh1_app_exit().
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include "fh1_sdk.h"
#include "apps_priv.h"
#include "fh1_ui.h"
#include "fh1_audio.h"
#include "fh1_input.h"
#include "fh1_gamepad.h"
#include "fh1_storage.h"
#include "esp_elf.h"
#include "fh1_users.h"
#include "esp_rom_crc.h"
#include "src/misc/lv_text_private.h"   // lv_text_get_width
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "fh1_app.h"
#include "fh1_board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "run";

#define MAX_TIMERS 8

static struct {
    bool loaded, exit_requested;
    fh1_manifest_t m;
    char data_dir[96];
    esp_elf_t elf;
    uint8_t *image;
    lv_timer_t *timers[MAX_TIMERS];
    size_t heap_before;
    bool motion, hold;                 // SDK 0.2: what the app asked of the gamepads (undone on unload)
} s_app;
static lv_timer_t *s_watch;

// SDK 0.5: the app's full screen, drawn by its frame() on a task of its own.
static struct {
    TaskHandle_t task;
    fh1_frame_fn frame;
    void (*closed)(void *user);
    void *user;
    uint32_t period_ms;
    volatile bool stop;                // fh1_screen_close, or the app is being unloaded
    volatile bool unloading;           // then closed() runs from unload(), before the app's code goes away
    bool closed_pending;
    uint16_t *fb, *bb;
    int w, h;
    char app_dir[64];
} s_scr;

// ── SDK functions that exist only for apps ──

uint32_t fh1_sdk_version(void) { return (FH1_SDK_MAJOR << 16) | FH1_SDK_MINOR; }
const char *fh1_app_id(void) { return s_app.loaded ? s_app.m.id : ""; }
// The app's data folder is player 1's user's now (SDK 0.8): a controller taken over by another user mid-game
// moves saves and settings to that user's folder at once (the app sees the change with fh1_user).
static void data_dir_now(char *out, size_t n) { fh1_data_dir(s_app.m.id, out, n); }

const char *fh1_app_data_dir(void)
{
    if (!s_app.loaded) return "";
    data_dir_now(s_app.data_dir, sizeof(s_app.data_dir));
    return s_app.data_dir;
}
const char *fh1_app_dir(void) { return s_app.loaded ? s_scr.app_dir : ""; }

const char *fh1_asset_dir(const char *id)
{
    static char path[2][80];
    static int k;
    struct stat st;
    if (!id || strchr(id, '/') || strstr(id, "..")) return NULL;
    k ^= 1;
    snprintf(path[k], sizeof(path[k]), FH1_SD_PATH "/assets/%s", id);
    if (stat(path[k], &st) == 0 && S_ISDIR(st.st_mode)) return path[k];
    snprintf(path[k], sizeof(path[k]), FH1_INT_PATH "/assets/%s", id);
    if (stat(path[k], &st) == 0 && S_ISDIR(st.st_mode)) return path[k];
    return NULL;
}
void fh1_app_exit(void) { s_app.exit_requested = true; }
uint32_t fh1_millis(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
void fh1_ui_label_set(lv_obj_t *label, const char *text) { lv_label_set_text(label, text); }

void fh1_log(const char *fmt, ...)
{
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ESP_LOGI("app", "[%s] %s", fh1_app_id(), buf);
}

fh1_win_t *fh1_app_window(const char *title, int w, int h)
{
    fh1_win_t *win = fh1_win_create(title, w, h);
    if (win) fh1_win_set_owner(win, &s_app);
    return win;
}

// ── Gamepads (SDK 0.2) ──

_Static_assert(sizeof(fh1_pad_t) == sizeof(fh1_pad_state_t), "fh1_pad_t is fh1_pad_state_t for apps");

bool fh1_pad(int player, fh1_pad_t *out)
{
    fh1_gamepad_info_t g;
    if (!out || !fh1_gamepad_of_player(player, &g)) return false;
    fh1_pad_state_t st;
    if (!fh1_gamepad_state(g.id, &st)) return false;
    memcpy(out, &st, sizeof(*out));
    return true;
}

bool fh1_pad_info(int player, char *name, size_t n, uint32_t *rgb, bool *motion)
{
    fh1_gamepad_info_t g;
    if (!fh1_gamepad_of_player(player, &g)) return false;
    if (name && n) snprintf(name, n, "%s", g.name);
    if (rgb) *rgb = g.rgb;
    if (motion) *motion = (g.caps & FH1_PAD_CAP_MOTION) != 0;
    return true;
}

void fh1_pad_motion(bool on)
{
    if (on == s_app.motion) return;
    s_app.motion = on;
    fh1_gamepad_want_motion(on);
}

void fh1_pad_hold_keys(bool hold)
{
    if (hold == s_app.hold) return;
    s_app.hold = hold;
    fh1_gamepad_mute_keys(hold);
}

// ── Canvas (SDK 0.2): RGB565 in PSRAM, drawn in units (each a s x s block of pixels) ──

typedef struct {
    uint16_t *px;
    int w, h, s;                       // pixels, and the scale
} canvas_t;

static void canvas_deleted(lv_event_t *e)
{
    canvas_t *c = lv_event_get_user_data(e);
    heap_caps_free(c->px);
    free(c);
}

lv_obj_t *fh1_canvas(lv_obj_t *parent, int w, int h)
{
    const int s = fh1_ui_scale();
    canvas_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->w = w * s;
    c->h = h * s;
    c->s = s;
    c->px = heap_caps_calloc((size_t)c->w * c->h, 2, MALLOC_CAP_SPIRAM);
    if (!c->px) {
        free(c);
        return NULL;
    }
    lv_obj_t *o = lv_canvas_create(parent);
    lv_canvas_set_buffer(o, c->px, c->w, c->h, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_user_data(o, c);
    lv_obj_add_event_cb(o, canvas_deleted, LV_EVENT_DELETE, c);
    return o;
}

static inline uint16_t c565(uint32_t rgb)
{
    return (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
}

// Pixels x0..x1, y0..y1 (clipped).
static void fill_px(canvas_t *c, int x0, int y0, int x1, int y1, uint16_t v)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > c->w - 1) x1 = c->w - 1;
    if (y1 > c->h - 1) y1 = c->h - 1;
    for (int y = y0; y <= y1; y++) {
        uint16_t *row = c->px + (size_t)y * c->w;
        for (int x = x0; x <= x1; x++) row[x] = v;
    }
}

void fh1_canvas_fill(lv_obj_t *o, uint32_t rgb)
{
    canvas_t *c = o ? lv_obj_get_user_data(o) : NULL;
    if (c) fill_px(c, 0, 0, c->w - 1, c->h - 1, c565(rgb));
}

void fh1_canvas_rect(lv_obj_t *o, int x, int y, int w, int h, uint32_t rgb)
{
    canvas_t *c = o ? lv_obj_get_user_data(o) : NULL;
    if (c && w > 0 && h > 0) fill_px(c, x * c->s, y * c->s, (x + w) * c->s - 1, (y + h) * c->s - 1, c565(rgb));
}

void fh1_canvas_line(lv_obj_t *o, int x0, int y0, int x1, int y1, int width, uint32_t rgb)
{
    canvas_t *c = o ? lv_obj_get_user_data(o) : NULL;
    if (!c) return;
    const uint16_t v = c565(rgb);
    const int s = c->s, half = (width < 1 ? 1 : width) * s / 2;
    // Bresenham in pixels, a square of the width at each step.
    x0 *= s, y0 *= s, x1 *= s, y1 *= s;
    const int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (int guard = 0; guard < 4096; guard++) {
        fill_px(c, x0 - half, y0 - half, x0 + half, y0 + half, v);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) err += dy, x0 += sx;
        if (e2 <= dx) err += dx, y0 += sy;
    }
}

void fh1_canvas_circle(lv_obj_t *o, int x, int y, int r, uint32_t rgb)
{
    canvas_t *c = o ? lv_obj_get_user_data(o) : NULL;
    if (!c || r <= 0) return;
    const uint16_t v = c565(rgb);
    const int s = c->s, cx = x * s, cy = y * s, R = r * s;
    for (int dy = -R; dy <= R; dy++) {
        const int dx = (int)sqrtf((float)(R * R - dy * dy));
        fill_px(c, cx - dx, cy + dy, cx + dx, cy + dy, v);
    }
}

void fh1_canvas_text(lv_obj_t *o, int x, int y, const char *text, uint32_t rgb)
{
    canvas_t *c = o ? lv_obj_get_user_data(o) : NULL;
    if (!c || !text) return;
    lv_layer_t layer;
    lv_canvas_init_layer(o, &layer);
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.color = lv_color_hex(rgb);
    d.font = lv_obj_get_style_text_font(o, LV_PART_MAIN);
    d.text = text;
    lv_area_t a = { x * c->s, y * c->s, c->w - 1, c->h - 1 };
    lv_draw_label(&layer, &d, &a);
    lv_canvas_finish_layer(o, &layer);
}

void fh1_canvas_show(lv_obj_t *o)
{
    if (o) lv_obj_invalidate(o);
}

// A filled triangle: each pixel row between its edges (in pixels, from the unit corners).
void fh1_canvas_tri(lv_obj_t *o, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t rgb)
{
    canvas_t *c = o ? lv_obj_get_user_data(o) : NULL;
    if (!c) return;
    const uint16_t v = c565(rgb);
    const int s = c->s;
    int X[3] = { x0 * s, x1 * s, x2 * s }, Y[3] = { y0 * s, y1 * s, y2 * s };
    // Sort by y.
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 2 - i; j++) {
            if (Y[j] > Y[j + 1]) {
                int t = Y[j]; Y[j] = Y[j + 1]; Y[j + 1] = t;
                t = X[j]; X[j] = X[j + 1]; X[j + 1] = t;
            }
        }
    }
    if (Y[2] == Y[0]) return;
    for (int y = Y[0] < 0 ? 0 : Y[0]; y <= Y[2] && y < c->h; y++) {
        // The long edge (0-2), and the short one this row is on (0-1 or 1-2).
        const float xa = X[0] + (float)(X[2] - X[0]) * (y - Y[0]) / (Y[2] - Y[0]);
        float xb;
        if (y < Y[1]) xb = Y[1] == Y[0] ? X[1] : X[0] + (float)(X[1] - X[0]) * (y - Y[0]) / (Y[1] - Y[0]);
        else xb = Y[2] == Y[1] ? X[1] : X[1] + (float)(X[2] - X[1]) * (y - Y[1]) / (Y[2] - Y[1]);
        const int l = (int)(xa < xb ? xa : xb), r = (int)(xa < xb ? xb : xa);
        fill_px(c, l, y, r, y, v);
    }
}

int fh1_canvas_text_width(lv_obj_t *o, const char *text)
{
    canvas_t *c = o ? lv_obj_get_user_data(o) : NULL;
    if (!c || !text) return 0;
    const lv_font_t *f = lv_obj_get_style_text_font(o, LV_PART_MAIN);
    lv_text_attributes_t at;
    memset(&at, 0, sizeof(at));        // no letter spacing
    return (int)lv_text_get_width(text, strlen(text), f, &at) / c->s;
}

typedef struct {
    void (*cb)(void *user);
    void *user;
} app_timer_t;

static void timer_tramp(lv_timer_t *t)
{
    app_timer_t *at = lv_timer_get_user_data(t);
    if (s_app.loaded && !s_app.exit_requested) at->cb(at->user);
}

int fh1_app_timer(uint32_t period_ms, void (*cb)(void *user), void *user)
{
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!s_app.timers[i]) {
            app_timer_t *at = malloc(sizeof(*at));
            if (!at) return -1;
            at->cb = cb;
            at->user = user;
            s_app.timers[i] = lv_timer_create(timer_tramp, period_ms, at);
            return i;
        }
    }
    return -1;
}

void fh1_app_timer_stop(int id)
{
    if (id < 0 || id >= MAX_TIMERS || !s_app.timers[id]) return;
    free(lv_timer_get_user_data(s_app.timers[id]));
    lv_timer_delete(s_app.timers[id]);
    s_app.timers[id] = NULL;
}

static bool any_timer(void)
{
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (s_app.timers[i]) return true;
    }
    return false;
}

// ── SDK 0.4: users, notifications, settings, saved states ──

bool fh1_user(int player, char *name, size_t n)
{
    fh1_user_t u;
    if (!fh1_users_player(player, &u)) {
        if (n) name[0] = 0;
        return false;
    }
    snprintf(name, n, "%s", u.name);
    return true;
}

lv_obj_t *fh1_user_face(lv_obj_t *parent, int player, int size)
{
    fh1_user_t u;
    if (!fh1_users_player(player, &u)) return NULL;
    return users_face_widget(parent, u.id, size >= 32 ? 32 : 16);
}

void fh1_notify(const char *text) { users_notify_player(0, text); }

// Names for settings and states: letters, digits, _ . - (no folders), up to 31.
static bool name_ok(const char *k)
{
    const size_t n = k ? strlen(k) : 0;
    if (!n || n > 31 || strstr(k, "..")) return false;
    for (size_t i = 0; i < n; i++) {
        const char c = k[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-')) return false;
    }
    return true;
}

// Settings: "key=value" lines in <data dir>/settings.txt.
bool fh1_setting_get(const char *key, char *value, size_t n)
{
    if (n) value[0] = 0;
    if (!s_app.loaded || !name_ok(key)) return false;
    char p[128], line[176], dir[96];
    data_dir_now(dir, sizeof(dir));
    snprintf(p, sizeof(p), "%s/settings.txt", dir);
    FILE *f = fopen(p, "r");
    if (!f) return false;
    const size_t kl = strlen(key);
    bool found = false;
    while (!found && fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, kl) != 0 || line[kl] != '=') continue;
        line[strcspn(line, "\r\n")] = 0;
        snprintf(value, n, "%s", line + kl + 1);
        found = true;
    }
    fclose(f);
    return found;
}

bool fh1_setting_set(const char *key, const char *value)
{
    if (!s_app.loaded || !name_ok(key) || !value || strlen(value) > 127 || strpbrk(value, "\r\n")) return false;
    char p[128], tmp[136], line[176], dir[96];
    data_dir_now(dir, sizeof(dir));
    snprintf(p, sizeof(p), "%s/settings.txt", dir);
    snprintf(tmp, sizeof(tmp), "%s.new", p);
    fh1_mkdirs(dir);
    FILE *in = fopen(p, "r"), *out = fopen(tmp, "w");
    if (!out) {
        if (in) fclose(in);
        return false;
    }
    const size_t kl = strlen(key);
    while (in && fgets(line, sizeof(line), in))
        if (!(strncmp(line, key, kl) == 0 && line[kl] == '=')) fputs(line, out);
    fprintf(out, "%s=%s\n", key, value);
    if (in) fclose(in);
    const bool ok = fclose(out) == 0;
    if (ok) {
        remove(p);
        rename(tmp, p);
    }
    fh1_int_info_changed();
    return ok;
}

// States: <data dir>/<name>.state = "FH1S" [length u32] [CRC-32 u32] [the data].
int fh1_state_save(const char *name, const void *data, size_t len)
{
    if (!s_app.loaded || !name_ok(name) || (!data && len)) return -1;
    char p[128], tmp[136], dir[96];
    data_dir_now(dir, sizeof(dir));
    snprintf(p, sizeof(p), "%s/%s.state", dir, name);
    snprintf(tmp, sizeof(tmp), "%s.new", p);
    fh1_mkdirs(dir);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    const uint32_t hdr[3] = { 0x53314846u, (uint32_t)len, esp_rom_crc32_le(0, data, len) };
    bool ok = fwrite(hdr, sizeof(hdr), 1, f) == 1 && (!len || fwrite(data, len, 1, f) == 1);
    ok = fclose(f) == 0 && ok;
    if (ok) {
        remove(p);
        ok = rename(tmp, p) == 0;
    }
    fh1_int_info_changed();
    return ok ? 0 : -1;
}

int fh1_state_load(const char *name, void *data, size_t max)
{
    if (!s_app.loaded || !name_ok(name)) return -1;
    char p[128], dir[96];
    data_dir_now(dir, sizeof(dir));
    snprintf(p, sizeof(p), "%s/%s.state", dir, name);
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    uint32_t hdr[3];
    int r = -1;
    if (fread(hdr, sizeof(hdr), 1, f) == 1 && hdr[0] == 0x53314846u && hdr[1] <= max &&
        (!hdr[1] || fread(data, hdr[1], 1, f) == 1) && esp_rom_crc32_le(0, data, hdr[1]) == hdr[2])
        r = (int)hdr[1];
    fclose(f);
    return r;
}

// The compiler's helper for float division (the S3's FPU has no divide): any app dividing floats needs it.
// ── Full screen (SDK 0.5) ──

static void screen_closed(void *user)
{
    if (s_app.loaded && s_scr.closed) s_scr.closed(user);
}

static void screen_task(void *arg)
{
    const size_t bytes = (size_t)s_scr.w * s_scr.h * 2;
    bool show_all = true;              // the first frame, and after an overlay drew over the screen
    uint32_t last = fh1_millis();
    TickType_t wake = xTaskGetTickCount();
    while (!s_scr.stop) {
        if (fh1_app_quit_requested()) break;
        if (fh1_app_should_yield()) {
            fh1_ui_lend_screen();
            show_all = true;
            wake = xTaskGetTickCount();
            continue;
        }
        const uint32_t now = fh1_millis();
        const int r = s_scr.frame(s_scr.bb, s_scr.w, s_scr.h, now - last, s_scr.user);
        last = now;
        if (r == FH1_FRAME_END || s_scr.stop) break;
        if (r == FH1_FRAME_SHOW || show_all) memcpy(s_scr.fb, s_scr.bb, bytes);
        show_all = false;
        // Block every frame, even when late, so core 1's idle task (and the watchdog) are fed.
        if (xTaskDelayUntil(&wake, pdMS_TO_TICKS(s_scr.period_ms)) == pdFALSE) {
            vTaskDelay(1);
            wake = xTaskGetTickCount();
        }
    }
    heap_caps_free(s_scr.bb);
    s_scr.bb = NULL;
    fh1_ui_pause(false, true);
    if (!s_scr.unloading) fh1_ui_async(screen_closed, s_scr.user);
    else s_scr.closed_pending = true;
    s_scr.task = NULL;
    vTaskDelete(NULL);
}

bool fh1_screen_open(uint32_t period_ms, fh1_frame_fn frame, void (*closed)(void *user), void *user)
{
    if (!s_app.loaded || s_scr.task || !frame) return false;
    s_scr.fb = fh1_board_framebuffer(&s_scr.w, &s_scr.h);
    if (!s_scr.fb) return false;
    s_scr.bb = heap_caps_calloc((size_t)s_scr.w * s_scr.h, 2, MALLOC_CAP_SPIRAM);
    if (!s_scr.bb) return false;
    s_scr.frame = frame;
    s_scr.closed = closed;
    s_scr.user = user;
    s_scr.period_ms = period_ms < 10 ? 10 : period_ms;
    s_scr.stop = s_scr.unloading = false;
    fh1_ui_pause(true, false);
    if (xTaskCreatePinnedToCore(screen_task, "app_screen", 8192, NULL, 4, &s_scr.task, 1) != pdPASS) {
        s_scr.task = NULL;
        heap_caps_free(s_scr.bb);
        s_scr.bb = NULL;
        fh1_ui_pause(false, true);
        return false;
    }
    return true;
}

void fh1_screen_close(void) { if (s_scr.task) s_scr.stop = true; }

// ── Development commands (SDK 0.6): a few lines queued by the console, taken by the app ──

#define DEV_QUEUE 6
static portMUX_TYPE s_dev_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_dev[DEV_QUEUE][64];
static int s_dev_head, s_dev_n;

bool run_dev_push(const char *text)
{
    bool ok = false;
    portENTER_CRITICAL(&s_dev_mux);
    if (s_app.loaded && s_dev_n < DEV_QUEUE) {
        strlcpy(s_dev[(s_dev_head + s_dev_n) % DEV_QUEUE], text, sizeof(s_dev[0]));
        s_dev_n++;
        ok = true;
    }
    portEXIT_CRITICAL(&s_dev_mux);
    return ok;
}

bool fh1_dev_command(char *out, size_t n)
{
    bool ok = false;
    portENTER_CRITICAL(&s_dev_mux);
    if (s_dev_n) {
        strlcpy(out, s_dev[s_dev_head], n);
        s_dev_head = (s_dev_head + 1) % DEV_QUEUE;
        s_dev_n--;
        ok = true;
    }
    portEXIT_CRITICAL(&s_dev_mux);
    return ok;
}

const uint8_t *fh1_font8_glyph(uint32_t cp);           // fh1_ui: unscii-8, ASCII + Latin-1

int fh1_px_text(uint16_t *px, int w, int h, int x, int y, const char *text, uint32_t rgb, int scale)
{
    if (scale < 1) scale = 1;
    const uint16_t c = c565(rgb);
    const unsigned char *p = (const unsigned char *)text;
    while (*p && x < w) {
        uint32_t cp = *p++;
        if ((cp & 0xE0) == 0xC0 && *p) cp = ((cp & 0x1F) << 6) | (*p++ & 0x3F);
        const uint8_t *g = fh1_font8_glyph(cp);
        for (int gy = 0; gy < 8; gy++)
            for (int gx = 0; gx < 8; gx++) {
                if (!(g[gy] & (0x80 >> gx))) continue;
                for (int sy = 0; sy < scale; sy++) {
                    const int yy = y + gy * scale + sy;
                    if (yy < 0 || yy >= h) continue;
                    for (int sx = 0; sx < scale; sx++) {
                        const int xx = x + gx * scale + sx;
                        if (xx >= 0 && xx < w) px[yy * w + xx] = c;
                    }
                }
            }
        x += 8 * scale;
    }
    return x;
}

// SDK 0.8: player 1's controller's button style, and a button's icon in it (fh1_ui draws both).
int fh1_pad_style(void) { return fh1_gamepad_style(0); }

extern float __divsf3(float a, float b);

// ── The exported symbol table: SDK 0.1 to 0.4 ──

static const struct esp_elfsym s_sdk_syms[] = {
    ESP_ELFSYM_EXPORT(fh1_sdk_version),
    ESP_ELFSYM_EXPORT(fh1_app_id),
    ESP_ELFSYM_EXPORT(fh1_app_data_dir),
    ESP_ELFSYM_EXPORT(fh1_user),
    ESP_ELFSYM_EXPORT(fh1_user_face),
    ESP_ELFSYM_EXPORT(fh1_notify),
    ESP_ELFSYM_EXPORT(fh1_setting_get),
    ESP_ELFSYM_EXPORT(fh1_setting_set),
    ESP_ELFSYM_EXPORT(fh1_state_save),
    ESP_ELFSYM_EXPORT(fh1_state_load),
    ESP_ELFSYM_EXPORT(fh1_app_exit),
    ESP_ELFSYM_EXPORT(fh1_app_dir),
    ESP_ELFSYM_EXPORT(fh1_screen_open),
    ESP_ELFSYM_EXPORT(fh1_screen_close),
    ESP_ELFSYM_EXPORT(fh1_px_text),
    ESP_ELFSYM_EXPORT(fh1_dev_command),
    ESP_ELFSYM_EXPORT(fh1_asset_dir),
    ESP_ELFSYM_EXPORT(fh1_sfx_play),
    ESP_ELFSYM_EXPORT(fh1_sfx_stop),
    ESP_ELFSYM_EXPORT(fh1_sfx_stop_all),
    ESP_ELFSYM_EXPORT(fh1_sfx_playing),
    ESP_ELFSYM_EXPORT(fh1_pad_style),
    ESP_ELFSYM_EXPORT(fh1_px_key),
    ESP_ELFSYM_EXPORT(fh1_millis),
    ESP_ELFSYM_EXPORT(fh1_app_timer),
    ESP_ELFSYM_EXPORT(fh1_app_timer_stop),
    ESP_ELFSYM_EXPORT(fh1_log),
    ESP_ELFSYM_EXPORT(fh1_app_window),
    ESP_ELFSYM_EXPORT(fh1_win_body),
    ESP_ELFSYM_EXPORT(fh1_win_set_title),
    ESP_ELFSYM_EXPORT(fh1_win_close),
    ESP_ELFSYM_EXPORT(fh1_win_on_key),
    ESP_ELFSYM_EXPORT(fh1_win_on_close),
    ESP_ELFSYM_EXPORT(fh1_ui_label),
    ESP_ELFSYM_EXPORT(fh1_ui_label_set),
    ESP_ELFSYM_EXPORT(fh1_ui_button),
    ESP_ELFSYM_EXPORT(fh1_ui_button_set_text),
    ESP_ELFSYM_EXPORT(fh1_ui_dim),
    ESP_ELFSYM_EXPORT(fh1_ui_message),
    ESP_ELFSYM_EXPORT(fh1_ui_confirm),
    ESP_ELFSYM_EXPORT(fh1_input_is_down),
    ESP_ELFSYM_EXPORT(fh1_caps),
    ESP_ELFSYM_EXPORT(fh1_cap_name),
    ESP_ELFSYM_EXPORT(fh1_audio_open),
    ESP_ELFSYM_EXPORT(fh1_audio_write),
    ESP_ELFSYM_EXPORT(fh1_audio_set_volume),
    ESP_ELFSYM_EXPORT(fh1_audio_available),
    ESP_ELFSYM_EXPORT(fh1_audio_close),
    // SDK 0.2
    ESP_ELFSYM_EXPORT(fh1_pad),
    ESP_ELFSYM_EXPORT(fh1_pad_info),
    ESP_ELFSYM_EXPORT(fh1_pad_motion),
    ESP_ELFSYM_EXPORT(fh1_pad_hold_keys),
    ESP_ELFSYM_EXPORT(fh1_canvas),
    ESP_ELFSYM_EXPORT(fh1_canvas_fill),
    ESP_ELFSYM_EXPORT(fh1_canvas_rect),
    ESP_ELFSYM_EXPORT(fh1_canvas_line),
    ESP_ELFSYM_EXPORT(fh1_canvas_circle),
    ESP_ELFSYM_EXPORT(fh1_canvas_text),
    ESP_ELFSYM_EXPORT(fh1_canvas_show),
    ESP_ELFSYM_EXPORT(fh1_canvas_tri),
    ESP_ELFSYM_EXPORT(fh1_canvas_text_width),
    ESP_ELFSYM_EXPORT(atan2f), ESP_ELFSYM_EXPORT(fabsf), ESP_ELFSYM_EXPORT(asinf),
    ESP_ELFSYM_EXPORT(__divsf3),
    // The C library apps may use, beyond the ELF loader's own small table.
    ESP_ELFSYM_EXPORT(snprintf), ESP_ELFSYM_EXPORT(vsnprintf), ESP_ELFSYM_EXPORT(sscanf),
    ESP_ELFSYM_EXPORT(fopen), ESP_ELFSYM_EXPORT(fclose), ESP_ELFSYM_EXPORT(fread), ESP_ELFSYM_EXPORT(fgets),
    ESP_ELFSYM_EXPORT(fseek), ESP_ELFSYM_EXPORT(ftell), ESP_ELFSYM_EXPORT(fflush), ESP_ELFSYM_EXPORT(remove),
    ESP_ELFSYM_EXPORT(rename),
    ESP_ELFSYM_EXPORT(memmove), ESP_ELFSYM_EXPORT(memcmp), ESP_ELFSYM_EXPORT(strncmp), ESP_ELFSYM_EXPORT(strncpy),
    ESP_ELFSYM_EXPORT(strlcpy), ESP_ELFSYM_EXPORT(strlcat), ESP_ELFSYM_EXPORT(strstr),
    ESP_ELFSYM_EXPORT(atoi), ESP_ELFSYM_EXPORT(strtoul), ESP_ELFSYM_EXPORT(qsort), ESP_ELFSYM_EXPORT(rand),
    ESP_ELFSYM_EXPORT(srand), ESP_ELFSYM_EXPORT(time),
    ESP_ELFSYM_EXPORT(sinf), ESP_ELFSYM_EXPORT(cosf), ESP_ELFSYM_EXPORT(sqrtf), ESP_ELFSYM_EXPORT(floorf),
    ESP_ELFSYM_END
};

// ── Load and unload ──

// The full screen's task runs the app's code: it must end before the app is unloaded. Asking it to stop
// never waits here (it may be lending the screen to the UI task, which runs this).
static bool screen_stopped(void)
{
    if (!s_scr.task) return true;
    s_scr.unloading = s_scr.stop = true;
    return false;
}

static void unload(void)
{
    if (!s_app.loaded || !screen_stopped()) return;
    // Closed by the system (a stop, another app): the app still frees what it holds (its art, its sounds), from
    // here, while its code is loaded. Without this a stopped game kept megabytes of PSRAM.
    if (s_scr.closed_pending) {
        s_scr.closed_pending = false;
        sfx_shutdown();
        if (s_scr.closed) s_scr.closed(s_scr.user);
    }
    fh1_win_close_owned(&s_app);
    for (int i = 0; i < MAX_TIMERS; i++) fh1_app_timer_stop(i);
    fh1_pad_motion(false);             // what it asked of the gamepads, undone
    fh1_pad_hold_keys(false);
    s_dev_n = 0;
    sfx_shutdown();                    // the mixer reads the app's samples: stopped before they go
    esp_elf_deinit(&s_app.elf);
    heap_caps_free(s_app.image);
    s_app.image = NULL;
    s_app.loaded = false;
    s_app.exit_requested = false;
    size_t after = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    ESP_LOGI(TAG, "%s unloaded; free heap %u (before load %u)", s_app.m.id, (unsigned)after, (unsigned)s_app.heap_before);
}

static void watch(lv_timer_t *t)
{
    if (!s_app.loaded) return;
    if (s_app.exit_requested || (fh1_win_count_owned(&s_app) == 0 && !any_timer() && !s_scr.task)) unload();
}

void run_init(void)
{
    esp_elf_register_symbol(s_sdk_syms);
    s_watch = lv_timer_create(watch, 300, NULL);
}

const char *run_current(void) { return s_app.loaded ? s_app.m.id : NULL; }

void run_stop(void)
{
    if (s_app.loaded) s_app.exit_requested = true;
    watch(NULL);
}

esp_err_t run_start(const fh1_manifest_t *m, char *err, size_t n)
{
    if (s_app.loaded) {
        if (strcmp(s_app.m.id, m->id) == 0) {
            snprintf(err, n, "already running");
            return ESP_ERR_INVALID_STATE;
        }
        unload();     // one packaged app at a time
        if (s_app.loaded) {
            snprintf(err, n, "%s is closing; try again", s_app.m.id);
            return ESP_ERR_INVALID_STATE;
        }
    }
    if (m->assets) {
        snprintf(err, n, "an asset package: apps use it, it does not run");
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < m->nuses; i++)
        if (!fh1_asset_dir(m->uses[i])) {
            snprintf(err, n, "missing assets: %s (install its package)", m->uses[i]);
            return ESP_ERR_NOT_FOUND;
        }
    char path[96];
    snprintf(path, sizeof(path), FH1_APPS_DIR "/%s/app.elf", m->id);
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        snprintf(err, n, "app.elf missing");
        return ESP_ERR_NOT_FOUND;
    }
    s_app.heap_before = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    s_app.image = heap_caps_malloc(st.st_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    FILE *f = fopen(path, "rb");
    bool ok = s_app.image && f && fread(s_app.image, 1, st.st_size, f) == (size_t)st.st_size;
    if (f) fclose(f);
    if (!ok) {
        heap_caps_free(s_app.image);
        s_app.image = NULL;
        snprintf(err, n, "could not read app.elf");
        return ESP_FAIL;
    }
    esp_elf_init(&s_app.elf);
    int r = esp_elf_relocate(&s_app.elf, s_app.image, st.st_size);
    if (r != 0) {
        esp_elf_deinit(&s_app.elf);
        heap_caps_free(s_app.image);
        s_app.image = NULL;
        snprintf(err, n, "could not load (error %d); see the log", r);
        return ESP_FAIL;
    }
    s_app.m = *m;
    fh1_data_dir(m->id, s_app.data_dir, sizeof(s_app.data_dir));      // player 1's user's folder
    snprintf(s_scr.app_dir, sizeof(s_scr.app_dir), FH1_APPS_DIR "/%s", m->id);
    fh1_mkdirs(s_app.data_dir);
    s_app.loaded = true;
    s_app.exit_requested = false;
    ESP_LOGI(TAG, "starting %s %s (%u bytes)", m->id, m->version, (unsigned)st.st_size);
    char *argv[] = { s_app.m.id, s_app.data_dir };
    r = esp_elf_request(&s_app.elf, 0, 2, argv);
    ESP_LOGI(TAG, "%s: app_main returned %d, %u window(s)", m->id, r, (unsigned)fh1_win_count_owned(&s_app));
    watch(NULL);
    return ESP_OK;
}
