// The Music app: the library (every track in <storage>/music and its folders, as a paged list) and the
// playlist logic; the full-screen deck is music_deck.c, the decoding fh1_player. Music plays on when the
// windows close, until Y (stop) or a game starts. A plays a track and opens the deck; X opens the deck.
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include "apps_priv.h"
#include "music_priv.h"
#include "fh1_ui.h"
#include "fh1_i18n.h"
#include "fh1_input.h"
#include "fh1_storage.h"
#include "fh1_player.h"
#include "fh1_settings.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_log.h"

static const char *TAG = "music";

typedef struct {
    char path[160];
    char title[64];
} track_t;

static track_t *s_list;                 // PSRAM; kept while music plays, even with the window closed
static int s_count, s_cap;
static int s_cur = -1;                  // the playlist entry playing
static fh1_win_t *s_win;
static lv_timer_t *s_timer;
static music_mode_t s_mode;

// ── The library ──

static void add(const char *path, const char *name)
{
    if (s_count == s_cap) {
        int cap = s_cap ? s_cap * 2 : 128;
        track_t *n = heap_caps_realloc(s_list, cap * sizeof(track_t), MALLOC_CAP_SPIRAM);
        if (!n) return;
        s_list = n;
        s_cap = cap;
    }
    track_t *t = &s_list[s_count++];
    snprintf(t->path, sizeof(t->path), "%s", path);
    const char *dot = strrchr(name, '.');
    int len = dot ? (int)(dot - name) : (int)strlen(name);
    snprintf(t->title, sizeof(t->title), "%.*s", len, name);
    for (char *p = t->title; *p; p++) {
        if (*p == '_') *p = ' ';
    }
}

// Tracks in dir and its folders (three levels: artist/album/track is common).
static void scan(const char *dir, int depth)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char path[160];
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || strlen(dir) + strlen(e->d_name) + 2 > sizeof(path)) continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (e->d_type == DT_DIR) {
            if (depth < 3) scan(path, depth + 1);
        } else if (fh1_player_supported(e->d_name)) {
            add(path, e->d_name);
        }
    }
    closedir(d);
}

// Folders together, then by file name: albums keep their order ("01 ...", "02 ...").
static int by_path(const void *a, const void *b) { return strcasecmp(((const track_t *)a)->path, ((const track_t *)b)->path); }

static void rescan(void)
{
    char playing[160] = "";
    if (s_cur >= 0 && s_cur < s_count) snprintf(playing, sizeof(playing), "%s", s_list[s_cur].path);
    s_count = 0;
    fh1_mkdirs(FH1_INT_PATH "/music");
    scan(FH1_INT_PATH "/music", 1);
    fh1_sd_info_t si;
    fh1_sd_info(&si);
    if (si.mounted) {
        fh1_mkdirs(FH1_SD_PATH "/music");
        scan(FH1_SD_PATH "/music", 1);
    }
    if (s_count > 1) qsort(s_list, s_count, sizeof(track_t), by_path);
    s_cur = -1;
    for (int i = 0; playing[0] && i < s_count; i++) {
        if (strcmp(s_list[i].path, playing) == 0) s_cur = i;
    }
}

// ── The playlist ──

static void play_index(int i)
{
    if (i < 0 || i >= s_count) return;
    s_cur = i;
    fh1_player_play(s_list[i].path);
    if (s_win) fh1_ui_list_refresh(s_win, s_count);
}

static int pick_next(int step, bool by_user)
{
    if (s_count <= 0) return -1;
    if (s_mode == MUSIC_SHUFFLE && s_count > 1) {
        int n;
        do n = (int)(esp_random() % s_count);
        while (n == s_cur);
        return n;
    }
    if (s_mode == MUSIC_REPEAT_ONE && !by_user) return s_cur;
    return ((s_cur < 0 ? 0 : s_cur + step) % s_count + s_count) % s_count;
}

static void ended_ui(void *arg) { play_index(pick_next(1, false)); }
static void on_end(void *user) { fh1_ui_async(ended_ui, NULL); }     // player task -> UI task

static void step_ui(void *arg) { play_index(pick_next((int)(intptr_t)arg, true)); }

// From the deck's task.
void music_step(int step) { fh1_ui_async(step_ui, (void *)(intptr_t)step); }
static void current_ui(void *arg) { play_index(s_cur >= 0 ? s_cur : 0); }
void music_play_current(void) { fh1_ui_async(current_ui, NULL); }
music_mode_t music_mode(void) { return s_mode; }
void music_set_mode(music_mode_t m)
{
    s_mode = m % MUSIC_MODES;
    fh1_settings_set_u8("music", "mode", (uint8_t)s_mode);
}
void music_position(int *index, int *count)
{
    *index = s_cur;
    *count = s_count;
}

void music_stop_for_game(void)
{
    fh1_player_status_t st;
    fh1_player_status(&st);
    if (st.state != FH1_PLAYER_STOPPED) {
        ESP_LOGI(TAG, "a game starts: the music stops");
        fh1_player_stop();
    }
}

// ── The window ──

static void title_update(lv_timer_t *t)
{
    if (!s_win) return;
    fh1_player_status_t st;
    fh1_player_status(&st);
    char title[48];
    if (st.state == FH1_PLAYER_PLAYING || st.state == FH1_PLAYER_PAUSED) {
        snprintf(title, sizeof(title), "%s %s %d/%d", T("Music"), st.state == FH1_PLAYER_PAUSED ? "||" : ">",
                 s_cur + 1, s_count);
    } else {
        snprintf(title, sizeof(title), "%s", T("Music"));
    }
    fh1_win_set_title(s_win, title);
}

static void list_item(int i, char *text, size_t len, int *icon, void *u)
{
    snprintf(text, len, "%s%s", i == s_cur ? "\xC2\xBB " : "", s_list[i].title);     // "» " marks the one playing
    *icon = strncmp(s_list[i].path, FH1_SD_PATH "/", 4) == 0 ? FH1_ICON_SD : FH1_ICON_INTERNAL;
}

static void pick(int i, void *u)
{
    fh1_player_status_t st;
    fh1_player_status(&st);
    // The track playing: back to the deck without starting over.
    if (!(i == s_cur && (st.state == FH1_PLAYER_PLAYING || st.state == FH1_PLAYER_PAUSED))) play_index(i);
    music_deck_open();
}

static bool list_key(fh1_win_t *w, int key, void *u)
{
    if (key == FH1_KEY_X) {
        music_deck_open();
        return true;
    }
    if (key == FH1_KEY_Y) {
        fh1_player_stop();
        s_cur = -1;
        fh1_ui_list_refresh(w, s_count);
        title_update(NULL);
        return true;
    }
    return false;
}

static void list_closed(fh1_win_t *w, void *u)
{
    if (w != s_win) return;
    s_win = NULL;
    lv_timer_delete(s_timer);
    s_timer = NULL;
}

void music_open(void)
{
    if (s_win && fh1_win_is_open(s_win)) {
        fh1_win_focus(s_win);
        return;
    }
    rescan();
    s_win = fh1_win_create(T("Music"), 230, 180);
    if (!s_win) return;
    const fh1_hint_t h[] = { { FH1_KEY_A, T("Listen") }, { FH1_KEY_X, T("Deck") }, { FH1_KEY_Y, T("Stop") },
                             { FH1_HINT_LEFTRIGHT, T("Page") }, { FH1_KEY_B, T("Close") } };
    fh1_win_set_hints(s_win, h, 5);
    fh1_win_on_key(s_win, list_key, NULL);
    fh1_win_on_close(s_win, list_closed, NULL);
    fh1_ui_list(s_win, s_count, list_item, pick, NULL);
    if (s_cur >= 0) fh1_ui_list_go(s_win, s_cur);
    if (!s_count) fh1_ui_line(fh1_win_body(s_win), T("No music (.mp3 .flac .ogg .wav .mod in /sd/music)"));
    s_timer = lv_timer_create(title_update, 1000, NULL);
    title_update(NULL);
}

// The deck closed: the list shows the track playing.
void music_deck_closed(void)
{
    if (!s_win) return;
    fh1_ui_list_refresh(s_win, s_count);
    if (s_cur >= 0) fh1_ui_list_go(s_win, s_cur);
    title_update(NULL);
}

void music_init(void)
{
    s_mode = fh1_settings_get_u8("music", "mode", MUSIC_REPEAT_ALL) % MUSIC_MODES;
    fh1_player_on_end(on_end, NULL);
    const char *const exts[] = { "mp3", "flac", "ogg", "oga", "wav", "mod" };
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++)
        fh1_ui_register_file_type(exts[i], FH1_ICON_MUSIC, "Receiving music", "Music received");
}
