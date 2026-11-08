// The Sega Master System / Game Gear emulator: SMS Plus (from retro-go, vendored), the same shape as the
// NES one (fh1_nes.h). One session at a time.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum { FH1_SMS_PLAY, FH1_SMS_BENCHMARK } fh1_sms_mode_t;
typedef enum { FH1_SMS_DRAW_AUTO, FH1_SMS_DRAW_EVERY, FH1_SMS_DRAW_HALF, FH1_SMS_DRAW_COUNT } fh1_sms_draw_t;

typedef struct {
    uint8_t drawing;      // fh1_sms_draw_t
    bool turbo;
    bool show_fps;
    bool sprite_limit;    // 8 sprites per line, as the console
} fh1_sms_opts_t;

typedef struct {
    char title[40];
    bool pal;             // 50 Hz
    bool game_gear;
    float fps_emulation, fps_with_display, fps_shown;
} fh1_sms_result_t;

typedef struct fh1_sms fh1_sms_t;

fh1_sms_t *fh1_sms_open(const char *rom_path, fh1_sms_result_t *info, char *err, size_t errlen);
void fh1_sms_play(fh1_sms_t *s, fh1_sms_mode_t mode, fh1_sms_result_t *res);
fh1_sms_opts_t *fh1_sms_options(fh1_sms_t *s);
void fh1_sms_save_options(fh1_sms_t *s);
void fh1_sms_reset(fh1_sms_t *s);
esp_err_t fh1_sms_save_state(fh1_sms_t *s);
esp_err_t fh1_sms_load_state(fh1_sms_t *s);      // ESP_ERR_NOT_FOUND when there is none
void fh1_sms_close(fh1_sms_t *s);                // writes the cartridge RAM if the game used it
