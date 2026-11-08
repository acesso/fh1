// The game library apps, one per console, all the same code: a paged list of the console's games in
// /int/roms and /sd/roms (thousands are fine), A plays, X runs a speed test; MENU in a game opens its
// options over the frozen picture. An event's overlay pauses the game and shows over it; a system restart
// closes the game (saving) first. Each console is a descriptor below (game_sys_t).
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <dirent.h>
#include "apps_priv.h"
#include "fh1_ui.h"
#include "fh1_input.h"
#include "fh1_storage.h"
#include "fh1_i18n.h"
#include "fh1_app.h"
#include "fh1_text.h"
#include "fh1_gb.h"
#include "fh1_nes.h"
#include "fh1_sms.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct {
    char title[40];
    const char *kind;          // "Color", "PAL", ...
    int rate;                  // the console's frames per second
    float emu, draw, shown;
} game_res_t;

typedef struct {
    const char *name;                              // window titles, messages
    const fh1_ui_span_t *spans;                    // or a rich title (GameBoy Color)
    int nspans;
    const char *const *exts;                       // without the dot, NULL-terminated
    void (*title)(const char *path, char *out, size_t n);
    void *(*open)(const char *path, game_res_t *r, char *err, size_t n);
    void (*play)(void *g, bool bench, game_res_t *r);
    void (*close)(void *g);
    void (*reset)(void *g);
    esp_err_t (*save_state)(void *g);
    esp_err_t (*load_state)(void *g);
    int nopts;
    void (*opt_text)(void *g, int i, char *buf, size_t n);    // "Palette: green", translated
    void (*opt_next)(void *g, int i);                          // next value, saved
} game_sys_t;

// ── The list ──

typedef struct {
    char path[100];
    char title[40];
} entry_t;

static const game_sys_t *s_sys;          // the console whose list is open
static fh1_win_t *s_win, *s_menu;
static entry_t *s_list;                  // PSRAM
static int s_count, s_cap;
static void *s_game;                     // the loaded game, kept between plays
static const game_sys_t *s_game_sys;
static int s_game_idx = -1;
static bool s_bench;
static game_res_t s_res;
static lv_obj_t *s_opt_btn[8];

static bool has_ext(const game_sys_t *sys, const char *name)
{
    const char *dot = strrchr(name, '.');
    for (int i = 0; dot && sys->exts[i]; i++) {
        if (strcasecmp(dot + 1, sys->exts[i]) == 0) return true;
    }
    return false;
}

static void scan(const game_sys_t *sys, const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) {
        if (!has_ext(sys, e->d_name) || strlen(dir) + strlen(e->d_name) + 2 > sizeof(s_list[0].path)) continue;
        if (s_count == s_cap) {
            int cap = s_cap ? s_cap * 2 : 64;
            entry_t *n = heap_caps_realloc(s_list, cap * sizeof(entry_t), MALLOC_CAP_SPIRAM);
            if (!n) break;
            s_list = n;
            s_cap = cap;
        }
        entry_t *it = &s_list[s_count++];
        snprintf(it->path, sizeof(it->path), "%s/%s", dir, e->d_name);
        sys->title(it->path, it->title, sizeof(it->title));
    }
    if (d) closedir(d);
}

static int by_title(const void *a, const void *b) { return strcasecmp(((const entry_t *)a)->title, ((const entry_t *)b)->title); }

static void list_item(int i, char *text, size_t len, int *icon, void *u)
{
    snprintf(text, len, "%s", s_list[i].title);
    *icon = strncmp(s_list[i].path, FH1_SD_PATH "/", 4) == 0 ? FH1_ICON_SD : FH1_ICON_INTERNAL;
}

// ── Playing ──

static void open_menu(void);

static void close_game(void)
{
    if (!s_game) return;
    s_game_sys->close(s_game);
    s_game = NULL;
    s_game_idx = -1;
}

static void after_play(void *arg)
{
    if (s_bench) {
        close_game();
        char t[220];
        snprintf(t, sizeof(t), T("%s (%s)\nEmulation only: %.1f fps\nWhile drawing: %.1f fps\nShown: %.1f fps\nReal speed: %d fps"),
                 s_res.title, s_res.kind, s_res.emu, s_res.draw, s_res.shown, s_res.rate);
        fh1_ui_message(T("Speed test"), t);
    } else if (s_game) {
        open_menu();
    }
}

static void play_task(void *arg)
{
    for (;;) {
        s_game_sys->play(s_game, s_bench, &s_res);
        if (fh1_app_quit_requested()) break;
        if (!s_bench && fh1_app_should_yield()) {      // an overlay: show it over the game, then go on
            fh1_ui_lend_screen();
            if (!fh1_app_quit_requested()) continue;
        }
        break;
    }
    if (fh1_app_quit_requested()) {                    // shutting down: save and give the screen back
        close_game();
        fh1_ui_pause(false, true);
    } else {
        fh1_ui_pause(false, s_bench);
        fh1_ui_async(after_play, NULL);
    }
    vTaskDelete(NULL);
}

static void resume(void)
{
    fh1_ui_pause(true, false);
    if (xTaskCreatePinnedToCore(play_task, "game", 6144, NULL, 4, NULL, 1) != pdPASS) {
        fh1_ui_pause(false, true);
        fh1_ui_message(s_game_sys->name, T("No memory to start the emulator."));
    }
}

static void start(int i, bool bench)
{
    if (i < 0 || i >= s_count) return;
    music_stop_for_game();             // the game's sound takes the output
    if (s_game && (s_game_idx != i || s_game_sys != s_sys)) close_game();
    if (!s_game) {
        // Freeze the window system first: the game's hot data gets the internal RAM it gives back.
        char err[96];
        fh1_ui_pause(true, false);
        s_game = s_sys->open(s_list[i].path, &s_res, err, sizeof(err));
        if (!s_game) {
            fh1_ui_pause(false, true);
            fh1_ui_message(s_sys->name, err);
            return;
        }
        s_game_sys = s_sys;
        s_game_idx = i;
    }
    s_bench = bench;
    resume();
}

// ── The options menu, over the frozen game ──

static void opt_labels(void)
{
    for (int i = 0; s_menu && i < s_game_sys->nopts; i++) {
        char t[64];
        s_game_sys->opt_text(s_game, i, t, sizeof(t));
        fh1_ui_button_set_text(s_opt_btn[i], t);
    }
}

static void opt_cb(void *u)
{
    s_game_sys->opt_next(s_game, (int)(intptr_t)u);
    opt_labels();
}

static void menu_resume(void *u)
{
    fh1_win_t *w = s_menu;
    s_menu = NULL;
    if (w) fh1_win_close(w);
    resume();
}

static void menu_save(void *u)
{
    fh1_ui_message(s_game_sys->name, s_game_sys->save_state(s_game) == ESP_OK ? T("State saved.") : T("Could not save the state."));
}

static void menu_load(void *u)
{
    esp_err_t r = s_game_sys->load_state(s_game);
    if (r == ESP_OK) menu_resume(NULL);
    else fh1_ui_message(s_game_sys->name, r == ESP_ERR_NOT_FOUND ? T("No saved state.") : T("Could not load the state."));
}

static void reset_yes(bool yes, void *u)
{
    if (!yes) return;
    s_game_sys->reset(s_game);
    menu_resume(NULL);
}
static void menu_reset(void *u) { fh1_ui_confirm(T("Reset game"), T("Reset the game? Unsaved progress is lost."), reset_yes, NULL); }

static void menu_quit(void *u)
{
    fh1_win_t *w = s_menu;
    s_menu = NULL;
    if (w) fh1_win_close(w);
    close_game();
    fh1_ui_pause(false, true);          // the whole desktop back
}

static bool menu_key(fh1_win_t *w, int key, void *u)
{
    if (key == FH1_KEY_B || key == FH1_KEY_MENU) {
        menu_resume(NULL);
        return true;
    }
    return false;
}

static void menu_closed(fh1_win_t *w, void *u)
{
    if (s_menu == w) s_menu = NULL;
}

static void open_menu(void)
{
    s_menu = fh1_win_create(s_res.title[0] ? s_res.title : s_game_sys->name, 200, 220);
    const fh1_hint_t mh[] = { { FH1_KEY_A, T("Choose") }, { FH1_KEY_B, T("Back") } };
    fh1_win_set_hints(s_menu, mh, 2);
    fh1_win_on_key(s_menu, menu_key, NULL);
    fh1_win_on_close(s_menu, menu_closed, NULL);
    fh1_ui_button(s_menu, NULL, T("Resume"), menu_resume, NULL);
    for (int i = 0; i < s_game_sys->nopts && i < 8; i++)
        s_opt_btn[i] = fh1_ui_button(s_menu, NULL, "", opt_cb, (void *)(intptr_t)i);
    fh1_ui_button(s_menu, NULL, T("Save state"), menu_save, NULL);
    fh1_ui_button(s_menu, NULL, T("Load state"), menu_load, NULL);
    fh1_ui_button(s_menu, NULL, T("Reset game"), menu_reset, NULL);
    fh1_ui_button(s_menu, NULL, T("Quit game"), menu_quit, NULL);
    opt_labels();
}

// ── The list window ──

static void pick(int i, void *u) { start(i, false); }

static bool list_key(fh1_win_t *w, int key, void *u)
{
    if (key != FH1_KEY_X) return false;
    start(fh1_ui_list_index(w), true);
    return true;
}

static void list_closed(fh1_win_t *w, void *u)
{
    if (w == s_win) s_win = NULL;
}

static void open_library(const game_sys_t *sys)
{
    if (s_win && fh1_win_is_open(s_win)) fh1_win_close(s_win);
    if (s_game) close_game();        // a game left open (should not happen): the list may change
    s_sys = sys;
    s_count = 0;
    fh1_mkdirs(FH1_INT_PATH "/roms");
    scan(sys, FH1_INT_PATH "/roms");
    fh1_sd_info_t si;
    fh1_sd_info(&si);
    if (si.mounted) scan(sys, FH1_SD_PATH "/roms");
    if (s_count > 1) qsort(s_list, s_count, sizeof(entry_t), by_title);
    s_win = fh1_win_create(sys->name, 230, 180);
    if (!s_win) return;
    if (sys->spans) fh1_win_set_title_rich(s_win, sys->spans, sys->nspans);
    const fh1_hint_t h[] = { { FH1_KEY_A, T("Play") }, { FH1_KEY_X, T("Test") }, { FH1_HINT_LEFTRIGHT, T("Page") },
                             { FH1_KEY_B, T("Close") } };
    fh1_win_set_hints(s_win, h, 4);
    fh1_win_on_key(s_win, list_key, NULL);
    fh1_win_on_close(s_win, list_closed, NULL);
    fh1_ui_list(s_win, s_count, list_item, pick, NULL);
    if (!s_count) {
        char t[96];
        snprintf(t, sizeof(t), T("No games (.%s in /sd/roms)"), sys->exts[0]);
        fh1_ui_line(fh1_win_body(s_win), t);
    }
}

// ── Consoles ──

static const char *const s_draw_names[] = { "auto", "every frame", "half" };

static void opt_onoff(char *buf, size_t n, const char *fmt, bool on) { snprintf(buf, n, T(fmt), on ? T("on") : T("off")); }

// GameBoy Color: titles from the cartridge header (0x134..0x142), "POKEMON RED" -> "Pokemon Red".
static void gb_title(const char *path, char *out, size_t n)
{
    uint8_t h[16] = { 0 };
    FILE *f = fopen(path, "rb");
    if (f) {
        if (fseek(f, 0x134, SEEK_SET) != 0 || fread(h, 1, 15, f) != 15) h[0] = 0;
        fclose(f);
    }
    size_t k = 0;
    bool word_start = true;
    for (int i = 0; i < 15 && h[i] >= 0x20 && h[i] < 0x7f && k < n - 1; i++) {
        char c = (char)h[i];
        out[k++] = word_start ? toupper((unsigned char)c) : tolower((unsigned char)c);
        word_start = (c == ' ' || c == '-');
    }
    while (k && out[k - 1] == ' ') k--;
    out[k] = 0;
    if (!k) fh1_title_from_filename(path, out, n);
}

static void *gb_open(const char *path, game_res_t *r, char *err, size_t n)
{
    fh1_gb_result_t info;
    fh1_gb_t *g = fh1_gb_open(path, &info, err, n);
    if (g) {
        gb_title(path, r->title, sizeof(r->title));
        r->kind = info.color ? "Color" : "DMG";
        r->rate = 60;
    }
    return g;
}

static void gb_play(void *g, bool bench, game_res_t *r)
{
    fh1_gb_result_t res = { 0 };
    fh1_gb_play(g, bench ? FH1_GB_BENCHMARK : FH1_GB_PLAY, &res);
    r->emu = res.fps_emulation;
    r->draw = res.fps_with_display;
    r->shown = res.fps_shown;
}

static void gb_opt_text(void *g, int i, char *b, size_t n)
{
    fh1_gb_opts_t *o = fh1_gb_options(g);
    switch (i) {
    case 0: snprintf(b, n, T("Picture: %s"), o->scale == FH1_GB_SCALE_FIT ? T("fit") : "1x"); break;
    case 1: snprintf(b, n, T("Palette: %s"), T(fh1_gb_palette_name(o->palette))); break;
    case 2: snprintf(b, n, T("Drawing: %s"), T(s_draw_names[o->drawing % 3])); break;
    case 3: snprintf(b, n, T("Speed: %s"), o->turbo ? T("turbo") : T("normal")); break;
    default: opt_onoff(b, n, "FPS: %s", o->show_fps); break;
    }
}

static void gb_opt_next(void *g, int i)
{
    fh1_gb_opts_t *o = fh1_gb_options(g);
    switch (i) {
    case 0: o->scale = (o->scale + 1) % FH1_GB_SCALE_COUNT; break;
    case 1: o->palette = (o->palette + 1) % FH1_GB_PALETTES; break;
    case 2: o->drawing = (o->drawing + 1) % FH1_GB_DRAW_COUNT; break;
    case 3: o->turbo = !o->turbo; break;
    default: o->show_fps = !o->show_fps; break;
    }
    fh1_gb_save_options(g);
}

static const char *const s_gb_exts[] = { "gb", "gbc", NULL };
static const game_sys_t s_gb_sys = {
    .name = "GameBoy Color", .spans = s_gbc_name, .nspans = 6, .exts = s_gb_exts, .title = gb_title,
    .open = gb_open, .play = gb_play, .close = (void (*)(void *))fh1_gb_close, .reset = (void (*)(void *))fh1_gb_reset,
    .save_state = (esp_err_t (*)(void *))fh1_gb_save_state, .load_state = (esp_err_t (*)(void *))fh1_gb_load_state,
    .nopts = 5, .opt_text = gb_opt_text, .opt_next = gb_opt_next,
};

// NES
static void *nes_open(const char *path, game_res_t *r, char *err, size_t n)
{
    fh1_nes_result_t info;
    fh1_nes_t *g = fh1_nes_open(path, &info, err, n);
    if (g) {
        snprintf(r->title, sizeof(r->title), "%s", info.title);
        r->kind = info.pal ? "PAL" : "NTSC";
        r->rate = info.pal ? 50 : 60;
    }
    return g;
}

static void nes_play(void *g, bool bench, game_res_t *r)
{
    fh1_nes_result_t res = { 0 };
    fh1_nes_play(g, bench ? FH1_NES_BENCHMARK : FH1_NES_PLAY, &res);
    r->emu = res.fps_emulation;
    r->draw = res.fps_with_display;
    r->shown = res.fps_shown;
}

static void nes_opt_text(void *g, int i, char *b, size_t n)
{
    fh1_nes_opts_t *o = fh1_nes_options(g);
    switch (i) {
    case 0: snprintf(b, n, T("Palette: %s"), T(fh1_nes_palette_name(o->palette))); break;
    case 1: snprintf(b, n, T("Drawing: %s"), T(s_draw_names[o->drawing % 3])); break;
    case 2: snprintf(b, n, T("Speed: %s"), o->turbo ? T("turbo") : T("normal")); break;
    case 3: snprintf(b, n, T("Sprites: %s"), o->sprite_limit ? T("8 per line") : T("no limit")); break;
    default: opt_onoff(b, n, "FPS: %s", o->show_fps); break;
    }
}

static void nes_opt_next(void *g, int i)
{
    fh1_nes_opts_t *o = fh1_nes_options(g);
    switch (i) {
    case 0: o->palette = (o->palette + 1) % FH1_NES_PALETTES; break;
    case 1: o->drawing = (o->drawing + 1) % FH1_NES_DRAW_COUNT; break;
    case 2: o->turbo = !o->turbo; break;
    case 3: o->sprite_limit = !o->sprite_limit; break;
    default: o->show_fps = !o->show_fps; break;
    }
    fh1_nes_save_options(g);
}

static const char *const s_nes_exts[] = { "nes", NULL };
static const game_sys_t s_nes_sys = {
    .name = "NES", .exts = s_nes_exts, .title = fh1_title_from_filename,
    .open = nes_open, .play = nes_play, .close = (void (*)(void *))fh1_nes_close, .reset = (void (*)(void *))fh1_nes_reset,
    .save_state = (esp_err_t (*)(void *))fh1_nes_save_state, .load_state = (esp_err_t (*)(void *))fh1_nes_load_state,
    .nopts = 5, .opt_text = nes_opt_text, .opt_next = nes_opt_next,
};

// Master System / Game Gear
static void *sms_open(const char *path, game_res_t *r, char *err, size_t n)
{
    fh1_sms_result_t info;
    fh1_sms_t *g = fh1_sms_open(path, &info, err, n);
    if (g) {
        snprintf(r->title, sizeof(r->title), "%s", info.title);
        r->kind = info.game_gear ? "Game Gear" : info.pal ? "PAL" : "NTSC";
        r->rate = info.pal ? 50 : 60;
    }
    return g;
}

static void sms_play(void *g, bool bench, game_res_t *r)
{
    fh1_sms_result_t res = { 0 };
    fh1_sms_play(g, bench ? FH1_SMS_BENCHMARK : FH1_SMS_PLAY, &res);
    r->emu = res.fps_emulation;
    r->draw = res.fps_with_display;
    r->shown = res.fps_shown;
}

static void sms_opt_text(void *g, int i, char *b, size_t n)
{
    fh1_sms_opts_t *o = fh1_sms_options(g);
    switch (i) {
    case 0: snprintf(b, n, T("Drawing: %s"), T(s_draw_names[o->drawing % 3])); break;
    case 1: snprintf(b, n, T("Speed: %s"), o->turbo ? T("turbo") : T("normal")); break;
    case 2: snprintf(b, n, T("Sprites: %s"), o->sprite_limit ? T("8 per line") : T("no limit")); break;
    default: opt_onoff(b, n, "FPS: %s", o->show_fps); break;
    }
}

static void sms_opt_next(void *g, int i)
{
    fh1_sms_opts_t *o = fh1_sms_options(g);
    switch (i) {
    case 0: o->drawing = (o->drawing + 1) % FH1_SMS_DRAW_COUNT; break;
    case 1: o->turbo = !o->turbo; break;
    case 2: o->sprite_limit = !o->sprite_limit; break;
    default: o->show_fps = !o->show_fps; break;
    }
    fh1_sms_save_options(g);
}

static const char *const s_sms_exts[] = { "sms", "gg", "sg", NULL };
static const game_sys_t s_sms_sys = {
    .name = "Master System", .exts = s_sms_exts, .title = fh1_title_from_filename,
    .open = sms_open, .play = sms_play, .close = (void (*)(void *))fh1_sms_close, .reset = (void (*)(void *))fh1_sms_reset,
    .save_state = (esp_err_t (*)(void *))fh1_sms_save_state, .load_state = (esp_err_t (*)(void *))fh1_sms_load_state,
    .nopts = 4, .opt_text = sms_opt_text, .opt_next = sms_opt_next,
};

void games_open_gameboy(void) { open_library(&s_gb_sys); }
void games_open_nes(void) { open_library(&s_nes_sys); }
void games_open_sms(void) { open_library(&s_sms_sys); }
