// The launcher, the built-in apps (Devices, Storage, Installer, System) and the console hooks.
// Everything here runs in the UI task, except the two console hooks, which hand over to it.
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <strings.h>
#include <unistd.h>
#include "fh1_apps.h"
#include "apps_priv.h"
#include "fh1_player.h"
#include "fh1_gamepad.h"
#include "fh1_users.h"
#include "fh1_mem.h"
#include "fh1_avatar.h"
#include "fh1_bridge.h"
#include "fh1_ui.h"
#include "fh1_board.h"
#include "fh1_devices.h"
#include "fh1_storage.h"
#include "fh1_input.h"
#include "fh1_devconsole.h"
#include "fh1_sdk.h"
#include "fh1_ota.h"
#include "fh1_gb.h"
#include "psa/crypto.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "esp_idf_version.h"
#include "fh1_audio.h"
#include <math.h>
#include <time.h>
#include "fh1_settings.h"
#include "fh1_i18n.h"
#include "fh1_events.h"
#include "fh1_app.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "apps";

#define MAX_INSTALLED 32
#define MAX_PACKAGES 24

typedef struct {
    char path[128];
    char file[48];
    bool on_sd;
    bool valid;
    fh1_manifest_t m;
    char err[64];
} package_t;

static fh1_manifest_t s_installed[MAX_INSTALLED];
static int s_ninstalled;
static package_t s_pkgs[MAX_PACKAGES];
static int s_npkgs;
static SemaphoreHandle_t s_lock;       // guards the two lists for the console's reads

static fh1_win_t *s_launcher, *s_devices, *s_storage, *s_installer, *s_system;

static void refresh_all(void *arg);

// ── Registry: installed apps are the folders in /int/apps; packages are the .fpk files staged ──

static void scan_installed(void)
{
    fh1_manifest_t *list = calloc(MAX_INSTALLED, sizeof(*list));
    int n = 0;
    DIR *d = opendir(FH1_APPS_DIR);
    struct dirent *e;
    while (d && list && (e = readdir(d)) != NULL && n < MAX_INSTALLED) {
        if (e->d_name[0] == '.') continue;
        char p[128];
        snprintf(p, sizeof(p), FH1_APPS_DIR "/%.40s/manifest.json", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        char buf[1024];
        size_t len = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[len] = 0;
        char err[64];
        if (pkg_manifest_parse(buf, &list[n], err, sizeof(err)) == ESP_OK) n++;
        else ESP_LOGW(TAG, "%s: %s", e->d_name, err);
    }
    if (d) closedir(d);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (list) memcpy(s_installed, list, n * sizeof(*list));
    s_ninstalled = n;
    xSemaphoreGive(s_lock);
    free(list);
}

static void scan_dir(const char *dir, bool sd, package_t *list, int *n)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL && *n < MAX_PACKAGES) {
        size_t l = strlen(e->d_name);
        if (l < 5 || strcasecmp(e->d_name + l - 4, ".fpk") != 0) continue;
        package_t *p = &list[(*n)++];
        memset(p, 0, sizeof(*p));
        snprintf(p->path, sizeof(p->path), "%s/%.60s", dir, e->d_name);
        strlcpy(p->file, e->d_name, sizeof(p->file));
        p->on_sd = sd;
        p->valid = pkg_peek(p->path, &p->m, p->err, sizeof(p->err)) == ESP_OK;
    }
    if (d) closedir(d);
}

static void scan_packages(void)
{
    package_t *list = calloc(MAX_PACKAGES, sizeof(*list));
    if (!list) return;
    int n = 0;
    scan_dir(FH1_INT_PACKAGES, false, list, &n);
    fh1_sd_info_t si;
    fh1_sd_info(&si);
    if (si.mounted) scan_dir(FH1_SD_PACKAGES, true, list, &n);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_pkgs, list, n * sizeof(*list));
    s_npkgs = n;
    xSemaphoreGive(s_lock);
    free(list);
}

static void caps_text(uint32_t caps, char *out, size_t n)
{
    out[0] = 0;
    for (int i = 0; i < FH1_CAP_COUNT; i++) {
        if (caps & (1u << i)) {
            if (out[0]) strlcat(out, " ", n);
            strlcat(out, fh1_cap_name(1u << i), n);
        }
    }
}

// ── Starting apps ──

typedef struct {
    const char *id, *name;
    void (*open)(void);
    int icon;                  // fh1_ui_icon_t before the name in the menu, or -1
} builtin_t;

static void open_devices(void);
static void open_storage(void);
static void open_installer(void);
static void open_about(void);
static void themes_open(void);


// "GameBoy" in the pixel font, then "Color" in another typeface, each letter its own colour, hopping. The
// same text is the app's menu entry and its windows' title.
const fh1_ui_span_t s_gbc_name[] = {
    { "GameBoy ", 0, NULL, false },
    { "C", 0xe4002b, &lv_font_montserrat_10, true }, { "o", 0x7b2fbe, &lv_font_montserrat_10, true },
    { "l", 0x009a44, &lv_font_montserrat_10, true }, { "o", 0xf5a800, &lv_font_montserrat_10, true },
    { "r", 0x0057b8, &lv_font_montserrat_10, true },
};
#define GBC_NAME_PARTS 6

// Built-in apps shown in the APPS menu, before the installed ones.
static const builtin_t s_user_builtins[] = {
    { "sys.gameboy", "GameBoy Color", games_open_gameboy, FH1_ICON_MENU_GAMEBOY },
    { "sys.nes", "NES", games_open_nes, FH1_ICON_MENU_NES },
    { "sys.sms", "Master System", games_open_sms, FH1_ICON_MENU_SMS },
    { "sys.music", "Music", music_open, FH1_ICON_MENU_MUSIC },
    { "sys.themes", "Themes", themes_open, FH1_ICON_MENU_THEMES },
};
#define NUSER (sizeof(s_user_builtins) / sizeof(s_user_builtins[0]))

static const builtin_t s_builtins[] = {
    { "sys.installer", "Installer", open_installer, FH1_ICON_MENU_INSTALL },
    { "sys.storage", "Storage", open_storage, FH1_ICON_MENU_STORAGE },
    { "sys.devices", "Devices", open_devices, FH1_ICON_MENU_DEVICES },
    { "sys.io", "Inputs and outputs", io_open, FH1_ICON_MENU_IO },
    { "sys.bridges", "Bridges", bridges_open, FH1_ICON_MENU_BRIDGE },
    { "sys.themes", "Themes", themes_open, FH1_ICON_MENU_THEMES },        // an app, and in SYSTEM too
    { "sys.about", "About", open_about, FH1_ICON_MENU_ABOUT },
};
#define NBUILTIN (sizeof(s_builtins) / sizeof(s_builtins[0]))

static esp_err_t start_app(const char *id, char *err, size_t n)
{
    for (size_t i = 0; i < NUSER; i++) {
        if (strcmp(s_user_builtins[i].id, id) == 0) {
            s_user_builtins[i].open();
            return ESP_OK;
        }
    }
    for (size_t i = 0; i < NBUILTIN; i++) {
        if (strcmp(s_builtins[i].id, id) == 0) {
            s_builtins[i].open();
            return ESP_OK;
        }
    }
    for (int i = 0; i < s_ninstalled; i++) {
        fh1_manifest_t *m = &s_installed[i];
        if (strcmp(m->id, id) != 0) continue;
        uint32_t missing = m->needs & ~fh1_caps();
        if (missing) {
            char c[80];
            caps_text(missing, c, sizeof(c));
            snprintf(err, n, T("%s needs %s"), m->name, c);
            return ESP_ERR_NOT_SUPPORTED;
        }
        return run_start(m, err, n);
    }
    snprintf(err, n, "no app %s", id);
    return ESP_ERR_NOT_FOUND;
}

enum { MENU_APPS, MENU_USERS, MENU_SYSTEM, MENU_COUNT };
static const char *const s_menu_titles[MENU_COUNT] = { "Apps", "Users", "System" };
static const int s_menu_icons[MENU_COUNT] = { FH1_ICON_MENU_APPS, FH1_ICON_MENU_USERS, FH1_ICON_MENU_SYSTEM };
static int s_menu;

static void launch_cb(void *user)
{
    char err[96];
    if (start_app(user, err, sizeof(err)) != ESP_OK) fh1_ui_message(T("Can't start"), err);
    else if (s_menu == MENU_APPS) users_announce_players();       // who plays: their face, over the app
}

// ── Launcher: top-level menus side by side, LEFT and RIGHT switch between them ──


static void launcher_fill(void)
{
    if (!s_launcher || !fh1_win_is_open(s_launcher)) return;
    int focus = fh1_ui_focused_index(s_launcher);
    fh1_win_clear(s_launcher);
    fh1_win_set_title(s_launcher, T(s_menu_titles[s_menu]));
    if (s_menu == MENU_USERS) {
        users_menu_fill(s_launcher);          // apps_users.c
    } else if (s_menu == MENU_SYSTEM) {
        for (size_t i = 0; i < NBUILTIN; i++) {
            lv_obj_t *b = fh1_ui_button(s_launcher, NULL, T(s_builtins[i].name), launch_cb, (void *)s_builtins[i].id);
            if (s_builtins[i].icon >= 0) fh1_ui_button_lead_icon(b, (fh1_ui_icon_t)s_builtins[i].icon);
        }
    } else {
        for (size_t i = 0; i < NUSER; i++) {
            lv_obj_t *b;
            if (strcmp(s_user_builtins[i].id, "sys.gameboy") == 0) {
                b = fh1_ui_button_spans(s_launcher, NULL, s_gbc_name, GBC_NAME_PARTS, launch_cb, (void *)s_user_builtins[i].id);
            } else {
                b = fh1_ui_button(s_launcher, NULL, T(s_user_builtins[i].name), launch_cb, (void *)s_user_builtins[i].id);
            }
            if (s_user_builtins[i].icon >= 0) fh1_ui_button_lead_icon(b, (fh1_ui_icon_t)s_user_builtins[i].icon);
        }
        for (int i = 0; i < s_ninstalled; i++) {
            fh1_manifest_t *m = &s_installed[i];
            uint32_t missing = m->needs & ~fh1_caps();
            char t[96], c[64];
            if (missing) {
                caps_text(missing, c, sizeof(c));
                char nd[48];
                snprintf(nd, sizeof(nd), T("needs %s"), c);
                snprintf(t, sizeof(t), "%s (%s)", m->name, nd);
            } else {
                snprintf(t, sizeof(t), "%s%s", m->name, run_current() && strcmp(run_current(), m->id) == 0 ? " *" : "");
            }
            lv_obj_t *b = fh1_ui_button(s_launcher, NULL, t, launch_cb, m->id);
            fh1_ui_button_lead_icon(b, FH1_ICON_MENU_PACKAGE);
            if (missing) fh1_ui_dim(b, true);
        }
    }
    fh1_ui_focus_index(s_launcher, focus < 0 ? 0 : focus);
}

static bool launcher_key(fh1_win_t *w, int key, void *u)
{
    if (key != FH1_KEY_LEFT && key != FH1_KEY_RIGHT) return false;
    int next = s_menu + (key == FH1_KEY_RIGHT ? 1 : -1);
    if (next >= 0 && next < MENU_COUNT) {
        s_menu = next;
        fh1_win_clear(s_launcher);      // a new menu starts with the cursor on its first item
        launcher_fill();
        fh1_ui_focus_index(s_launcher, 0);
    }
    return true;
}

static void launcher_closed(fh1_win_t *w, void *u) { s_launcher = NULL; }

// Users changed: the Users menu shows them.
static void users_changed_menu(void)
{
    if (s_menu == MENU_USERS) launcher_fill();
}

// The tabs show the launcher's menu only while the launcher has focus.
static void tabs_tick(lv_timer_t *t)
{
    static int shown = -2;
    int cur = (s_launcher && fh1_win_top() == s_launcher) ? s_menu : -1;
    if (cur != shown) {
        const char *titles[MENU_COUNT];
        for (int i = 0; i < MENU_COUNT; i++) titles[i] = T(s_menu_titles[i]);
        fh1_ui_set_tabs(titles, s_menu_icons, MENU_COUNT, cur);
    }
    shown = cur;
}

static void open_launcher(void *arg)
{
    if (s_launcher && fh1_win_is_open(s_launcher)) {
        fh1_win_focus(s_launcher);
        return;
    }
    s_launcher = fh1_win_create(T("Apps"), 175, 150);
    const fh1_hint_t hints[] = { { FH1_HINT_LEFTRIGHT, T("Menus") }, { FH1_KEY_A, T("Open") } };
    fh1_win_set_hints(s_launcher, hints, 2);
    fh1_win_on_key(s_launcher, launcher_key, NULL);
    fh1_win_on_close(s_launcher, launcher_closed, NULL);
    launcher_fill();
}

// ── Devices: by kind (Video, Sound, Network, Clock, Controls). A kind opens one window with a view per
// device, side by side (Left/Right): how it is doing (read only), on/off, probe again, and its own settings.
// Storage has its own window. ──

static void settings_labels(void);
static void mode_cb(void *u);
static void theme_cb(void *u);
static void lang_cb(void *u);
static void fps_cb(void *u);
static void sounds_cb(void *u);
static void open_wifi(void *u);
static void wifi_forget_cb(void *u);
static lv_obj_t *s_btn_mode, *s_btn_theme, *s_btn_lang, *s_btn_fps, *s_btn_snd;

typedef struct {
    const char *name;
    const char *cls[3];          // device classes (prefixes) of this kind
    bool clock;                  // also the system time, as its first view
} dev_kind_t;

static const dev_kind_t s_kinds[] = {
    { "Video", { "display", "input.touch" }, false },
    { "Sound", { "audio" }, false },
    { "Network", { "net" }, false },
    { "Clock", { "rtc" }, true },
    { "Controls", { "input.buttons", "link.bridge" }, false },
};
#define NKINDS (sizeof(s_kinds) / sizeof(s_kinds[0]))

static bool in_kind(const dev_kind_t *k, const fh1_device_t *d)
{
    for (int i = 0; i < 3 && k->cls[i]; i++) {
        if (strncmp(d->cls, k->cls[i], strlen(k->cls[i])) == 0) return true;
    }
    return false;
}

static int kind_count(const dev_kind_t *k)
{
    int n = 0;
    for (size_t i = 0; i < fh1_dev_count(); i++) n += in_kind(k, fh1_dev_at(i));
    return n;
}

// A device's view title, by what it does.
static const char *view_name(const fh1_device_t *d)
{
    static const struct { const char *cls, *name; } names[] = {
        { "display.backlight", "Backlight" }, { "display", "Screen" }, { "input.touch", "Touch" },
        { "audio.out", "Sound out" }, { "net.wifi", "Wi-Fi" }, { "rtc", "RTC chip" }, { "input.buttons", "Buttons" },
        { "link.bridge", "Bluetooth" }, { "link.bridge.wifi", "Wi-Fi link" },
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(d->cls, names[i].cls) == 0) return T(names[i].name);
    }
    return d->id;
}

typedef struct {
    fh1_device_t *d;
    lv_obj_t *state, *drv, *info, *power, *onoff;
} dev_view_t;

static struct {
    fh1_win_t *win;
    const dev_kind_t *kind;
    int n;
    dev_view_t v[8];
    lv_obj_t *net_name, *net_ip, *net_signal;     // Network
    lv_obj_t *time_now, *time_zone, *time_src;   // Clock
    lv_obj_t *chan;                               // Sound
    lv_timer_t *timer;
} s_kw;

static fh1_win_t *s_kinds_win;

static void dev_toggle(void *user)
{
    fh1_device_t *d = user;
    if (d->essential) {
        fh1_ui_message(T("Devices"), T("It cannot be switched off."));
        return;
    }
    fh1_dev_set_enabled(d->id, d->state == FH1_DEV_DISABLED);
}

static void dev_probe(void *user)
{
    fh1_device_t *d = user;
    if (d->state != FH1_DEV_DISABLED) fh1_dev_reprobe(d->id);
}

static void vol_slide(int v, bool done, void *u)
{
    if (done) fh1_audio_set_master_volume(v);
    else fh1_audio_preview_volume(v);
}

static void bl_slide(int v, bool done, void *u)
{
    if (done) fh1_board_backlight(v);
    else fh1_board_backlight_preview(v);
}

static const char *const s_channel_names[FH1_AUDIO_CHANNEL_MODES] = { "stereo", "mono", "left only", "right only" };

static void chan_cb(void *u)
{
    fh1_audio_set_channels((fh1_audio_channels_t)((fh1_audio_channels() + 1) % FH1_AUDIO_CHANNEL_MODES));
}

// A short test: A4 on the left, then E5 on the right, so each channel can be checked by ear.
static volatile bool s_tone_busy;

static void tone_task(void *arg)
{
    const int rate = 32000, n = rate * 2 / 5;
    int16_t buf[2 * 128];
    fh1_audio_t *a = fh1_audio_open(rate);
    for (int ch = 0; ch < 2; ch++) {
        const float f = ch ? 659.25f : 440.0f;
        for (int i = 0; i < n; i += 128) {
            for (int k = 0; k < 128; k++) {
                int16_t v = (int16_t)(26000.0f * sinf(2.0f * 3.14159265f * f * (i + k) / rate));   // ~80 % of full scale
                buf[2 * k] = ch ? 0 : v;
                buf[2 * k + 1] = ch ? v : 0;
            }
            fh1_audio_write_stereo(a, buf, 128);
        }
    }
    fh1_audio_close(a);
    s_tone_busy = false;
    vTaskDelete(NULL);
}

// Dev: 8 s of 440 Hz left and 660 Hz right together at 48 kHz, to measure a sound path (dev console "tone long").
static void tone_long_task(void *arg)
{
    const int rate = 48000, n = rate * 8;
    int16_t buf[2 * 240];
    fh1_audio_t *a = fh1_audio_open(rate);
    // Running phases (sinf of a growing angle was too slow to keep up with 48 kHz).
    const float tau = 2.0f * 3.14159265f, dl = tau * 440.0f / rate, dr = tau * 660.0f / rate;
    float pl = 0, pr = 0;
    for (int i = 0; i < n; i += 240) {
        for (int k = 0; k < 240; k++) {
            buf[2 * k] = (int16_t)(12000.0f * sinf(pl));
            buf[2 * k + 1] = (int16_t)(12000.0f * sinf(pr));
            if ((pl += dl) > tau) pl -= tau;
            if ((pr += dr) > tau) pr -= tau;
        }
        fh1_audio_write_stereo(a, buf, 240);
    }
    fh1_audio_close(a);
    s_tone_busy = false;
    vTaskDelete(NULL);
}

static void tone_long(void)
{
    if (s_tone_busy || !fh1_audio_available()) return;
    s_tone_busy = true;
    if (xTaskCreate(tone_long_task, "tone_long", 4096, NULL, 3, NULL) != pdPASS) s_tone_busy = false;
}

static void tone_cb(void *u)
{
    if (!fh1_audio_available()) {
        fh1_ui_message(T("Sound"), T("No sound device is on."));
        return;
    }
    if (s_tone_busy) return;               // one at a time
    s_tone_busy = true;
    if (xTaskCreate(tone_task, "tone", 4096, NULL, 3, NULL) != pdPASS) s_tone_busy = false;
}

// The readings, every half second while the window is open.
static void kind_update(lv_timer_t *t)
{
    if (!s_kw.win) return;
    char b[96];
    for (int i = 0; i < s_kw.n; i++) {
        dev_view_t *v = &s_kw.v[i];
        fh1_device_t *d = v->d;
        snprintf(b, sizeof(b), "%s%s", T(fh1_dev_state_name(d->state)), d->essential ? T(" (fixed)") : "");
        fh1_ui_value_set(v->state, b);
        fh1_ui_value_set(v->drv, d->drv->name);
        fh1_ui_value_set(v->info, d->state == FH1_DEV_READY ? d->info : T(d->reason));
        snprintf(b, sizeof(b), "%u mA", d->ma);
        fh1_ui_value_set(v->power, b);
        if (v->onoff) {
            snprintf(b, sizeof(b), T("Switched on: %s"), d->state == FH1_DEV_DISABLED ? T("no") : T("yes"));
            fh1_ui_button_set_text(v->onoff, b);
        }
    }
    if (s_kw.net_name) {
        char ssid[33] = "", ip[16] = "";
        int rssi = 0;
        bool up = fh1_wifi_status(ssid, sizeof(ssid), ip, sizeof(ip), &rssi);
        fh1_ui_value_set(s_kw.net_name, ssid[0] ? ssid : T("none saved"));
        fh1_ui_value_set(s_kw.net_ip, up ? ip : T("not connected"));
        snprintf(b, sizeof(b), "%d dBm · %s", rssi, rssi > -55 ? T("strong") : rssi > -67 ? T("good") : rssi > -75 ? T("fair") : T("weak"));
        fh1_ui_value_set(s_kw.net_signal, up ? b : "-");
    }
    if (s_kw.time_now) {
        time_t now = time(NULL);
        struct tm l, g;
        localtime_r(&now, &l);
        gmtime_r(&now, &g);
        bool set = l.tm_year + 1900 >= 2024;
        strftime(b, sizeof(b), "%H:%M:%S %d/%m/%Y", &l);
        fh1_ui_value_set(s_kw.time_now, set ? b : T("not set yet"));
        int off = ((l.tm_hour - g.tm_hour) + 36) % 24 - 12;
        snprintf(b, sizeof(b), "UTC%+d", off);
        fh1_ui_value_set(s_kw.time_zone, b);
        fh1_device_t *rtc = fh1_dev_ready_of_class("rtc");
        fh1_ui_value_set(s_kw.time_src, rtc ? T("RTC chip") : set ? T("network (NTP)") : T("waiting for the network"));
    }
    if (s_kw.chan) {
        snprintf(b, sizeof(b), T("Channels: %s"), T(s_channel_names[fh1_audio_channels()]));
        fh1_ui_button_set_text(s_kw.chan, b);
    }
}

static void bridges_open_cb(void *u) { bridges_open(); }

static void kind_closed(fh1_win_t *w, void *u)
{
    if (s_kw.timer) lv_timer_delete(s_kw.timer);
    memset(&s_kw, 0, sizeof(s_kw));
    s_btn_mode = s_btn_theme = s_btn_lang = s_btn_fps = s_btn_snd = NULL;
}

static void open_fw_update(void *u);

static void open_kind(void *user)
{
    const dev_kind_t *k = user;
    if (s_kw.win && fh1_win_is_open(s_kw.win)) fh1_win_close(s_kw.win);
    fh1_win_t *w = fh1_win_create(T(k->name), 240, 210);
    if (!w) return;
    memset(&s_kw, 0, sizeof(s_kw));
    s_kw.win = w;
    s_kw.kind = k;
    const fh1_hint_t hints[] = { { FH1_KEY_A, T("Change") }, { FH1_HINT_LEFTRIGHT, T("Switch") }, { FH1_KEY_B, T("Close") } };
    fh1_win_set_hints(w, hints, 3);
    fh1_win_on_close(w, kind_closed, NULL);
    if (k->clock) {
        lv_obj_t *pg = fh1_win_add_view(w, T("System time"));
        s_kw.time_now = fh1_ui_value(pg, T("Now"));
        s_kw.time_zone = fh1_ui_value(pg, T("Zone"));
        s_kw.time_src = fh1_ui_value(pg, T("Source"));
    }
    for (size_t i = 0; i < fh1_dev_count() && s_kw.n < 8; i++) {
        fh1_device_t *d = fh1_dev_at(i);
        if (!in_kind(k, d)) continue;
        dev_view_t *v = &s_kw.v[s_kw.n++];
        v->d = d;
        lv_obj_t *pg = fh1_win_add_view(w, view_name(d));
        // Its own settings first, then on/off and probing, then the readings.
        if (strcmp(d->cls, "display") == 0) {
            s_btn_mode = fh1_ui_button(w, pg, "", mode_cb, NULL);
            s_btn_theme = fh1_ui_button(w, pg, "", theme_cb, NULL);
            s_btn_snd = fh1_ui_button(w, pg, "", sounds_cb, NULL);
            s_btn_fps = fh1_ui_button(w, pg, "", fps_cb, NULL);
            s_btn_lang = fh1_ui_button(w, pg, "", lang_cb, NULL);
        } else if (strcmp(d->cls, "display.backlight") == 0) {
            fh1_ui_slider(w, pg, T("Brightness"), "%d%%", fh1_board_backlight_level(), 5, 100, 5, bl_slide, NULL);
        } else if (strcmp(d->cls, "audio.out") == 0) {
            fh1_ui_slider(w, pg, "Volume", "%d%%", fh1_audio_master_volume(), 0, 100, 5, vol_slide, NULL);
            s_kw.chan = fh1_ui_button(w, pg, "", chan_cb, NULL);
            fh1_ui_button(w, pg, T("Test sound"), tone_cb, NULL);
        } else if (strcmp(d->cls, "net.wifi") == 0) {
            fh1_ui_button(w, pg, T("Networks..."), open_wifi, NULL);
            fh1_ui_button(w, pg, T("Forget network"), wifi_forget_cb, NULL);
            s_kw.net_name = fh1_ui_value(pg, T("Network"));
            s_kw.net_ip = fh1_ui_value(pg, "IP");
            s_kw.net_signal = fh1_ui_value(pg, T("Signal"));
        } else if (strncmp(d->cls, "link.bridge", 11) == 0) {
            fh1_ui_button(w, pg, T("Bridges..."), bridges_open_cb, NULL);
            if (strcmp(d->cls, "link.bridge") == 0) fh1_ui_button(w, pg, T("Pair a controller (all bridges)"), pair_all_cb, NULL);
        }
        if (!d->essential) v->onoff = fh1_ui_button(w, pg, "", dev_toggle, d);
        fh1_ui_button(w, pg, T("Probe"), dev_probe, d);
        v->state = fh1_ui_value(pg, T("State"));
        v->drv = fh1_ui_value(pg, "Driver");
        v->info = fh1_ui_value(pg, T("Details"));
        v->power = fh1_ui_value(pg, T("Power"));
    }
    settings_labels();
    kind_update(NULL);
    s_kw.timer = lv_timer_create(kind_update, 500, NULL);
    fh1_win_show_view(w, 0);
}

// The list of kinds, each with how many of its devices are on.
static void devices_fill(void)
{
    if (!s_kinds_win || !fh1_win_is_open(s_kinds_win)) return;
    int focus = fh1_ui_focused_index(s_kinds_win);
    fh1_win_clear(s_kinds_win);
    for (size_t i = 0; i < NKINDS; i++) {
        const dev_kind_t *k = &s_kinds[i];
        int n = kind_count(k), ready = 0;
        for (size_t j = 0; j < fh1_dev_count(); j++) {
            fh1_device_t *d = fh1_dev_at(j);
            ready += in_kind(k, d) && d->state == FH1_DEV_READY;
        }
        char t[64];
        snprintf(t, sizeof(t), "%s (%d/%d)", T(k->name), ready, n);      // on / all
        fh1_ui_button(s_kinds_win, NULL, t, open_kind, (void *)k);
    }
    fh1_ui_focus_index(s_kinds_win, focus < 0 ? 0 : focus);
}

static void devices_closed(fh1_win_t *w, void *u) { s_kinds_win = NULL; }

static void open_devices(void)
{
    if (s_kinds_win && fh1_win_is_open(s_kinds_win)) {
        fh1_win_focus(s_kinds_win);
        return;
    }
    s_kinds_win = s_devices = fh1_win_create(T("Devices"), 200, 140);
    const fh1_hint_t hints[] = { { FH1_KEY_A, T("Open") }, { FH1_KEY_B, T("Close") } };
    fh1_win_set_hints(s_kinds_win, hints, 2);
    fh1_win_on_close(s_kinds_win, devices_closed, NULL);
    devices_fill();
}

// ── Storage ──

static lv_obj_t *s_meter_int, *s_meter_sd;
static lv_timer_t *s_storage_timer;

// "1.2 GB" / "340 MB" / "12 KB"
static void size_text(uint64_t bytes, char *out, size_t n)
{
    if (bytes >= (1ull << 30)) snprintf(out, n, "%.1f GB", bytes / (double)(1ull << 30));
    else if (bytes >= (1ull << 20)) snprintf(out, n, "%llu MB", bytes >> 20);
    else snprintf(out, n, "%llu KB", bytes >> 10);
}

static void storage_meter(lv_obj_t *m, uint64_t used, uint64_t total)
{
    char u[16], t[16], text[40];
    size_text(used, u, sizeof(u));
    size_text(total, t, sizeof(t));
    snprintf(text, sizeof(text), "%s / %s", u, t);
    int pct = total ? (int)(used * 100 / total) : 0;
    fh1_ui_meter_set(m, pct, text, fh1_ui_level_colour(100 - pct));
}

static void storage_update(lv_timer_t *t)
{
    if (!s_meter_int) return;
    fh1_fs_info_t fi = { 0 };
    fh1_int_info(&fi);
    storage_meter(s_meter_int, fi.used, fi.total);
    fh1_sd_info_t si;
    fh1_sd_info(&si);
    if (si.mounted) storage_meter(s_meter_sd, si.total - si.free, si.total);
    else fh1_ui_meter_set(s_meter_sd, 0, si.ejected ? T("ejected") : T("no card"), 0);
}

static void report(const char *what, esp_err_t err)
{
    char t[96];
    snprintf(t, sizeof(t), "%s: %s", T(what), err == ESP_OK ? T("done") : esp_err_to_name(err));
    fh1_ui_message(T("Storage"), t);
}

static void sd_mount_cb(void *u) { report("Mount", fh1_sd_mount()); }
static void sd_eject_cb(void *u) { report("Eject", fh1_sd_eject()); }
static void sd_format_yes(bool yes, void *u) { if (yes) report("Format SD", fh1_sd_format()); }
static void sd_format_cb(void *u) { fh1_ui_confirm(T("Format SD"), T("Erase everything on the SD card?"), sd_format_yes, NULL); }

static void int_format_do(void)
{
    run_stop();
    esp_err_t err = fh1_int_format();
    scan_installed();
    scan_packages();
    refresh_all(NULL);
    report("Format internal", err);
}
static void int_format_yes(bool yes, void *u) { if (yes) int_format_do(); }
static void int_format_cb(void *u)
{
    fh1_ui_confirm(T("Format internal"), T("Erase installed apps, their data and staged packages?"), int_format_yes, NULL);
}

static void storage_closed(fh1_win_t *w, void *u)
{
    lv_timer_delete(s_storage_timer);
    s_storage_timer = NULL;
    s_meter_int = s_meter_sd = NULL;
    s_storage = NULL;
}

static void open_storage(void)
{
    if (s_storage && fh1_win_is_open(s_storage)) {
        fh1_win_focus(s_storage);
        return;
    }
    s_storage = fh1_win_create(T("Storage"), 240, 160);
    const fh1_hint_t hints[] = { { FH1_KEY_A, T("Choose") }, { FH1_KEY_B, T("Close") } };
    fh1_win_set_hints(s_storage, hints, 2);
    s_meter_int = fh1_ui_meter(fh1_win_body(s_storage), T("Internal"));
    s_meter_sd = fh1_ui_meter(fh1_win_body(s_storage), "SD");
    fh1_ui_button(s_storage, NULL, T("Mount SD"), sd_mount_cb, NULL);
    fh1_ui_button(s_storage, NULL, T("Eject SD"), sd_eject_cb, NULL);
    fh1_ui_button(s_storage, NULL, T("Format SD..."), sd_format_cb, NULL);
    fh1_ui_button(s_storage, NULL, T("Format internal..."), int_format_cb, NULL);
    fh1_win_on_close(s_storage, storage_closed, NULL);
    s_storage_timer = lv_timer_create(storage_update, 1000, NULL);
    storage_update(NULL);
}

// ── Installer ──

static void installer_fill(void);

static esp_err_t do_install(const char *path, char *msg, size_t n)
{
    fh1_manifest_t m;
    char err[96];
    if (run_current()) {
        for (int i = 0; i < s_npkgs; i++) {
            if (strcmp(s_pkgs[i].path, path) == 0 && strcmp(s_pkgs[i].m.id, run_current()) == 0) run_stop();
        }
    }
    esp_err_t r = pkg_install(path, &m, err, sizeof(err));
    fh1_int_info_changed();
    if (r == ESP_OK) snprintf(msg, n, T("Installed %s %s"), m.name, m.version);
    else snprintf(msg, n, T("Not installed: %s"), err);
    scan_installed();
    refresh_all(NULL);
    return r;
}

static esp_err_t do_uninstall(const char *id, char *msg, size_t n)
{
    if (run_current() && strcmp(run_current(), id) == 0) run_stop();
    esp_err_t r = pkg_uninstall(id);
    fh1_int_info_changed();
    snprintf(msg, n, r == ESP_OK ? T("%s uninstalled") : T("%s could not be removed"), id);
    scan_installed();
    refresh_all(NULL);
    return r;
}

static void install_cb(void *user)
{
    char msg[128];
    do_install(((package_t *)user)->path, msg, sizeof(msg));
    fh1_ui_message(T("Installer"), msg);
}

static void uninstall_yes(bool yes, void *user)
{
    if (!yes) return;
    char msg[96];
    do_uninstall(user, msg, sizeof(msg));
    fh1_ui_message(T("Installer"), msg);
}

static void uninstall_cb(void *user)
{
    fh1_manifest_t *m = user;
    char t[96];
    snprintf(t, sizeof(t), T("Remove %s and its saved data?"), m->name);
    fh1_ui_confirm(T("Uninstall"), t, uninstall_yes, m->id);
}

static void installer_fill(void)
{
    if (!s_installer || !fh1_win_is_open(s_installer)) return;
    int focus = fh1_ui_focused_index(s_installer);
    fh1_win_clear(s_installer);
    lv_obj_t *body = fh1_win_body(s_installer);
    fh1_ui_button(s_installer, NULL, T("Firmware from SD..."), open_fw_update, NULL);
    fh1_ui_label(body, s_npkgs ? T("Packages") : T("No packages in /int/packages or /sd/packages."));
    for (int i = 0; i < s_npkgs; i++) {
        package_t *p = &s_pkgs[i];
        char t[128];
        if (p->valid) snprintf(t, sizeof(t), "%s %s [%s]", p->m.name, p->m.version, p->on_sd ? "SD" : "int");
        else snprintf(t, sizeof(t), "%s: %s", p->file, p->err);
        lv_obj_t *b = fh1_ui_button(s_installer, NULL, t, p->valid ? install_cb : NULL, p);
        fh1_ui_dim(b, !p->valid);
    }
    fh1_ui_label(body, s_ninstalled ? T("Installed") : T("Nothing installed."));
    for (int i = 0; i < s_ninstalled; i++) {
        char t[96];
        snprintf(t, sizeof(t), "%s %s", s_installed[i].name, s_installed[i].version);
        fh1_ui_button(s_installer, NULL, t, uninstall_cb, &s_installed[i]);
    }
    fh1_ui_focus_index(s_installer, focus < 0 ? 0 : focus);
}

static void installer_closed(fh1_win_t *w, void *u) { s_installer = NULL; }

static void open_installer(void)
{
    if (s_installer && fh1_win_is_open(s_installer)) {
        fh1_win_focus(s_installer);
        return;
    }
    scan_packages();
    s_installer = fh1_win_create(T("Installer"), 280, 180);
    const fh1_hint_t hints[] = { { FH1_KEY_A, T("Install/rm") }, { FH1_KEY_B, T("Close") } };
    fh1_win_set_hints(s_installer, hints, 2);
    fh1_win_on_close(s_installer, installer_closed, NULL);
    installer_fill();
}

// ── About: read only. What the device is and how it is doing. ──

static lv_obj_t *s_meter_ram, *s_meter_psram;
static lv_obj_t *s_ab_board, *s_ab_up, *s_ab_wifi;
static lv_timer_t *s_about_timer;

static void memory_meter(lv_obj_t *m, uint32_t caps)
{
    size_t total = heap_caps_get_total_size(caps), free_b = heap_caps_get_free_size(caps);
    char f[16], text[40];
    size_text(free_b, f, sizeof(f));
    snprintf(text, sizeof(text), T("%s free"), f);
    int used_pct = total ? (int)((total - free_b) * 100 / total) : 0;
    fh1_ui_meter_set(m, used_pct, text, fh1_ui_level_colour(100 - used_pct));
}

static void about_update(lv_timer_t *t)
{
    if (!s_ab_board) return;
    char ssid[33], ip[16], v[96];
    int rssi;
    bool up = fh1_wifi_status(ssid, sizeof(ssid), ip, sizeof(ip), &rssi);
    uint32_t sec = (uint32_t)(esp_timer_get_time() / 1000000);
    snprintf(v, sizeof(v), "%luh %02lum", (unsigned long)(sec / 3600), (unsigned long)(sec / 60 % 60));
    fh1_ui_value_set(s_ab_up, v);
    snprintf(v, sizeof(v), "%s", up ? ip : T("not connected"));
    fh1_ui_value_set(s_ab_wifi, v);
    memory_meter(s_meter_ram, MALLOC_CAP_INTERNAL);
    memory_meter(s_meter_psram, MALLOC_CAP_SPIRAM);
}

static void about_closed(fh1_win_t *w, void *u)
{
    lv_timer_delete(s_about_timer);
    s_about_timer = NULL;
    s_ab_board = NULL;
    s_meter_ram = s_meter_psram = NULL;
    s_system = NULL;
}

static void restart_cb(void *u);

static bool about_key(fh1_win_t *w, int key, void *u)
{
    if (key != FH1_KEY_X) return false;
    restart_cb(NULL);
    return true;
}

static void open_about(void)
{
    if (s_system && fh1_win_is_open(s_system)) {
        fh1_win_focus(s_system);
        return;
    }
    s_system = fh1_win_create(T("About"), 280, 220);
    const fh1_hint_t hints[] = { { FH1_KEY_X, T("Restart") }, { FH1_KEY_B, T("Close") } };
    fh1_win_set_hints(s_system, hints, 2);
    fh1_win_on_key(s_system, about_key, NULL);
    fh1_win_on_close(s_system, about_closed, NULL);
    lv_obj_t *body = fh1_win_body(s_system);
    char v[96];
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    s_ab_board = fh1_ui_value(body, T("Board"));
    fh1_ui_value_set(s_ab_board, fh1_board_name());
    snprintf(v, sizeof(v), "ESP32-S3 r%d.%d · %d x %d MHz", chip.revision / 100, chip.revision % 100, chip.cores,
             CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    fh1_ui_value_set(fh1_ui_value(body, "CPU"), v);
    snprintf(v, sizeof(v), T("flash %lu MB · PSRAM %u MB"), (unsigned long)(flash >> 20), (unsigned)(esp_psram_get_size() >> 20));
    fh1_ui_value_set(fh1_ui_value(body, T("Memory")), v);
    s_meter_ram = fh1_ui_meter(body, "RAM");
    s_meter_psram = fh1_ui_meter(body, "PSRAM");
    snprintf(v, sizeof(v), "%s · SDK %d.%d", esp_app_get_description()->version, FH1_SDK_MAJOR, FH1_SDK_MINOR);
    fh1_ui_value_set(fh1_ui_value(body, T("Version")), v);
    snprintf(v, sizeof(v), "ESP-IDF %s · LVGL %d.%d", esp_get_idf_version(), LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR);
    fh1_ui_value_set(fh1_ui_value(body, T("Built on")), v);
    s_ab_up = fh1_ui_value(body, T("Up"));
    s_ab_wifi = fh1_ui_value(body, "Wi-Fi");
    snprintf(v, sizeof(v), "fh1.local · %s %s", T("Code"), fh1_console_token());
    fh1_ui_value_set(fh1_ui_value(body, "Console"), v);
    s_about_timer = lv_timer_create(about_update, 1000, NULL);
    about_update(NULL);
}

// ── Settings that live in the device views (Video) ──

static void settings_labels(void)
{
    char t[64];
    if (!s_btn_mode) return;                       // the Video window is not open
    const fh1_display_mode_t *modes;
    int n = fh1_board_display_modes(&modes);
    char want[16];
    fh1_settings_get_str("display", "mode", want, sizeof(want));
    const char *name = modes[0].name;
    for (int i = 0; i < n; i++) {
        if (strcmp(modes[i].id, want) == 0) name = modes[i].name;
    }
    snprintf(t, sizeof(t), T("Screen: %s%s"), T(name),
             strcmp(name, fh1_board_display_mode()->name) != 0 ? T(" (restart)") : "");
    if (s_btn_mode) fh1_ui_button_set_text(s_btn_mode, t);
    const char *const *ids, *const *names;
    int nt = fh1_ui_themes(&ids, &names);
    for (int i = 0; i < nt; i++) {
        if (strcmp(ids[i], fh1_ui_theme()) == 0) snprintf(t, sizeof(t), T("Theme: %s"), names[i]);
    }
    if (s_btn_theme) fh1_ui_button_set_text(s_btn_theme, t);
    snprintf(t, sizeof(t), T("Language: %s"), strcmp(fh1_lang(), "pt") == 0 ? "Português" : "English");
    if (s_btn_lang) fh1_ui_button_set_text(s_btn_lang, t);
    snprintf(t, sizeof(t), T("FPS: %s"), fh1_ui_show_fps() ? T("on") : T("off"));
    if (s_btn_fps) fh1_ui_button_set_text(s_btn_fps, t);
    snprintf(t, sizeof(t), T("System sounds: %s"), fh1_ui_sounds() ? T("on") : T("off"));
    if (s_btn_snd) fh1_ui_button_set_text(s_btn_snd, t);
}

static void restart_yes(bool yes, void *u)
{
    if (yes) fh1_system_restart();
}

static void mode_cb(void *u)
{
    const fh1_display_mode_t *modes;
    int n = fh1_board_display_modes(&modes);
    char cur[16];
    fh1_settings_get_str("display", "mode", cur, sizeof(cur));
    int i = 0;
    while (i < n && strcmp(modes[i].id, cur) != 0) i++;
    i = (i + 1) % (i == n ? 1 : n);
    fh1_settings_set_str("display", "mode", modes[i].id);
    settings_labels();
    if (strcmp(modes[i].id, fh1_board_display_mode()->id) != 0) {
        fh1_ui_confirm(T("Screen"), T("Restart now to change the screen mode?"), restart_yes, NULL);
    }
}

static void theme_cb(void *u)
{
    const char *const *ids;
    int n = fh1_ui_themes(&ids, NULL);
    int i = 0;
    while (i < n && strcmp(ids[i], fh1_ui_theme()) != 0) i++;
    fh1_ui_set_theme(ids[(i + 1) % n]);
    settings_labels();
}

static void open_fw_update(void *u);


static void restart_cb(void *u) { fh1_ui_confirm(T("Restart"), T("Restart the FH1?"), restart_yes, NULL); }

static void lang_cb(void *u)
{
    fh1_set_lang(strcmp(fh1_lang(), "pt") == 0 ? "en" : "pt");
    settings_labels();
    refresh_all(NULL);
}

static void fps_cb(void *u)
{
    fh1_ui_set_show_fps(!fh1_ui_show_fps());
    settings_labels();
}

static void sounds_cb(void *u)
{
    fh1_ui_set_sounds(!fh1_ui_sounds());
    settings_labels();
}

// The UI's system sounds go through the apps' mixer, under the sound volume.
static void ui_sound_play(const int16_t *pcm, uint32_t frames, uint32_t rate) { fh1_sfx_play(pcm, frames, rate, 200); }

// ── Themes: the interface's looks and voices, tried at once ──
// One line a theme (the one in use says so), A puts it on (no restart); then the system sounds' switch.

#define THEMES_MAX 8
static fh1_win_t *s_themes_win;
static lv_obj_t *s_theme_btn[THEMES_MAX], *s_theme_snd;

static void themes_labels(void)
{
    if (!s_themes_win) return;
    const char *const *ids, *const *names;
    const int n = fh1_ui_themes(&ids, &names);
    char t[64];
    for (int i = 0; i < n && i < THEMES_MAX; i++) {
        snprintf(t, sizeof(t), strcmp(ids[i], fh1_ui_theme()) == 0 ? T("%s (in use)") : "%s", names[i]);
        fh1_ui_button_set_text(s_theme_btn[i], t);
    }
    snprintf(t, sizeof(t), T("System sounds: %s"), fh1_ui_sounds() ? T("on") : T("off"));
    fh1_ui_button_set_text(s_theme_snd, t);
}

static void theme_pick(void *u)
{
    const char *const *ids;
    const int n = fh1_ui_themes(&ids, NULL), i = (int)(intptr_t)u;
    if (i < n) fh1_ui_set_theme(ids[i]);
    themes_labels();
    settings_labels();
}

static void theme_sounds(void *u)
{
    fh1_ui_set_sounds(!fh1_ui_sounds());
    themes_labels();
    settings_labels();
}

static void themes_closed(fh1_win_t *w, void *u) { s_themes_win = NULL; }

static void themes_open(void)
{
    if (s_themes_win) {
        fh1_win_focus(s_themes_win);
        return;
    }
    const char *const *ids;
    const int n = fh1_ui_themes(&ids, NULL);
    fh1_win_t *w = s_themes_win = fh1_win_create(T("Themes"), 200, 32 + 13 * (n + 1) + 24);
    if (!w) return;
    for (int i = 0; i < n && i < THEMES_MAX; i++) s_theme_btn[i] = fh1_ui_button(w, NULL, "", theme_pick, (void *)(intptr_t)i);
    s_theme_snd = fh1_ui_button(w, NULL, "", theme_sounds, NULL);
    const fh1_hint_t hints[] = { { FH1_KEY_A, T("Use") }, { FH1_KEY_B, T("Close") } };
    fh1_win_set_hints(w, hints, 2);
    fh1_win_on_close(w, themes_closed, NULL);
    themes_labels();
    for (int i = 0; i < n; i++)                     // the cursor on the theme in use
        if (strcmp(ids[i], fh1_ui_theme()) == 0) fh1_ui_focus_index(w, i);
}

// ── Wi-Fi: scan, pick a network, type its password ──

static fh1_win_t *s_wifi_win;
static lv_obj_t *s_wifi_status;
static lv_timer_t *s_wifi_timer;
#define MAX_APS 16
static fh1_wifi_ap_t s_aps[MAX_APS];
static int s_naps = -2;          // -2 not scanned, -1 failed
static bool s_scanning;

static void wifi_fill(void);

static void wifi_status_tick(lv_timer_t *t)
{
    if (!s_wifi_status) return;
    char ssid[33], ip[16];
    int rssi;
    if (fh1_wifi_status(ssid, sizeof(ssid), ip, sizeof(ip), &rssi))
        lv_label_set_text_fmt(s_wifi_status, T("Connected: %s · %s · %d dBm"), ssid, ip, rssi);
    else if (ssid[0])
        lv_label_set_text_fmt(s_wifi_status, T("Joining %s..."), ssid);
    else
        lv_label_set_text(s_wifi_status, T("No network saved · setup access point FH1-setup"));
}

static void scan_done(void *arg)
{
    s_scanning = false;
    wifi_fill();
}

static void scan_task(void *arg)
{
    s_naps = fh1_wifi_scan(s_aps, MAX_APS);
    fh1_ui_async(scan_done, NULL);
    vTaskDelete(NULL);
}

static void wifi_scan_cb(void *u)
{
    if (s_scanning) return;
    s_scanning = true;
    wifi_fill();
    xTaskCreatePinnedToCore(scan_task, "wifi_scan", 4096, NULL, 2, NULL, 0);
}

static char s_join_ssid[33];

static void password_done(const char *text, void *u)
{
    if (!text) return;
    fh1_wifi_set_credentials(s_join_ssid, text);
    char t[80];
    snprintf(t, sizeof(t), T("Joining %s."), s_join_ssid);
    fh1_ui_message(T("Wi-Fi"), t);
}

static void wifi_pick_cb(void *user)
{
    fh1_wifi_ap_t *ap = user;
    strlcpy(s_join_ssid, ap->ssid, sizeof(s_join_ssid));
    if (ap->secure) {
        char title[48];
        snprintf(title, sizeof(title), "%.20s %s", ap->ssid, T("PASSWORD"));
        fh1_ui_keyboard(title, "", 64, password_done, NULL);
    } else {
        password_done("", NULL);
    }
}

static void forget_yes(bool yes, void *u) { if (yes) fh1_wifi_forget(); }
static void wifi_forget_cb(void *u) { fh1_ui_confirm(T("Wi-Fi"), T("Forget the saved network?"), forget_yes, NULL); }

static void wifi_fill(void)
{
    if (!s_wifi_win || !fh1_win_is_open(s_wifi_win)) return;
    int focus = fh1_ui_focused_index(s_wifi_win);
    fh1_win_clear(s_wifi_win);
    lv_obj_t *body = fh1_win_body(s_wifi_win);
    s_wifi_status = fh1_ui_line(body, "");
    wifi_status_tick(NULL);
    fh1_ui_button(s_wifi_win, NULL, s_scanning ? T("Scanning...") : T("Scan for networks"), wifi_scan_cb, NULL);
    fh1_ui_button(s_wifi_win, NULL, T("Forget network"), wifi_forget_cb, NULL);
    if (s_naps == -1) fh1_ui_label(body, T("Scan failed."));
    for (int i = 0; i < s_naps; i++) {
        char t[64];
        int bars = s_aps[i].rssi > -55 ? 4 : s_aps[i].rssi > -65 ? 3 : s_aps[i].rssi > -75 ? 2 : 1;
        snprintf(t, sizeof(t), "%s %.*s%s", s_aps[i].ssid, bars, "||||", s_aps[i].secure ? "" : T(" open"));
        fh1_ui_button(s_wifi_win, NULL, t, wifi_pick_cb, &s_aps[i]);
    }
    fh1_ui_focus_index(s_wifi_win, focus < 0 ? 0 : focus);
}

static void wifi_closed(fh1_win_t *w, void *u)
{
    lv_timer_delete(s_wifi_timer);
    s_wifi_timer = NULL;
    s_wifi_status = NULL;
    s_wifi_win = NULL;
}

static void open_wifi(void *u)
{
    if (s_wifi_win && fh1_win_is_open(s_wifi_win)) {
        fh1_win_focus(s_wifi_win);
        return;
    }
    s_wifi_win = fh1_win_create(T("Wi-Fi"), 240, 200);
    const fh1_hint_t hints[] = { { FH1_KEY_A, T("Choose") }, { FH1_KEY_B, T("Close") } };
    fh1_win_set_hints(s_wifi_win, hints, 2);
    fh1_win_on_close(s_wifi_win, wifi_closed, NULL);
    s_wifi_timer = lv_timer_create(wifi_status_tick, 1000, NULL);
    wifi_fill();
}

// ── Firmware update from the SD card: any .bin in /sd/firmware ──

#define FW_DIR FH1_SD_PATH "/firmware"
static fh1_win_t *s_fw_win;
static char s_fw_files[8][96], s_fw_versions[8][32];
static int s_nfw;
static const char *s_fw_name;     // the file being written, for the progress overlay

static void fw_restart(lv_timer_t *t) { fh1_system_restart(); }

typedef struct {
    esp_err_t err;
    char msg[96];
} fw_result_t;
static fw_result_t s_fw_result;

static void fw_result(void *arg)
{
    if (s_fw_result.err == ESP_OK) {
        lv_timer_set_repeat_count(lv_timer_create(fw_restart, 2500, NULL), 1);   // the overlay says so meanwhile
    } else {
        fh1_ui_message(T("Update"), s_fw_result.msg);
    }
}

// Progress goes to the transfer overlay, as for an update over the network.
static void fw_progress(int pct, void *u) { fh1_transfer_report(FH1_XFER_FIRMWARE, FH1_XFER_RUNNING, s_fw_name, pct, 100); }

static void fw_task(void *arg)
{
    const char *slash = strrchr(arg, '/');
    s_fw_name = slash ? slash + 1 : arg;
    fh1_transfer_report(FH1_XFER_FIRMWARE, FH1_XFER_RUNNING, s_fw_name, 0, 100);
    s_fw_result.err = fh1_ota_from_file(arg, fw_progress, NULL, s_fw_result.msg, sizeof(s_fw_result.msg));
    fh1_transfer_report(FH1_XFER_FIRMWARE, s_fw_result.err == ESP_OK ? FH1_XFER_DONE : FH1_XFER_FAILED, s_fw_name, 100, 100);
    fh1_ui_async(fw_result, NULL);
    vTaskDelete(NULL);
}

static void fw_yes(bool yes, void *user)
{
    if (!yes) return;
    if (s_fw_win && fh1_win_is_open(s_fw_win)) fh1_win_close(s_fw_win);
    xTaskCreatePinnedToCore(fw_task, "fw_update", 6144, user, 2, NULL, 0);
}

static void fw_pick(void *user)
{
    int i = (int)(intptr_t)user;
    char t[128];
    snprintf(t, sizeof(t), T("Install firmware %s?\nRunning: %s"), s_fw_versions[i], fh1_ota_running_version());
    fh1_ui_confirm(T("Update"), t, fw_yes, s_fw_files[i]);
}

static void fw_closed(fh1_win_t *w, void *u) { s_fw_win = NULL; }

static void open_fw_update(void *u)
{
    if (s_fw_win && fh1_win_is_open(s_fw_win)) fh1_win_close(s_fw_win);
    s_fw_win = fh1_win_create(T("Update from SD"), 240, 150);
    const fh1_hint_t hints[] = { { FH1_KEY_A, T("Install") }, { FH1_KEY_B, T("Close") } };
    fh1_win_set_hints(s_fw_win, hints, 2);
    fh1_win_on_close(s_fw_win, fw_closed, NULL);
    lv_obj_t *body = fh1_win_body(s_fw_win);
    s_nfw = 0;
    fh1_sd_info_t si;
    fh1_sd_info(&si);
    DIR *d = si.mounted ? opendir(FW_DIR) : NULL;
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL && s_nfw < 8) {
        size_t l = strlen(e->d_name);
        if (l < 5 || strcasecmp(e->d_name + l - 4, ".bin") != 0) continue;
        snprintf(s_fw_files[s_nfw], sizeof(s_fw_files[0]), FW_DIR "/%.60s", e->d_name);
        char err[64];
        if (fh1_ota_peek_file(s_fw_files[s_nfw], s_fw_versions[s_nfw], sizeof(s_fw_versions[0]), err, sizeof(err)) != ESP_OK) continue;
        char t[96];
        snprintf(t, sizeof(t), "%.60s %s", e->d_name, s_fw_versions[s_nfw]);
        fh1_ui_button(s_fw_win, NULL, t, fw_pick, (void *)(intptr_t)s_nfw);
        s_nfw++;
    }
    if (d) closedir(d);
    if (!si.mounted || !s_nfw) fh1_ui_line(body, !si.mounted ? T("No SD card.") : T("No firmware in /sd/firmware."));
    fh1_ui_value_set(fh1_ui_value(body, T("Running")), fh1_ota_running_version());
}

// ── Refresh on changes ──

static void refresh_all(void *arg)
{
    launcher_fill();
    devices_fill();
    installer_fill();
}

static void on_devices_changed(void *arg)
{
    scan_packages();      // the SD card may have come or gone
    refresh_all(NULL);
}

// ── Console hooks (called from the console's tasks) ──

static void console_json(cJSON *root)
{
    cJSON *a = cJSON_AddObjectToObject(root, "apps");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const char *running = run_current();
    if (running) cJSON_AddStringToObject(a, "running", running);
    cJSON *b = cJSON_AddArrayToObject(a, "builtin");
    for (size_t i = 0; i < NUSER; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", s_user_builtins[i].id);
        cJSON_AddStringToObject(o, "name", s_user_builtins[i].name);
        cJSON_AddItemToArray(b, o);
    }
    for (size_t i = 0; i < NBUILTIN; i++) {
        bool listed = false;                                       // Temas is in both menus: list it once
        for (size_t j = 0; j < NUSER; j++) listed |= strcmp(s_user_builtins[j].id, s_builtins[i].id) == 0;
        if (listed) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", s_builtins[i].id);
        cJSON_AddStringToObject(o, "name", s_builtins[i].name);
        cJSON_AddItemToArray(b, o);
    }
    cJSON *ins = cJSON_AddArrayToObject(a, "installed");
    for (int i = 0; i < s_ninstalled; i++) {
        fh1_manifest_t *m = &s_installed[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", m->id);
        cJSON_AddStringToObject(o, "name", m->name);
        cJSON_AddStringToObject(o, "version", m->version);
        cJSON *needs = cJSON_AddArrayToObject(o, "needs"), *wants = cJSON_AddArrayToObject(o, "wants");
        cJSON *missing = cJSON_AddArrayToObject(o, "missing");
        for (int c = 0; c < FH1_CAP_COUNT; c++) {
            uint32_t bit = 1u << c;
            if (m->needs & bit) cJSON_AddItemToArray(needs, cJSON_CreateString(fh1_cap_name(bit)));
            if (m->wants & bit) cJSON_AddItemToArray(wants, cJSON_CreateString(fh1_cap_name(bit)));
            if ((m->needs & bit) && !(fh1_caps() & bit)) cJSON_AddItemToArray(missing, cJSON_CreateString(fh1_cap_name(bit)));
        }
        cJSON_AddItemToArray(ins, o);
    }
    cJSON *pk = cJSON_AddArrayToObject(a, "packages");
    for (int i = 0; i < s_npkgs; i++) {
        package_t *p = &s_pkgs[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "file", p->file);
        cJSON_AddStringToObject(o, "path", p->path);
        cJSON_AddStringToObject(o, "where", p->on_sd ? "SD" : "internal");
        if (p->valid) {
            cJSON_AddStringToObject(o, "id", p->m.id);
            cJSON_AddStringToObject(o, "name", p->m.name);
            cJSON_AddStringToObject(o, "version", p->m.version);
        } else {
            cJSON_AddStringToObject(o, "name", p->err);
        }
        cJSON_AddItemToArray(pk, o);
    }
    xSemaphoreGive(s_lock);
}

// Dev: users from the console (System > Users does the same with keys). A user is named by name or id.
//   users                          the list: name#tag, kind, owner, PIN, player
//   user_new <name> [visitor]      id = the name, path = "visitor" for a visitor
//   user_use <user> [pin]          player 1 without a controller of their own
//   user_link <pad id> <user|"">   a controller's user ("" frees it)
//   user_pin <user> <old>,<new>    set, change ("" old) or remove ("" new) a PIN
//   user_rename <user> <new>[,pin]
//   user_backup <user> <pin>,<pass>   and user_restore <file> <pass>
//   user_remove <user>
//   user_delbackup <file>          delete a backup file
//   user_look                      look for other units now; user_rel <user>: their relations and the outbox
//   user_friend <user> <other,level[,pin]>   a trust statement; user_share <author> <who,viewer[,pin]>: a test header
//   users_data                     player 1's data folder and its app folders
//   users_reset                    back to no users (the owner's saves become the unit's again)
static const char *user_by(const char *s)
{
    static fh1_user_t u;
    if (fh1_users_get(s, &u)) return u.id;
    for (int i = 0; fh1_users_at(i, &u); i++)
        if (strcmp(u.name, s) == 0) return u.id;
    return NULL;
}

static void users_cmd(const char *op, const char *id, const char *path, char *out, size_t n)
{
    char a[64] = "", b[72] = "";
    const char *comma = strchr(path, ',');
    if (comma) {
        snprintf(a, sizeof(a), "%.*s", (int)(comma - path), path);
        snprintf(b, sizeof(b), "%s", comma + 1);
    } else {
        snprintf(a, sizeof(a), "%s", path);
    }
    esp_err_t r = ESP_OK;
    const int64_t t0 = esp_timer_get_time();
    if (strcmp(op, "users") == 0) {
        int k = snprintf(out, n, "unit %s:", fh1_users_unit_id());
        fh1_user_t u, p;
        for (int i = 0; fh1_users_at(i, &u) && k < (int)n; i++) {
            int pl = -1;
            for (int j = 0; j < FH1_PLAYERS; j++)
                if (fh1_users_player(j, &p) && strcmp(p.id, u.id) == 0) pl = j;
            k += snprintf(out + k, n - k, " %s#%s%s%s%s", u.name, u.tag, u.owner ? "*" : u.kind == FH1_USER_VISITOR ? "~" : "",
                          u.pin ? "(pin)" : "", pl == 0 ? "[1P]" : pl == 1 ? "[2P]" : "");
        }
        return;
    }
    if (strcmp(op, "users_data") == 0) {
        // Player 1's data folder (fh1_data_dir) and what is in it: one folder per app.
        char dir[96];
        fh1_data_dir("", dir, sizeof(dir));
        int k = snprintf(out, n, "%s:", dir);
        DIR *d = opendir(dir);
        struct dirent *e;
        while (d && (e = readdir(d)) != NULL && k < (int)n) k += snprintf(out + k, n - k, " %s", e->d_name);
        if (d) closedir(d);
        return;
    }
    if (strcmp(op, "user_delbackup") == 0) {
        // A backup file in the backup folder (id: its file name, no folders).
        char f[96];
        const bool ok = id[0] && !strchr(id, '/') && !strstr(id, "..") &&
                        snprintf(f, sizeof(f), FH1_USERS_BACKUP_DIR "/%s", id) < (int)sizeof(f) && unlink(f) == 0;
        snprintf(out, n, "backup %s %s", id, ok ? "deleted" : "not deleted");
        return;
    }
    if (strcmp(op, "user_avbench") == 0) {
        // Dev: how long the avatar views take to draw (each 10 times).
        uint32_t *px = heap_caps_malloc(FH1_AVATAR_FULL_W * FH1_AVATAR_FULL_H * 4, MALLOC_CAP_SPIRAM);
        if (!px) return;
        fh1_avatar_t a;
        fh1_avatar_from_seed("bench", 5, &a);
        int64_t t0 = esp_timer_get_time();
        for (int i = 0; i < 10; i++) fh1_avatar_render_full(&a, i % FH1_AV_EXPRS, i * 100, px);
        int64_t t1 = esp_timer_get_time();
        for (int i = 0; i < 10; i++) fh1_avatar_render_face(&a, i % FH1_AV_EXPRS, i * 100, 32, px);
        int64_t t2 = esp_timer_get_time();
        for (int i = 0; i < 10; i++) fh1_avatar_render_face(&a, i % FH1_AV_EXPRS, i * 100, 16, px);
        int64_t t3 = esp_timer_get_time();
        heap_caps_free(px);
        snprintf(out, n, "avatar: full %lld us, face %lld us, mini %lld us", (t1 - t0) / 10, (t2 - t1) / 10, (t3 - t2) / 10);
        return;
    }
    if (strcmp(op, "user_forget") == 0) {
        snprintf(out, n, "forget contact %s: %s", id, esp_err_to_name(fh1_contacts_remove(id)));
        return;
    }
    if (strcmp(op, "user_look") == 0) {
        net_kick();
        snprintf(out, n, "looking for other units; contacts now %d", fh1_contacts_count());
        return;
    }
    if (strcmp(op, "user_rel") == 0) {
        // A local user's relations: other#tag out/in levels.
        const char *uid = user_by(id);
        if (!uid) {
            snprintf(out, n, "no user %s", id);
            return;
        }
        char ids[24][FH1_USER_ID_LEN + 1];
        const int k = fh1_users_relations(uid, ids, 24);
        int o = snprintf(out, n, "%s:", id);
        for (int i = 0; i < k && o < (int)n; i++) {
            uint8_t lo, li;
            fh1_users_relation(uid, ids[i], &lo, &li);
            o += snprintf(out + o, n - o, " %.4s out %u in %u", ids[i], lo, li);
        }
        fh1_outbox_t *box = heap_caps_malloc(4 * sizeof(fh1_outbox_t), MALLOC_CAP_SPIRAM);
        const int ob = box ? fh1_users_outbox(box, 4) : -1;
        heap_caps_free(box);
        if (o < (int)n) snprintf(out + o, n - o, "; outbox %d", ob);
        return;
    }
    if (strcmp(op, "user_friend") == 0) {
        // id: the local user; path: "<other id>,<level>[,pin]"
        const char *uid = user_by(id);
        char other[FH1_USER_ID_LEN + 1] = "", pin[12] = "";
        int level = 2;
        sscanf(path, "%16[^,],%d,%11s", other, &level, pin);
        esp_err_t r = uid ? fh1_users_trust(uid, other, (uint8_t)level, pin) : ESP_ERR_NOT_FOUND;
        if (r == ESP_OK) net_kick();
        snprintf(out, n, "user_friend %s -> %s level %d: %s", id, other, level, esp_err_to_name(r));
        return;
    }
    if (strcmp(op, "user_share") == 0) {
        // id: the author; path: "<who>,<viewer>[,pin]": signs a header for a test item, checks it for the viewer.
        char uid_b[FH1_USER_ID_LEN + 1] = "", vid_b[FH1_USER_ID_LEN + 1] = "";      // user_by's answer is one buffer
        if (user_by(id)) strlcpy(uid_b, user_by(id), sizeof(uid_b));
        char viewer_n[20] = "", pin[12] = "";
        int who = 0;
        sscanf(path, "%d,%19[^,],%11s", &who, viewer_n, pin);
        if (user_by(viewer_n)) strlcpy(vid_b, user_by(viewer_n), sizeof(vid_b));
        const char *uid = uid_b[0] ? uid_b : NULL, *vid = vid_b[0] ? vid_b : NULL;
        uint8_t sha[32] = { 1, 2, 3 }, hdr[200];
        size_t hn = 0;
        esp_err_t r = uid ? fh1_users_share_sign(uid, pin, sha, 1234, (uint8_t)who, hdr, sizeof(hdr), &hn) : ESP_ERR_NOT_FOUND;
        char author[FH1_USER_ID_LEN + 1] = "";
        bool allowed = false;
        esp_err_t c = r == ESP_OK ? fh1_users_share_check(hdr, hn, vid ? vid : "", author, &allowed) : r;
        hdr[hn / 2] ^= 1;                                   // a changed header must fail
        bool a2;
        esp_err_t bad = r == ESP_OK ? fh1_users_share_check(hdr, hn, vid ? vid : "", NULL, &a2) : r;
        snprintf(out, n, "share by %s (%u bytes): sign %s, check %s, author %.4s, %s may see: %s; tampered: %s", id, (unsigned)hn,
                 esp_err_to_name(r), esp_err_to_name(c), author, viewer_n, allowed ? "yes" : "no", esp_err_to_name(bad));
        return;
    }
    if (strcmp(op, "users_reset") == 0) {
        snprintf(out, n, "users reset: %s", esp_err_to_name(fh1_users_reset()));
        return;
    }
    const char *uid = user_by(id);
    if (strcmp(op, "user_new") == 0) {
        char nid[FH1_USER_ID_LEN + 1] = "";
        r = fh1_users_create(id, strcmp(path, "visitor") == 0 ? FH1_USER_VISITOR : FH1_USER_LOCAL, nid);
        snprintf(out, n, "user_new %s: %s %s", id, esp_err_to_name(r), nid);
        return;
    }
    if (strcmp(op, "user_link") == 0) {
        const char *who = path[0] ? user_by(path) : "";
        r = who ? fh1_users_link_pad(id, who) : ESP_ERR_NOT_FOUND;
    } else if (strcmp(op, "user_restore") == 0) {
        char nid[FH1_USER_ID_LEN + 1] = "";
        r = fh1_users_import(id, path, nid);
    } else if (!uid) {
        r = ESP_ERR_NOT_FOUND;
    } else if (strcmp(op, "user_use") == 0) {
        if (fh1_users_check_pin(uid, path)) fh1_users_use(uid);
        else r = ESP_ERR_INVALID_CRC;
    } else if (strcmp(op, "user_pin") == 0) {
        r = fh1_users_set_pin(uid, a, b);
    } else if (strcmp(op, "user_rename") == 0) {
        r = fh1_users_rename(uid, a, b);
    } else if (strcmp(op, "user_backup") == 0) {
        char file[96] = "";
        r = fh1_users_export(uid, a, b, file, sizeof(file));
        if (r == ESP_OK) {
            snprintf(out, n, "backup: %s (%lld ms)", file, (esp_timer_get_time() - t0) / 1000);
            return;
        }
    } else if (strcmp(op, "user_remove") == 0) {
        r = fh1_users_remove(uid);
    } else {
        r = ESP_ERR_NOT_SUPPORTED;
    }
    snprintf(out, n, "%s %s: %s (%lld ms)", op, id, esp_err_to_name(r), (esp_timer_get_time() - t0) / 1000);
}

typedef struct {
    char op[16], id[48], path[128];
    char reply[480];
    SemaphoreHandle_t done;
} cmd_t;

static bool user_named(const char *name, fh1_user_t *out)
{
    for (int i = 0; fh1_users_at(i, out); i++)
        if (strcasecmp(out->name, name) == 0) return true;
    return false;
}

// Dev: an app's saved data per user. data_list (id: the app): each user's files, their size and when written.
// data_copy (id: the app, path: "from>to", user names): the files copied; the target's own are kept in a .bak
// folder next to them first.
static void data_op(cmd_t *c)
{
    const size_t n = sizeof(c->reply);
    char dir[96], p[192];
    fh1_user_t u, to;
    int k = 0;
    if (strcmp(c->op, "data_list") == 0) {
        for (int i = 0; fh1_users_at(i, &u) && k < (int)n; i++) {
            fh1_users_data_dir(u.id, c->id, dir, sizeof(dir));
            k += snprintf(c->reply + k, n - k, "%s%s:", i ? " | " : "", u.name);
            DIR *d = opendir(dir);
            struct dirent *e;
            while (d && (e = readdir(d)) != NULL && k < (int)n) {
                struct stat st;
                if (e->d_name[0] == '.' || snprintf(p, sizeof(p), "%s/%.64s", dir, e->d_name) >= (int)sizeof(p) || stat(p, &st)) continue;
                struct tm tm;
                localtime_r(&st.st_mtime, &tm);
                k += snprintf(c->reply + k, n - k, " %s %ldB %02d/%02d %02d:%02d", e->d_name, (long)st.st_size, tm.tm_mday,
                              tm.tm_mon + 1, tm.tm_hour, tm.tm_min);
            }
            if (d) closedir(d);
        }
        return;
    }
    char from[24] = "", *gt = strchr(c->path, '>');
    if (!gt || (size_t)(gt - c->path) >= sizeof(from) || !c->id[0] || strstr(c->id, "..") || strchr(c->id, '/')) {
        snprintf(c->reply, n, "data_copy: id the app, path \"from>to\" (user names)");
        return;
    }
    memcpy(from, c->path, gt - c->path);
    if (!user_named(from, &u) || !user_named(gt + 1, &to) || strcmp(u.id, to.id) == 0) {
        snprintf(c->reply, n, "data_copy: unknown users (%s, %s)", from, gt + 1);
        return;
    }
    char src[96], bak[104];
    fh1_users_data_dir(u.id, c->id, src, sizeof(src));
    fh1_users_data_dir(to.id, c->id, dir, sizeof(dir));
    snprintf(bak, sizeof(bak), "%s.bak", dir);
    fh1_rmtree(bak);
    rename(dir, bak);
    fh1_mkdirs(dir);
    int files = 0;
    long bytes = 0;
    DIR *d = opendir(src);
    struct dirent *e;
    static char buf[1024];
    while (d && (e = readdir(d)) != NULL) {
        char sp[192];
        if (e->d_name[0] == '.' || snprintf(sp, sizeof(sp), "%s/%.64s", src, e->d_name) >= (int)sizeof(sp)) continue;
        snprintf(p, sizeof(p), "%s/%.64s", dir, e->d_name);
        FILE *in = fopen(sp, "rb"), *out = in ? fopen(p, "wb") : NULL;
        size_t r;
        while (in && out && (r = fread(buf, 1, sizeof(buf), in)) > 0) bytes += (long)fwrite(buf, 1, r, out);
        if (in) fclose(in);
        if (out) fclose(out);
        files += in && out;
    }
    if (d) closedir(d);
    fh1_int_info_changed();
    snprintf(c->reply, n, "%s: %d files (%ld bytes) from %s to %s; %s's before in %s", c->id, files, bytes, u.name, to.name,
             to.name, bak);
}

static void run_cmd(void *arg)
{
    cmd_t *c = arg;
    char err[128] = "";
    if (strcmp(c->op, "demo_pads") == 0) {
        // Dev: pretend gamepads, one of each kind with a different colour and battery, to see the tray and the
        // Inputs and outputs window without real controllers ("op demo_pads off" removes them).
        static int handles[5] = { -1, -1, -1, -1, -1 };
        for (int i = 0; i < 5; i++) {
            if (handles[i] >= 0) fh1_gamepad_remove(handles[i]);
            handles[i] = -1;
        }
        if (strcmp(c->path, "off") != 0) {
            static const struct { uint8_t kind; uint32_t rgb; uint8_t battery; const char *name; } demo[] = {
                { FH1_PAD_PLAYSTATION, 0x0000ff, 230, "DualSense" }, { FH1_PAD_PRO, 0x3c4046, 150, "Pro Controller" },
                { FH1_PAD_JOYCON_L, 0x0ab9e6, 60, "Joy-Con (L)" }, { FH1_PAD_JOYCON_R, 0xff3c28, 15, "Joy-Con (R)" },
                { FH1_PAD_FLAT, 0xb0b4ba, 0, "8BitDo" },
            };
            for (int i = 0; i < 5; i++) {
                fh1_gamepad_info_t g = { .kind = demo[i].kind, .rgb = demo[i].rgb, .battery = demo[i].battery };
                snprintf(g.id, sizeof(g.id), "DE:MO:00:00:00:0%d", i);
                snprintf(g.name, sizeof(g.name), "%s", demo[i].name);
                snprintf(g.via, sizeof(g.via), "DEMO");
                handles[i] = fh1_gamepad_add(&g);
            }
        }
        snprintf(c->reply, sizeof(c->reply), "demo gamepads %s", strcmp(c->path, "off") == 0 ? "removed" : "added");
        xSemaphoreGive(c->done);
        return;
    }
    if (strcmp(c->op, "bridge_quality") == 0) {
        // Dev: the sound sent to bridges (id: 0 48 kHz stereo, 1 24 kHz stereo, 2 24 kHz mono).
        fh1_bridge_set_audio_quality((uint8_t)atoi(c->id));
        snprintf(c->reply, sizeof(c->reply), "bridge sound quality %u", fh1_bridge_audio_quality());
        xSemaphoreGive(c->done);
        return;
    }
    if (strcmp(c->op, "sound_out") == 0 || strcmp(c->op, "tone") == 0) {
        // Dev: choose the sound output (id: a device id, "" automatic), or play the test tone on it.
        if (strcmp(c->op, "sound_out") == 0) fh1_audio_set_output(c->id);
        else if (strcmp(c->path, "long") == 0) tone_long();
        else tone_cb(NULL);
        snprintf(c->reply, sizeof(c->reply), "sound: %s%s", fh1_audio_output()[0] ? fh1_audio_output() : "automatic",
                 strcmp(c->op, "tone") == 0 ? ", tone" : "");
        xSemaphoreGive(c->done);
        return;
    }
    if (strncmp(c->op, "user", 4) == 0) {
        users_cmd(c->op, c->id, c->path, c->reply, sizeof(c->reply));
        xSemaphoreGive(c->done);
        return;
    }
    if (strcmp(c->op, "assign") == 0) {
        // Dev: a player's gamepad as Inputs and outputs sets it. id: the player (1, 2); path: "", "fh1" or an id.
        const int p = atoi(c->id) - 1;
        fh1_gamepad_assign(p, c->path);
        char now[18];
        fh1_gamepad_assignment(p, now, sizeof(now));
        snprintf(c->reply, sizeof(c->reply), "player %d: %s", p + 1, now[0] ? now : "automatic");
        xSemaphoreGive(c->done);
        return;
    }
    if (strcmp(c->op, "start") == 0) {
        esp_err_t r = start_app(c->id, err, sizeof(err));
        snprintf(c->reply, sizeof(c->reply), r == ESP_OK ? "%s started" : "%s: %s", c->id, err);
    } else if (strcmp(c->op, "dev") == 0) {
        snprintf(c->reply, sizeof(c->reply), run_dev_push(c->path) ? "sent to %s" : "no app running (or its queue is full)",
                 run_current() ? run_current() : "");
    } else if (strcmp(c->op, "stop") == 0) {
        run_stop();
        snprintf(c->reply, sizeof(c->reply), "stopped");
    } else if (strcmp(c->op, "install") == 0) {
        do_install(c->path, c->reply, sizeof(c->reply));
    } else if (strcmp(c->op, "uninstall") == 0) {
        do_uninstall(c->id, c->reply, sizeof(c->reply));
    } else if (strcmp(c->op, "delete_pkg") == 0) {
        bool ok = (strncmp(c->path, FH1_INT_PACKAGES "/", strlen(FH1_INT_PACKAGES) + 1) == 0 ||
                   strncmp(c->path, FH1_SD_PACKAGES "/", strlen(FH1_SD_PACKAGES) + 1) == 0) && !strstr(c->path, "..");
        ok = ok && unlink(c->path) == 0;
        snprintf(c->reply, sizeof(c->reply), ok ? "package deleted" : "could not delete");
    } else if (strcmp(c->op, "delete_rom") == 0) {
        // A game file in <storage>/roms (path: /int/roms/<name> or /sd/roms/<name>, no subfolders).
        const char *slash = strrchr(c->path, '/');
        bool ok = (strncmp(c->path, FH1_INT_PATH "/roms/", strlen(FH1_INT_PATH "/roms/")) == 0 ||
                   strncmp(c->path, FH1_SD_PATH "/roms/", strlen(FH1_SD_PATH "/roms/")) == 0) &&
                  !strstr(c->path, "..") && slash && slash[1] && slash - c->path <= (int)strlen(FH1_INT_PATH "/roms");
        ok = ok && unlink(c->path) == 0;
        if (ok && strncmp(c->path, FH1_INT_PATH, strlen(FH1_INT_PATH)) == 0) fh1_int_info_changed();
        snprintf(c->reply, sizeof(c->reply), ok ? "game deleted" : "could not delete");
    } else if (strcmp(c->op, "stress") == 0) {
        // id: "<seconds> [psram] [flash] [ui]" (dev tool)
        int secs = atoi(c->id);
        esp_err_t r = stress_start(secs > 0 && secs <= 120 ? secs : 10, strstr(c->id, "psram") != NULL,
                                   strstr(c->id, "flash") != NULL, strstr(c->id, "ui") != NULL);
        snprintf(c->reply, sizeof(c->reply), r == ESP_OK ? "stress started" : "a stress test is already running");
    } else if (strcmp(c->op, "heap") == 0) {
        // Dev: internal RAM (free, largest, lowest ever) and the allocations that failed; "reset" counts them anew.
        fh1_mem_diag_report(c->reply, sizeof(c->reply), strcmp(c->path, "reset") == 0);
    } else if (strcmp(c->op, "data_list") == 0 || strcmp(c->op, "data_copy") == 0) {
        data_op(c);
    } else if (strcmp(c->op, "peek") == 0) {
        // 16 bytes of a file at an offset (dev tool); the offset rides in "id".
        FILE *f = fopen(c->path, "rb");
        uint8_t b[16];
        size_t r = 0;
        if (f) {
            fseek(f, atol(c->id), SEEK_SET);
            r = fread(b, 1, sizeof(b), f);
            fclose(f);
        }
        int p = snprintf(c->reply, sizeof(c->reply), "@%s:", c->id);
        for (size_t i = 0; i < r && p < (int)sizeof(c->reply) - 3; i++) p += snprintf(c->reply + p, sizeof(c->reply) - p, " %02x", b[i]);
    } else if (strcmp(c->op, "hash") == 0) {
        // Size and SHA-256 of a file, to check transfers (dev tool).
        FILE *f = fopen(c->path, "rb");
        if (!f) {
            snprintf(c->reply, sizeof(c->reply), "cannot open %s", c->path);
        } else {
            psa_crypto_init();
            psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
            psa_hash_setup(&op, PSA_ALG_SHA_256);
            uint8_t *buf = heap_caps_malloc(8192, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA), sha[32];   // see fh1_ota.c
            size_t total = 0, r, hl;
            psa_hash_operation_t blk = PSA_HASH_OPERATION_INIT;
            psa_hash_setup(&blk, PSA_ALG_SHA_256);
            while (buf && (r = fread(buf, 1, 8192, f)) > 0) {
                psa_hash_update(&op, buf, r);
                psa_hash_update(&blk, buf, r);
                total += r;
                if (total % 65536 == 0 || r < 8192) {      // a short hash per 64 KB block, in the log
                    uint8_t b[32];
                    size_t bl;
                    psa_hash_finish(&blk, b, sizeof(b), &bl);
                    ESP_LOGI(TAG, "hash block %u: %02x%02x%02x%02x", (unsigned)((total - 1) / 65536), b[0], b[1], b[2], b[3]);
                    blk = (psa_hash_operation_t)PSA_HASH_OPERATION_INIT;
                    psa_hash_setup(&blk, PSA_ALG_SHA_256);
                }
            }
            psa_hash_abort(&blk);
            heap_caps_free(buf);
            fclose(f);
            psa_hash_finish(&op, sha, sizeof(sha), &hl);
            char hex[65];
            for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", sha[i]);
            snprintf(c->reply, sizeof(c->reply), "%u bytes, sha256 %s", (unsigned)total, hex);
        }
    } else if (strcmp(c->op, "rescan") == 0) {
        snprintf(c->reply, sizeof(c->reply), "rescanned");
    } else if (strcmp(c->op, "int_format") == 0) {
        run_stop();
        esp_err_t r = fh1_int_format();
        scan_installed();
        snprintf(c->reply, sizeof(c->reply), "internal storage format: %s", esp_err_to_name(r));
    } else {
        snprintf(c->reply, sizeof(c->reply), "unknown app command");
    }
    scan_packages();
    refresh_all(NULL);
    xSemaphoreGive(c->done);
}

static void console_cmd(const cJSON *msg, char *reply, size_t n)
{
    cmd_t *c = calloc(1, sizeof(*c));
    if (!c) return;
    strlcpy(c->op, cJSON_GetStringValue(cJSON_GetObjectItem(msg, "op")) ?: "", sizeof(c->op));
    strlcpy(c->id, cJSON_GetStringValue(cJSON_GetObjectItem(msg, "id")) ?: "", sizeof(c->id));
    strlcpy(c->path, cJSON_GetStringValue(cJSON_GetObjectItem(msg, "path")) ?: "", sizeof(c->path));
    c->done = xSemaphoreCreateBinary();
    fh1_ui_async(run_cmd, c);
    if (xSemaphoreTake(c->done, pdMS_TO_TICKS(20000)) == pdTRUE) {
        strlcpy(reply, c->reply, n);
        vSemaphoreDelete(c->done);
        free(c);
    } else {
        strlcpy(reply, "still working; see the log", n);    // c is leaked on purpose: the UI task still holds it
    }
}

// Rescan packages now and then, so a file copied to the SD card shows up.
static void rescan_tick(lv_timer_t *t)
{
    scan_packages();
    installer_fill();
}

void fh1_apps_start(void)
{
    fh1_ui_set_sound_fn(ui_sound_play);
    s_lock = xSemaphoreCreateMutex();
    run_init();
    scan_installed();
    scan_packages();
    fh1_ui_set_menu_handler(open_launcher, NULL);
    music_init();
    users_ui_init(users_changed_menu);
    io_claim_init();                          // Menu held on any controller: it takes player 1
    net_start();                              // users between units (apps_net.c)
    // Games arriving over the console show as games for the GameBoy Color app.
    fh1_ui_register_file_type("gb", FH1_ICON_GAMEBOY, "Receiving a game", "Game received");
    fh1_ui_register_file_type("gbc", FH1_ICON_GAMEBOY, "Receiving a game", "Game received");
    fh1_ui_register_file_type("nes", FH1_ICON_NES, "Receiving a game", "Game received");
    fh1_ui_register_file_type("sms", FH1_ICON_SMS, "Receiving a game", "Game received");
    fh1_ui_register_file_type("gg", FH1_ICON_SMS, "Receiving a game", "Game received");
    fh1_ui_register_file_type("sg", FH1_ICON_SMS, "Receiving a game", "Game received");
    fh1_ui_on_devices_changed(on_devices_changed, NULL);
    fh1_console_set_apps_hooks(console_json, console_cmd);
    lv_timer_create(tabs_tick, 100, NULL);
    lv_timer_create(rescan_tick, 30000, NULL);     // uploads also trigger a rescan
    open_launcher(NULL);
    ESP_LOGI(TAG, "%d app(s) installed, %d package(s) staged", s_ninstalled, s_npkgs);
}
