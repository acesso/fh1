// The NES emulator: nofrendo (from retro-go, vendored), the same shape as the Game Boy one (fh1_gb.h). A
// session holds one loaded game: play it until MENU is pressed, change its options, play again, close it.
// The caller pauses the window system while it plays. One session at a time (nofrendo is a single machine).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    FH1_NES_PLAY,          // paced at the console's rate (60 or 50 frames/s) or turbo, until MENU
    FH1_NES_BENCHMARK,     // no frame limit: 10 s emulation only, then 10 s with drawing
} fh1_nes_mode_t;

typedef enum { FH1_NES_DRAW_AUTO, FH1_NES_DRAW_EVERY, FH1_NES_DRAW_HALF, FH1_NES_DRAW_COUNT } fh1_nes_draw_t;
#define FH1_NES_PALETTES 6

typedef struct {
    uint8_t palette;      // 0 .. FH1_NES_PALETTES - 1
    uint8_t drawing;      // fh1_nes_draw_t
    bool turbo;           // no frame limit
    bool show_fps;
    bool sprite_limit;    // 8 sprites per line, as the console (less flicker when off)
} fh1_nes_opts_t;

typedef struct {
    char title[40];       // from the file name, without tags like "(Europe)"
    bool pal;             // 50 Hz console
    int mapper;
    float fps_emulation, fps_with_display, fps_shown;
} fh1_nes_result_t;

typedef struct fh1_nes fh1_nes_t;

fh1_nes_t *fh1_nes_open(const char *rom_path, fh1_nes_result_t *info, char *err, size_t errlen);
void fh1_nes_play(fh1_nes_t *n, fh1_nes_mode_t mode, fh1_nes_result_t *res);    // returns on MENU or at the end
fh1_nes_opts_t *fh1_nes_options(fh1_nes_t *n);
void fh1_nes_save_options(fh1_nes_t *n);       // and applies them
const char *fh1_nes_palette_name(int i);       // English; translate for display
void fh1_nes_reset(fh1_nes_t *n);
esp_err_t fh1_nes_save_state(fh1_nes_t *n);
esp_err_t fh1_nes_load_state(fh1_nes_t *n);    // ESP_ERR_NOT_FOUND when there is none
void fh1_nes_close(fh1_nes_t *n);              // writes the battery save
// A display title from a file name: "Metroid (Europe) (Virtual Console).nes" -> "Metroid".
void fh1_nes_title_from_name(const char *file, char *out, size_t len);
