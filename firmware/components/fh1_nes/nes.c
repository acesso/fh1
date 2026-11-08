// The NES emulator: nofrendo on core 1, a blitter on core 0 that turns finished 8-bit frames into the
// screen's colours, options changed between plays from the options menu. Same shape as fh1_gb.
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "fh1_nes.h"
#include "fh1_board.h"
#include "fh1_input.h"
#include "fh1_settings.h"
#include "fh1_storage.h"
#include "fh1_audio.h"
#include "fh1_mem.h"
#include "fh1_app.h"
#include "fh1_text.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nofrendo.h"

static const char *TAG = "nes";
static const char *const s_palette_names[FH1_NES_PALETTES] = { "nofrendo", "composite", "NES Classic", "NTSC", "PVM", "smooth" };
#define BORDER 0x0000
#define FRAME_BYTES (NES_SCREEN_PITCH * NES_SCREEN_HEIGHT)

struct fh1_nes {
    nes_t *nes;
    fh1_audio_t *audio;          // the audio.out class: silent (and the APU unmixed) while no device is ready
    char save_base[112];         // <data dir>/nes/<title> (fh1_data_dir)
    fh1_nes_opts_t opts;
    uint16_t *pal;               // 256 entries, RGB565, internal RAM when there is room
    // Display: 1:1, cropped to fit (the console's overscan lines, and 8 columns a side on a 240-wide screen)
    uint16_t *fb;
    int fb_w, fb_h, x0, y0, out_w, out_h, src_x, src_y;
    // Frames: nofrendo draws one 8-bit frame on core 1 while the blitter (core 0) converts the other.
    uint8_t *frame[2];
    int back;
    volatile int ready;
    volatile bool blit_busy, stop;
    SemaphoreHandle_t go, gone;
    volatile uint32_t shown;
    bool half_rate, draw;
    uint32_t frame_no;
};

static fh1_nes_t *s_open;        // nofrendo is one machine: one session at a time

// ── Drawing ──

static void blit_task(void *arg)
{
    fh1_nes_t *n = arg;
    uint16_t *row = fh1_mem_fast(NES_SCREEN_WIDTH * 2);
    while (!n->stop && row) {
        if (xSemaphoreTake(n->go, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        const uint8_t *src = n->frame[n->ready];
        const uint16_t *pal = n->pal;
        for (int y = 0; y < n->out_h; y++) {
            const uint8_t *s = NES_SCREEN_GETPTR(src, n->src_x, n->src_y + y);
            for (int x = 0; x < n->out_w; x++) row[x] = pal[s[x]];
            memcpy(n->fb + (n->y0 + y) * n->fb_w + n->x0, row, n->out_w * 2);
        }
        n->shown++;
        n->blit_busy = false;
    }
    fh1_mem_free(row);
    xSemaphoreGive(n->gone);
    vTaskDelete(NULL);
}

static void layout(fh1_nes_t *n)
{
    const int crop_v = n->nes->overscan;                   // 8 lines top and bottom on NTSC, none on PAL
    n->out_h = NES_SCREEN_HEIGHT - 2 * crop_v;
    if (n->out_h > n->fb_h) n->out_h = n->fb_h;
    n->src_y = (NES_SCREEN_HEIGHT - n->out_h) / 2;
    n->out_w = NES_SCREEN_WIDTH <= n->fb_w ? NES_SCREEN_WIDTH : n->fb_w;   // 240 wide: 8 columns off each side
    n->src_x = (NES_SCREEN_WIDTH - n->out_w) / 2;
    n->x0 = (n->fb_w - n->out_w) / 2;
    n->y0 = (n->fb_h - n->out_h) / 2;
}

static void fill_fb(fh1_nes_t *n, uint16_t c)
{
    for (int i = 0; i < n->fb_w * n->fb_h; i++) n->fb[i] = c;
}

static void build_palette(fh1_nes_t *n)
{
    uint16_t *p = nofrendo_buildpalette(n->opts.palette % FH1_NES_PALETTES, 16);   // fast memory, via nes_alloc.h
    if (!p) return;
    uint16_t *old = n->pal;
    n->pal = p;
    if (old) fh1_mem_free(old);
}

// ── FPS: a 3 x 5 pixel font for digits, in the border ──

static const uint16_t s_digits[12] = {
    075557, 026227, 071747, 071717, 055711, 074717, 074757, 071111, 075757, 075717, 000000, 001240,
};

static void draw_text(fh1_nes_t *n, int x, int y, const char *s)
{
    for (; *s; s++, x += 4) {
        int g = (*s >= '0' && *s <= '9') ? *s - '0' : (*s == '/' ? 11 : 10);
        for (int r = 0; r < 5; r++) {
            for (int c = 0; c < 4; c++) {
                bool on = c < 3 && ((s_digits[g] >> (3 * (4 - r) + (2 - c))) & 1);
                if (y + r < n->fb_h && x + c < n->fb_w) n->fb[(y + r) * n->fb_w + x + c] = on ? 0xFFE0 : BORDER;
            }
        }
    }
}

static void show_fps(fh1_nes_t *n, float emulated, float shown)
{
    char t[16];
    snprintf(t, sizeof(t), "%2d/%2d", (int)(emulated + 0.5f), (int)(shown + 0.5f));
    // In the border above the picture when there is one, else over its top-left corner.
    draw_text(n, n->x0 + 2, n->y0 >= 7 ? (n->y0 - 5) / 2 : 1, t);
}

// ── Titles and saves ──

void fh1_nes_title_from_name(const char *file, char *out, size_t len) { fh1_title_from_filename(file, out, len); }

static void make_save_base(fh1_nes_t *n, const char *title)
{
    char name[40];
    size_t o = 0;
    for (const char *p = title; *p && o + 1 < sizeof(name); p++) {
        char c = *p;
        name[o++] = ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) ? c : '_';
    }
    name[o] = 0;
    char dir[80];
    fh1_data_dir("nes", dir, sizeof(dir));          // player 1's user's saves
    snprintf(n->save_base, sizeof(n->save_base), "%s/%s", dir, o ? name : "game");
}

static bool has_battery(fh1_nes_t *n) { return n->nes->cart && n->nes->cart->battery && n->nes->cart->prg_ram_banks > 0; }

esp_err_t fh1_nes_save_state(fh1_nes_t *n)
{
    char p[128];
    snprintf(p, sizeof(p), "%s.sta", n->save_base);
    fh1_mkdirs_parent(p);
    int r = state_save(p);
    fh1_int_info_changed();
    return r == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t fh1_nes_load_state(fh1_nes_t *n)
{
    char p[128];
    struct stat st;
    snprintf(p, sizeof(p), "%s.sta", n->save_base);
    if (stat(p, &st) != 0) return ESP_ERR_NOT_FOUND;
    if (state_load(p) != 0) {
        nes_reset(true);             // a half-read state leaves the machine in pieces
        return ESP_FAIL;
    }
    return ESP_OK;
}

void fh1_nes_reset(fh1_nes_t *n) { nes_reset(false); }

// ── Options ──

fh1_nes_opts_t *fh1_nes_options(fh1_nes_t *n) { return &n->opts; }
const char *fh1_nes_palette_name(int i) { return s_palette_names[i % FH1_NES_PALETTES]; }

static void apply_options(fh1_nes_t *n)
{
    build_palette(n);
    ppu_setopt(PPU_LIMIT_SPRITES, n->opts.sprite_limit);
}

void fh1_nes_save_options(fh1_nes_t *n)
{
    const fh1_nes_opts_t *o = &n->opts;
    fh1_settings_set_u32("nes", "opts", o->palette | o->drawing << 4 | o->turbo << 8 | o->show_fps << 9 | (!o->sprite_limit) << 10);
    apply_options(n);
}

static void load_options(fh1_nes_t *n)
{
    uint32_t v = fh1_settings_get_u32("nes", "opts", 4);      // PVM palette by default
    n->opts.palette = (v & 15) % FH1_NES_PALETTES;
    n->opts.drawing = (v >> 4 & 15) % FH1_NES_DRAW_COUNT;
    n->opts.turbo = v >> 8 & 1;
    n->opts.show_fps = (v >> 9 & 1) || fh1_settings_get_u8("ui", "fps", 0);
    n->opts.sprite_limit = !(v >> 10 & 1);
}

// ── Session ──

// The ROM file, read into PSRAM through an internal buffer (the SD card reads by DMA).
static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = (len > 16 && len <= 0x200000) ? heap_caps_malloc(len, MALLOC_CAP_SPIRAM) : NULL;
    uint8_t *buf = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    size_t done = 0, r;
    while (data && buf && (r = fread(buf, 1, 4096, f)) > 0) {
        memcpy(data + done, buf, r);
        done += r;
    }
    heap_caps_free(buf);
    fclose(f);
    if (!data || done != (size_t)len) {
        heap_caps_free(data);
        return NULL;
    }
    *size = len;
    return data;
}

static void free_session(fh1_nes_t *n)
{
    if (n->go) vSemaphoreDelete(n->go);
    if (n->gone) vSemaphoreDelete(n->gone);
    for (int i = 0; i < 2; i++) heap_caps_free(n->frame[i]);
    if (n->pal) fh1_mem_free(n->pal);
    fh1_mem_free(n);
}

fh1_nes_t *fh1_nes_open(const char *rom_path, fh1_nes_result_t *info, char *err, size_t errlen)
{
    memset(info, 0, sizeof(*info));
    if (s_open) {
        snprintf(err, errlen, "a NES game is already open");
        return NULL;
    }
    fh1_nes_t *n = fh1_mem_fast_calloc(1, sizeof(*n));
    if (!n) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    // The drawing task first: its stack counts against internal RAM before the machine takes its share.
    n->go = xSemaphoreCreateBinary();
    n->gone = xSemaphoreCreateBinary();
    n->fb = fh1_board_framebuffer(&n->fb_w, &n->fb_h);
    for (int i = 0; i < 2; i++) n->frame[i] = heap_caps_calloc(1, FRAME_BYTES, MALLOC_CAP_SPIRAM);
    if (!n->go || !n->gone || !n->frame[0] || !n->frame[1] ||
        xTaskCreatePinnedToCore(blit_task, "nes_blit", 3072, n, 3, NULL, 0) != pdPASS) {
        snprintf(err, errlen, "out of memory");
        free_session(n);
        return NULL;
    }
    // The machine: its CPU, PPU and APU tables, then the cartridge's RAM, take internal RAM in that order
    // (nes_alloc.h); the ROM image stays in PSRAM.
    // The file first: reading the card takes buffers for a moment, outside the machine's share.
    size_t size = 0;
    uint8_t *data = read_file(rom_path, &size);
    n->nes = data ? nes_init(SYS_DETECT, 32000, false, NULL) : NULL;
    rom_t *cart = n->nes ? rom_loadmem(data, size) : NULL;
    int r = -1;
    if (cart) {
        cart->free_data_ptr = true;           // nofrendo frees the image with the cartridge
        if (cart->system == SYS_UNKNOWN && (strstr(rom_path, "(E)") || strstr(rom_path, "(Europe)") ||
                                            strstr(rom_path, "(A)") || strstr(rom_path, "(Australia)")))
            cart->system = SYS_NES_PAL;
        r = nes_insertcart(cart);             // shuts the machine down itself on failure
    } else {
        heap_caps_free(data);
        if (n->nes) nes_shutdown();
    }
    if (r < 0) {
        snprintf(err, errlen, !data ? "could not read the ROM" : !n->nes ? "out of memory" :
                              r == -2 ? "mapper not supported" : "not a NES ROM");
        n->stop = true;
        xSemaphoreTake(n->gone, pdMS_TO_TICKS(1000));
        free_session(n);
        return NULL;
    }
    n->nes->blit_func = NULL;                 // frames are handed to the blitter in fh1_nes_play
    n->audio = fh1_audio_open(32000);
    fh1_nes_title_from_name(rom_path, info->title, sizeof(info->title));
    info->pal = n->nes->system == SYS_NES_PAL;
    info->mapper = n->nes->cart->mapper_number;
    make_save_base(n, info->title);
    load_options(n);
    apply_options(n);
    if (has_battery(n)) {
        char sav[128];
        snprintf(sav, sizeof(sav), "%s.sav", n->save_base);
        struct stat st;
        if (stat(sav, &st) == 0) rom_loadsram(sav);
    }
    // nofrendo needs two frames before a saved state restores cleanly (as in retro-go)
    nes_emulate(false);
    nes_emulate(false);
    s_open = n;
    ESP_LOGI(TAG, "%s: mapper %d, %s, PRG %d KB, CHR %d KB%s; internal RAM for: session %s, CPU RAM page %s, PRG RAM %s, CHR RAM %s",
             info->title, info->mapper, info->pal ? "PAL" : "NTSC", n->nes->cart->prg_rom_banks * 8,
             n->nes->cart->chr_rom_banks * 8, has_battery(n) ? ", battery" : "", fh1_mem_is_fast(n) ? "yes" : "no",
             fh1_mem_is_fast(n->nes->mem) ? "yes" : "no", fh1_mem_is_fast(n->nes->cart->prg_ram) ? "yes" : "no",
             n->nes->cart->chr_ram_banks ? (fh1_mem_is_fast(n->nes->cart->chr_ram) ? "yes" : "no") : "-");
    fh1_mem_log("game loaded");
    return n;
}

static void frame_shown(fh1_nes_t *n)
{
    n->ready = n->back;
    n->back ^= 1;
    n->blit_busy = true;
    xSemaphoreGive(n->go);
}

void fh1_nes_play(fh1_nes_t *n, fh1_nes_mode_t mode, fh1_nes_result_t *res)
{
    layout(n);
    fill_fb(n, BORDER);
    const int64_t frame_us = 1000000 / n->nes->refresh_rate;
    const float rate = n->nes->refresh_rate;
    int64_t start = esp_timer_get_time(), next = start, last_report = start;
    uint32_t frames = 0, report_frames = 0, reports = 0, calm = 0;
    uint32_t calm_need = 10, full_secs = 0;   // auto drawing: seconds of calm before trying every frame again
    int64_t run_us = 0;
    bool menu_was_down = true;
    int phase = 0;
    int64_t phase_start = start;
    n->draw = (mode == FH1_NES_PLAY);
    n->shown = 0;
    // Sound only while playing, and only with a ready audio device (it may have been switched on since).
    n->nes->apu_mix = mode == FH1_NES_PLAY && fh1_audio_available();
    input_connect(1, NES_JOYPAD);          // a second pad is always plugged in, as on the console
    for (;;) {
        bool menu = fh1_input_is_down(FH1_KEY_MENU);
        if (menu && !menu_was_down && mode == FH1_NES_PLAY) break;
        menu_was_down = menu;
        // The system wants the screen (an event's overlay: playing only) or is shutting down.
        if (fh1_app_quit_requested() || (mode == FH1_NES_PLAY && fh1_app_should_yield())) break;
        int pad = 0;
        if (fh1_input_is_down(FH1_KEY_A)) pad |= NES_PAD_A;
        if (fh1_input_is_down(FH1_KEY_B)) pad |= NES_PAD_B;
        if (fh1_input_is_down(FH1_KEY_START)) pad |= NES_PAD_START;
        if (fh1_input_is_down(FH1_KEY_SELECT)) pad |= NES_PAD_SELECT;
        if (fh1_input_is_down(FH1_KEY_UP)) pad |= NES_PAD_UP;
        if (fh1_input_is_down(FH1_KEY_DOWN)) pad |= NES_PAD_DOWN;
        if (fh1_input_is_down(FH1_KEY_LEFT)) pad |= NES_PAD_LEFT;
        if (fh1_input_is_down(FH1_KEY_RIGHT)) pad |= NES_PAD_RIGHT;
        input_update(0, pad);
        // Player 2: the second Bluetooth controller (A, B, Start, Select and the D-pad, as player 1's).
        const uint32_t k2 = fh1_input_player_keys(1);
        int pad2 = 0;
        if (k2 & 1u << FH1_KEY_A) pad2 |= NES_PAD_A;
        if (k2 & 1u << FH1_KEY_B) pad2 |= NES_PAD_B;
        if (k2 & 1u << FH1_KEY_START) pad2 |= NES_PAD_START;
        if (k2 & 1u << FH1_KEY_SELECT) pad2 |= NES_PAD_SELECT;
        if (k2 & 1u << FH1_KEY_UP) pad2 |= NES_PAD_UP;
        if (k2 & 1u << FH1_KEY_DOWN) pad2 |= NES_PAD_DOWN;
        if (k2 & 1u << FH1_KEY_LEFT) pad2 |= NES_PAD_LEFT;
        if (k2 & 1u << FH1_KEY_RIGHT) pad2 |= NES_PAD_RIGHT;
        input_update(1, pad2);
        // Draw this frame? Not when the blitter is still busy with the last one, nor every other frame at
        // half rate: the machine then skips the picture work altogether.
        n->frame_no++;
        bool half = n->opts.drawing == FH1_NES_DRAW_HALF || (n->opts.drawing == FH1_NES_DRAW_AUTO && n->half_rate);
        bool draw = n->draw && !n->blit_busy && !(half && (n->frame_no & 1));
        if (draw) nes_setvidbuf(n->frame[n->back]);
        int64_t t_run = esp_timer_get_time();
        nes_emulate(draw);
        run_us += esp_timer_get_time() - t_run;
        if (draw) frame_shown(n);
        if (n->nes->apu_mix) {
            // nofrendo's mix peaks around a third of full scale (measured, Metroid): 2.5x brings it near full
            // scale; the rare louder sample is clipped.
            int16_t *b = n->nes->apu->buffer;
            for (int i = 0; i < n->nes->apu->samples_per_frame; i++) {
                int v = b[i] * 5 / 2;
                b[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            }
            fh1_audio_write(n->audio, b, n->nes->apu->samples_per_frame);
        }
        frames++;
        report_frames++;
        int64_t now = esp_timer_get_time();
        if (mode == FH1_NES_PLAY) {
            next += frame_us;
            if (next > now && !n->opts.turbo) {
                vTaskDelay(pdMS_TO_TICKS((next - now) / 1000));
            } else {
                if (now - next > 100000) next = now;
                if ((frames & 3) == 0) vTaskDelay(1);      // let core 1's idle task run (see fh1_gb.c)
            }
            if (now - last_report >= 1000000) {
                float emulated = report_frames * 1e6f / (now - last_report);
                float shown = n->shown * 1e6f / (now - last_report);
                // A try at every frame that fails within a minute doubles the wait before the next try (10 s up
                // to 10 min): drawing every frame can slow the machine itself (PSRAM), and retrying every 10 s
                // made the game slow down for a few seconds each time.
                if (!n->half_rate && emulated < rate - 2) {
                    n->half_rate = true;
                    if (full_secs < 60) calm_need = calm_need * 2 > 600 ? 600 : calm_need * 2;
                } else if (n->half_rate && emulated > rate - 0.5f && ++calm >= calm_need) {
                    n->half_rate = false;
                    full_secs = 0;
                }
                if (!n->half_rate) {
                    calm = 0;
                    if (++full_secs >= 60) calm_need = 10;
                }
                if (n->opts.show_fps) show_fps(n, emulated, shown);
                if (++reports % 5 == 0) {
                    ESP_LOGI(TAG, "%.1f frames/s emulated (%.1f ms each), %.1f shown", emulated,
                             report_frames ? run_us / 1000.0f / report_frames : 0.0f, shown);
                }
                run_us = 0;
                last_report = now;
                report_frames = 0;
                n->shown = 0;
            }
        } else {
            if (now - phase_start >= 10000000) {
                float fps = frames * 1e6f / (now - phase_start);
                if (phase == 0) {
                    res->fps_emulation = fps;
                    ESP_LOGI(TAG, "benchmark: emulation only %.1f frames/s", fps);
                    phase = 1;
                    n->draw = true;
                } else {
                    res->fps_with_display = fps;
                    res->fps_shown = n->shown * 1e6f / (now - phase_start);
                    ESP_LOGI(TAG, "benchmark: emulation %.1f frames/s while drawing, %.1f shown", fps, res->fps_shown);
                    break;
                }
                frames = 0;
                n->shown = 0;
                phase_start = now;
            }
            if ((frames & 63) == 0) vTaskDelay(1);
        }
    }
    for (int i = 0; i < 50 && n->blit_busy; i++) vTaskDelay(1);
}

void fh1_nes_close(fh1_nes_t *n)
{
    if (!n) return;
    if (has_battery(n)) {
        char sav[128];
        snprintf(sav, sizeof(sav), "%s.sav", n->save_base);
        fh1_mkdirs_parent(sav);
        rom_savesram(sav);
        fh1_int_info_changed();
    }
    n->stop = true;
    xSemaphoreTake(n->gone, pdMS_TO_TICKS(1000));
    fill_fb(n, 0x10A2);
    nes_shutdown();
    fh1_audio_close(n->audio);
    s_open = NULL;
    free_session(n);
}
