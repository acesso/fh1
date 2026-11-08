// The Game Boy / Game Boy Color emulator (firmware/PLAN.md, "Emulators"). A session holds one loaded game:
// play it until MENU is pressed, change its options, play again, close it. The caller pauses the window
// system while it plays.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    FH1_GB_PLAY,          // paced at 59.7 frames/s (or turbo), until MENU
    FH1_GB_BENCHMARK,     // no frame limit: 10 s emulation only, then 10 s with drawing
} fh1_gb_mode_t;

typedef enum { FH1_GB_SCALE_FIT, FH1_GB_SCALE_1X, FH1_GB_SCALE_COUNT } fh1_gb_scale_t;
typedef enum { FH1_GB_DRAW_AUTO, FH1_GB_DRAW_EVERY, FH1_GB_DRAW_HALF, FH1_GB_DRAW_COUNT } fh1_gb_draw_t;
// For original Game Boy games: 0 = the Game Boy Color's own palette for this game (as a real GBC picks it
// from the title), 1-12 = the GBC's palettes chosen with a button held at power-on, 13-16 = one-colour
// classics (green, grey, sepia, ocean). The GBC palettes colour the background and each sprite group apart.
#define FH1_GB_PALETTES 17

// Options: change them between plays (from the options menu); they are saved.
typedef struct {
    uint8_t scale;        // fh1_gb_scale_t
    uint8_t palette;      // 0 .. FH1_GB_PALETTES - 1
    uint8_t drawing;      // fh1_gb_draw_t
    bool turbo;           // no frame limit
    bool show_fps;        // emulated / shown frames per second, above the picture
} fh1_gb_opts_t;

typedef struct {
    char title[17];
    bool color;
    float fps_emulation;       // benchmark: emulation alone
    float fps_with_display;    // emulation while frames are drawn on the other core
    float fps_shown;           // frames that reached the panel
} fh1_gb_result_t;

typedef struct fh1_gb fh1_gb_t;

fh1_gb_t *fh1_gb_open(const char *rom_path, fh1_gb_result_t *info, char *err, size_t errlen);
void fh1_gb_play(fh1_gb_t *g, fh1_gb_mode_t mode, fh1_gb_result_t *res);    // returns on MENU or at the end
fh1_gb_opts_t *fh1_gb_options(fh1_gb_t *g);
void fh1_gb_save_options(fh1_gb_t *g);
const char *fh1_gb_palette_name(int i);     // English; translate for display
void fh1_gb_reset(fh1_gb_t *g);
esp_err_t fh1_gb_save_state(fh1_gb_t *g);
esp_err_t fh1_gb_load_state(fh1_gb_t *g);   // ESP_ERR_NOT_FOUND when there is none
void fh1_gb_close(fh1_gb_t *g);             // writes the battery save
