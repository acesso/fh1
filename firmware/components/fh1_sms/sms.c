// The Master System / Game Gear emulator: SMS Plus on core 1, a blitter on core 0 that turns finished 8-bit
// frames into the screen's colours. Same shape as fh1_nes.
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <sys/stat.h>
#include "fh1_sms.h"
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
#include "shared.h"

static const char *TAG = "sms";
#define BORDER 0x0000
#define FRAME_BYTES (SMS_WIDTH * SMS_HEIGHT)
#define RATE 32000

struct fh1_sms {
    fh1_audio_t *audio;
    char save_base[112];         // <data dir>/sms/<title> (fh1_data_dir)
    fh1_sms_opts_t opts;
    uint8_t *rom;
    uint16_t cur_pal[256];           // the VDP's palette as of the last drawn frame
    uint16_t pal[2][256];            // ... and a copy per frame buffer, for the blitter
    int16_t *mix;                    // L, R samples for the audio class
    uint16_t *fb;
    int fb_w, fb_h, x0, y0, out_w, out_h, src_x, src_y;
    uint8_t *frame[2];
    int back;
    volatile int ready;
    volatile bool blit_busy, stop;
    SemaphoreHandle_t go, gone;
    volatile uint32_t shown;
    bool half_rate, draw, sound;
    uint32_t frame_no;
};

static fh1_sms_t *s_open;

// ── Drawing ──

static void blit_task(void *arg)
{
    fh1_sms_t *s = arg;
    uint16_t *row = fh1_mem_fast(SMS_WIDTH * 2);
    while (!s->stop && row) {
        if (xSemaphoreTake(s->go, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        const uint8_t *src = s->frame[s->ready];
        const uint16_t *pal = s->pal[s->ready];
        for (int y = 0; y < s->out_h; y++) {
            const uint8_t *p = src + (s->src_y + y) * SMS_WIDTH + s->src_x;
            for (int x = 0; x < s->out_w; x++) row[x] = pal[p[x]];
            memcpy(s->fb + (s->y0 + y) * s->fb_w + s->x0, row, s->out_w * 2);
        }
        s->shown++;
        s->blit_busy = false;
    }
    fh1_mem_free(row);
    xSemaphoreGive(s->gone);
    vTaskDelete(NULL);
}

// 1:1, the visible part of the console's picture (256 x 192 on a Master System, 160 x 144 on a Game Gear),
// cut to the screen's width when it is narrower.
static void layout(fh1_sms_t *s)
{
    int vw = bitmap.viewport.w, vh = bitmap.viewport.h;
    s->out_w = vw <= s->fb_w ? vw : s->fb_w;
    s->out_h = vh <= s->fb_h ? vh : s->fb_h;
    s->src_x = bitmap.viewport.x + (vw - s->out_w) / 2;
    s->src_y = bitmap.viewport.y + (vh - s->out_h) / 2;
    s->x0 = (s->fb_w - s->out_w) / 2;
    s->y0 = (s->fb_h - s->out_h) / 2;
}

static void fill_fb(fh1_sms_t *s, uint16_t c)
{
    for (int i = 0; i < s->fb_w * s->fb_h; i++) s->fb[i] = c;
}

static const uint16_t s_digits[12] = {
    075557, 026227, 071747, 071717, 055711, 074717, 074757, 071111, 075757, 075717, 000000, 001240,
};

static void show_fps(fh1_sms_t *s, float emulated, float shown)
{
    char t[16];
    snprintf(t, sizeof(t), "%2d/%2d", (int)(emulated + 0.5f), (int)(shown + 0.5f));
    int x = s->x0 + 2, y = s->y0 >= 7 ? (s->y0 - 5) / 2 : 1;
    for (const char *c = t; *c; c++, x += 4) {
        int g = (*c >= '0' && *c <= '9') ? *c - '0' : (*c == '/' ? 11 : 10);
        for (int r = 0; r < 5; r++) {
            for (int k = 0; k < 4; k++) {
                bool on = k < 3 && ((s_digits[g] >> (3 * (4 - r) + (2 - k))) & 1);
                if (y + r < s->fb_h && x + k < s->fb_w) s->fb[(y + r) * s->fb_w + x + k] = on ? 0xFFE0 : BORDER;
            }
        }
    }
}

// ── Saves ──

static void make_save_base(fh1_sms_t *s, const char *title)
{
    char name[40];
    size_t o = 0;
    for (const char *p = title; *p && o + 1 < sizeof(name); p++) {
        char c = *p;
        name[o++] = ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) ? c : '_';
    }
    name[o] = 0;
    char dir[80];
    fh1_data_dir("sms", dir, sizeof(dir));          // player 1's user's saves
    snprintf(s->save_base, sizeof(s->save_base), "%s/%s", dir, o ? name : "game");
}

esp_err_t fh1_sms_save_state(fh1_sms_t *s)
{
    char p[128];
    snprintf(p, sizeof(p), "%s.sta", s->save_base);
    fh1_mkdirs_parent(p);
    FILE *f = fopen(p, "wb");
    if (!f) return ESP_FAIL;
    system_save_state(f);
    bool ok = !ferror(f);
    fclose(f);
    fh1_int_info_changed();
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t fh1_sms_load_state(fh1_sms_t *s)
{
    char p[128];
    snprintf(p, sizeof(p), "%s.sta", s->save_base);
    FILE *f = fopen(p, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    system_load_state(f);
    fclose(f);
    return ESP_OK;
}

void fh1_sms_reset(fh1_sms_t *s) { system_reset(); }

static bool sram_used(void)
{
    for (int i = 0; cart.sram && i < 0x8000; i++) {
        if (cart.sram[i]) return true;
    }
    return false;
}

// ── Options ──

fh1_sms_opts_t *fh1_sms_options(fh1_sms_t *s) { return &s->opts; }

void fh1_sms_save_options(fh1_sms_t *s)
{
    const fh1_sms_opts_t *o = &s->opts;
    fh1_settings_set_u32("sms", "opts", o->drawing | o->turbo << 4 | o->show_fps << 5 | (!o->sprite_limit) << 6);
    option.spritelimit = o->sprite_limit;
}

static void load_options(fh1_sms_t *s)
{
    uint32_t v = fh1_settings_get_u32("sms", "opts", 0);
    s->opts.drawing = (v & 15) % FH1_SMS_DRAW_COUNT;
    s->opts.turbo = v >> 4 & 1;
    s->opts.show_fps = (v >> 5 & 1) || fh1_settings_get_u8("ui", "fps", 0);
    s->opts.sprite_limit = !(v >> 6 & 1);
}

// ── Session ──

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    long skip = (len > 0x4000 && ((len / 512) & 1)) ? 512 : 0;      // a copier header
    fseek(f, skip, SEEK_SET);
    len -= skip;
    size_t alloc = len < 0x4000 ? 0x4000 : (len + 0x3fff) & ~0x3fff;  // whole 16 KB pages
    uint8_t *data = (len > 0 && len <= 0x400000) ? heap_caps_calloc(1, alloc, MALLOC_CAP_SPIRAM) : NULL;
    uint8_t *buf = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    size_t done = 0, r;
    while (data && buf && (r = fread(buf, 1, 4096, f)) > 0 && done + r <= alloc) {
        memcpy(data + done, buf, r);
        done += r;
    }
    heap_caps_free(buf);
    fclose(f);
    if (!data || done != (size_t)len) {
        heap_caps_free(data);
        return NULL;
    }
    *size = alloc;
    return data;
}

static void free_session(fh1_sms_t *s)
{
    if (s->go) vSemaphoreDelete(s->go);
    if (s->gone) vSemaphoreDelete(s->gone);
    for (int i = 0; i < 2; i++) heap_caps_free(s->frame[i]);
    heap_caps_free(s->rom);
    fh1_mem_free(s->mix);
    if (s->audio) fh1_audio_close(s->audio);
    fh1_mem_free(s);
}

static void stop_blitter(fh1_sms_t *s)
{
    s->stop = true;
    xSemaphoreTake(s->gone, pdMS_TO_TICKS(1000));
}

fh1_sms_t *fh1_sms_open(const char *rom_path, fh1_sms_result_t *info, char *err, size_t errlen)
{
    memset(info, 0, sizeof(*info));
    if (s_open) {
        snprintf(err, errlen, "a game is already open");
        return NULL;
    }
    fh1_sms_t *s = fh1_mem_fast_calloc(1, sizeof(*s));
    if (!s) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    size_t size = 0;
    s->rom = read_file(rom_path, &size);           // first: the card's buffers stay out of the reserve
    if (!s->rom) {
        snprintf(err, errlen, "could not read the ROM");
        free_session(s);
        return NULL;
    }
    s->go = xSemaphoreCreateBinary();
    s->gone = xSemaphoreCreateBinary();
    s->fb = fh1_board_framebuffer(&s->fb_w, &s->fb_h);
    for (int i = 0; i < 2; i++) s->frame[i] = heap_caps_calloc(1, FRAME_BYTES, MALLOC_CAP_SPIRAM);
    if (!s->go || !s->gone || !s->frame[0] || !s->frame[1] ||
        xTaskCreatePinnedToCore(blit_task, "sms_blit", 3072, s, 3, NULL, 0) != pdPASS) {
        snprintf(err, errlen, "out of memory");
        free_session(s);          // the blitter never started
        return NULL;
    }
    s->audio = fh1_audio_open(RATE);
    s->sound = fh1_audio_available();
    system_reset_config();
    // Without a sound device the sound chips are not emulated at all (a rate of 0 turns them off).
    option.sndrate = s->sound ? RATE : 0;
    option.overscan = 0;
    option.extra_gg = 0;
    size_t l = strlen(rom_path);
    option.console = (l > 3 && strcasecmp(rom_path + l - 3, ".sg") == 0) ? 5 : 0;
    load_options(s);
    option.spritelimit = s->opts.sprite_limit;
    if (!load_rom(s->rom, size, size)) {
        snprintf(err, errlen, "not a Master System ROM");
        stop_blitter(s);
        free_session(s);
        return NULL;
    }
    bitmap.width = SMS_WIDTH;
    bitmap.height = SMS_HEIGHT;
    bitmap.pitch = SMS_WIDTH;
    bitmap.data = s->frame[0];
    // The machine: work RAM, video RAM and sound buffers take internal RAM in that order (sms_alloc.h); the
    // big lookup tables go to PSRAM.
    system_poweron();
    if (s->sound) s->mix = fh1_mem_fast(snd.sample_count * 4 + 8);
    fh1_title_from_filename(rom_path, info->title, sizeof(info->title));
    info->pal = sms.display != DISPLAY_NTSC;
    info->game_gear = IS_GG;
    make_save_base(s, info->title);
    char sav[128];
    snprintf(sav, sizeof(sav), "%s.sav", s->save_base);
    FILE *f = fopen(sav, "rb");
    if (f) {
        fread(cart.sram, 1, 0x8000, f);
        fclose(f);
    }
    render_copy_palette(s->cur_pal);
    s_open = s;
    ESP_LOGI(TAG, "%s: %s, %s, %u KB ROM; internal RAM for: session %s, work RAM %s, video RAM %s; sound %s",
             info->title, IS_GG ? "Game Gear" : "Master System", info->pal ? "PAL" : "NTSC", (unsigned)(size / 1024),
             fh1_mem_is_fast(s) ? "yes" : "no", fh1_mem_is_fast(sms.wram) ? "yes" : "no",
             fh1_mem_is_fast(vdp.vram) ? "yes" : "no", s->sound ? "on" : "off");
    fh1_mem_log("game loaded");
    return s;
}

void fh1_sms_play(fh1_sms_t *s, fh1_sms_mode_t mode, fh1_sms_result_t *res)
{
    layout(s);
    fill_fb(s, BORDER);
    const int rate = sms.display == DISPLAY_NTSC ? 60 : 50;
    const int64_t frame_us = 1000000 / rate;
    int64_t start = esp_timer_get_time(), next = start, last_report = start;
    uint32_t frames = 0, report_frames = 0, reports = 0, calm = 0;
    uint32_t calm_need = 10, full_secs = 0;   // auto drawing: seconds of calm before trying every frame again
    int64_t run_us = 0;
    bool menu_was_down = true;
    int phase = 0;
    int64_t phase_start = start;
    s->draw = (mode == FH1_SMS_PLAY);
    s->shown = 0;
    for (;;) {
        bool menu = fh1_input_is_down(FH1_KEY_MENU);
        if (menu && !menu_was_down && mode == FH1_SMS_PLAY) break;
        menu_was_down = menu;
        // The system wants the screen (an event's overlay: playing only) or is shutting down.
        if (fh1_app_quit_requested() || (mode == FH1_SMS_PLAY && fh1_app_should_yield())) break;
        input.pad[0] = input.pad[1] = 0;
        input.system = 0;
        if (fh1_input_is_down(FH1_KEY_UP)) input.pad[0] |= INPUT_UP;
        if (fh1_input_is_down(FH1_KEY_DOWN)) input.pad[0] |= INPUT_DOWN;
        if (fh1_input_is_down(FH1_KEY_LEFT)) input.pad[0] |= INPUT_LEFT;
        if (fh1_input_is_down(FH1_KEY_RIGHT)) input.pad[0] |= INPUT_RIGHT;
        if (fh1_input_is_down(FH1_KEY_A)) input.pad[0] |= INPUT_BUTTON2;
        if (fh1_input_is_down(FH1_KEY_B)) input.pad[0] |= INPUT_BUTTON1;
        // Player 2: the second Bluetooth controller.
        const uint32_t k2 = fh1_input_player_keys(1);
        if (k2 & 1u << FH1_KEY_UP) input.pad[1] |= INPUT_UP;
        if (k2 & 1u << FH1_KEY_DOWN) input.pad[1] |= INPUT_DOWN;
        if (k2 & 1u << FH1_KEY_LEFT) input.pad[1] |= INPUT_LEFT;
        if (k2 & 1u << FH1_KEY_RIGHT) input.pad[1] |= INPUT_RIGHT;
        if (k2 & 1u << FH1_KEY_A) input.pad[1] |= INPUT_BUTTON2;
        if (k2 & 1u << FH1_KEY_B) input.pad[1] |= INPUT_BUTTON1;
        if (IS_GG) {
            if (fh1_input_is_down(FH1_KEY_START)) input.system |= INPUT_START;
            if (fh1_input_is_down(FH1_KEY_SELECT)) input.system |= INPUT_PAUSE;
        } else {
            if (fh1_input_is_down(FH1_KEY_START)) input.system |= INPUT_PAUSE;
            if (fh1_input_is_down(FH1_KEY_SELECT)) input.system |= INPUT_START;
        }
        s->frame_no++;
        bool half = s->opts.drawing == FH1_SMS_DRAW_HALF || (s->opts.drawing == FH1_SMS_DRAW_AUTO && s->half_rate);
        bool draw = s->draw && !s->blit_busy && !(half && (s->frame_no & 1));
        if (draw) bitmap.data = s->frame[s->back];
        int64_t t_run = esp_timer_get_time();
        system_frame(!draw);
        run_us += esp_timer_get_time() - t_run;
        if (draw) {
            render_copy_palette(s->cur_pal);            // only when the game changed it
            memcpy(s->pal[s->back], s->cur_pal, sizeof(s->cur_pal));
            s->ready = s->back;
            s->back ^= 1;
            s->blit_busy = true;
            xSemaphoreGive(s->go);
        }
        if (s->sound && s->mix && mode == FH1_SMS_PLAY) {
            int n = snd.sample_count;
            for (int i = 0; i < n; i++) {            // the console's stereo, a little louder (as retro-go does)
                int l = snd.stream[0][i] * 11 / 4, r = snd.stream[1][i] * 11 / 4;
                s->mix[2 * i] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
                s->mix[2 * i + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
            }
            fh1_audio_write_stereo(s->audio, s->mix, n);
        }
        frames++;
        report_frames++;
        int64_t now = esp_timer_get_time();
        if (mode == FH1_SMS_PLAY) {
            next += frame_us;
            if (next > now && !s->opts.turbo) {
                vTaskDelay(pdMS_TO_TICKS((next - now) / 1000));
            } else {
                if (now - next > 100000) next = now;
                if ((frames & 3) == 0) vTaskDelay(1);      // let core 1's idle task run (see fh1_gb.c)
            }
            if (now - last_report >= 1000000) {
                float emulated = report_frames * 1e6f / (now - last_report);
                float shown = s->shown * 1e6f / (now - last_report);
                // A try at every frame that fails within a minute doubles the wait before the next try (10 s up
                // to 10 min): drawing every frame can slow the machine itself (PSRAM), and retrying every 10 s
                // made the game slow down for a few seconds each time.
                if (!s->half_rate && emulated < rate - 2) {
                    s->half_rate = true;
                    if (full_secs < 60) calm_need = calm_need * 2 > 600 ? 600 : calm_need * 2;
                } else if (s->half_rate && emulated > rate - 0.5f && ++calm >= calm_need) {
                    s->half_rate = false;
                    full_secs = 0;
                }
                if (!s->half_rate) {
                    calm = 0;
                    if (++full_secs >= 60) calm_need = 10;
                }
                if (s->opts.show_fps) show_fps(s, emulated, shown);
                if (++reports % 5 == 0) {
                    ESP_LOGI(TAG, "%.1f frames/s emulated (%.1f ms each), %.1f shown", emulated,
                             report_frames ? run_us / 1000.0f / report_frames : 0.0f, shown);
                }
                run_us = 0;
                last_report = now;
                report_frames = 0;
                s->shown = 0;
            }
        } else {
            if (now - phase_start >= 10000000) {
                float fps = frames * 1e6f / (now - phase_start);
                if (phase == 0) {
                    res->fps_emulation = fps;
                    ESP_LOGI(TAG, "benchmark: emulation only %.1f frames/s", fps);
                    phase = 1;
                    s->draw = true;
                } else {
                    res->fps_with_display = fps;
                    res->fps_shown = s->shown * 1e6f / (now - phase_start);
                    ESP_LOGI(TAG, "benchmark: emulation %.1f frames/s while drawing, %.1f shown", fps, res->fps_shown);
                    break;
                }
                frames = 0;
                s->shown = 0;
                phase_start = now;
            }
            if ((frames & 63) == 0) vTaskDelay(1);
        }
    }
    for (int i = 0; i < 50 && s->blit_busy; i++) vTaskDelay(1);
}

void fh1_sms_close(fh1_sms_t *s)
{
    if (!s) return;
    if (sram_used()) {
        char sav[128];
        snprintf(sav, sizeof(sav), "%s.sav", s->save_base);
        fh1_mkdirs_parent(sav);
        FILE *f = fopen(sav, "wb");
        if (f) {
            fwrite(cart.sram, 1, 0x8000, f);
            fclose(f);
            fh1_int_info_changed();
        }
    }
    stop_blitter(s);
    fill_fb(s, 0x10A2);
    system_shutdown();
    free(cart.sram);              // allocated by load_rom
    cart.sram = NULL;
    cart.rom = NULL;              // ours: freed with the session
    s_open = NULL;
    free_session(s);
}
