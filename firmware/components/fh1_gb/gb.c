// The Game Boy / Game Boy Color emulator (M8): Peanut-GB (cgb branch) on core 1, a blitter on core 0 that
// scales finished frames into the screen, options changed between plays from the options menu.
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "fh1_gb.h"
#include "fh1_board.h"
#include "fh1_input.h"
#include "fh1_settings.h"
#include "fh1_storage.h"
#include "esp_heap_caps.h"
#include "fh1_mem.h"
#include "fh1_app.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#define PEANUT_FULL_GBC_SUPPORT 1
#define PEANUT_GB_HIGH_LCD_ACCURACY 0     // per-line sprite sorting off: faster, rare sprite-priority glitches
#define PEANUT_GB_RAM_POINTERS 1         // work and video RAM allocated here, in internal RAM when it has room
#define ENABLE_SOUND 1                  // the sound registers go to minigb_apu
#include "minigb_apu.h"
#include "peanut_gb.h"
#include "fh1_audio.h"

static const char *TAG = "gb";

#include "gbc_palettes.h"

// Original Game Boy shades, lightest to darkest (RGB565): green, grey, sepia, ocean.
#define CLASSIC_FIRST 13
static const uint16_t s_palettes[4][4] = {
    { 0xE7DA, 0x8E0E, 0x334A, 0x08C4 },
    { 0xFFFF, 0xAD55, 0x52AA, 0x0000 },
    { 0xF79A, 0xC5CF, 0x7B05, 0x3961 },
    { 0xDF9F, 0x6D9A, 0x2B10, 0x0846 },
};
static const char *const s_palette_names[FH1_GB_PALETTES] = {
    "GBC (auto)",
    // The GBC's power-on choices, in the order of gbc_key_combos: Right, Left, Up, Down, with A, with B.
    "GBC green", "GBC blue", "GBC brown", "GBC pastel", "GBC dark green", "GBC dark blue", "GBC red", "GBC orange",
    "GBC inverted", "GBC grey", "GBC dark brown", "GBC yellow",
    "green", "grey", "sepia", "ocean",
};
#define BORDER 0x0000

struct fh1_gb {
    struct gb_s gb;
    uint8_t *rom;               // whole ROM, PSRAM
    size_t rom_size;
    uint8_t *bank0[4];          // the ROM's first 16 KB in 4 KB pieces (internal RAM is fragmented); PSRAM
                                // pieces point into rom
    uint8_t *cart_ram;
    size_t cart_ram_size;
    char save_base[112];        // <data dir>/gb/<title> (fh1_data_dir)
    fh1_gb_opts_t opts;
    // Display mapping
    uint16_t *fb;
    int fb_w, fb_h, x0, y0, out_w, out_h;
    uint16_t xmap[LCD_WIDTH * 2];     // screen column -> GB column
    uint16_t row0[LCD_HEIGHT + 1];    // GB line -> first screen row
    // Frames: the emulator (core 1) fills one 160 x 144 buffer while the blitter (core 0) scales the other.
    // A frame that arrives while the blitter is busy is not shown; the game keeps time.
    uint16_t *frame[2];
    int back;
    volatile int ready;
    volatile bool blit_busy, stop;
    SemaphoreHandle_t go, gone;
    volatile uint32_t shown;
    bool half_rate;
    uint32_t frame_no;
    bool draw;
    bool error;
    uint8_t auto_combo;                // the GBC's palette combination for this game (from its title)
    uint16_t dmg_lut[0x34];            // original Game Boy pixel (layer << 4 | shade) -> RGB565
    fh1_audio_t *audio;                // the audio.out class (silent while no device is ready)
    int16_t *snd;                      // one frame of sound: AUDIO_SAMPLES L, R pairs (the sound task's)
    QueueHandle_t snd_q;               // frames' register-write logs, to the sound task (see "Sound")
    SemaphoreHandle_t snd_gone;
};


#define HELD_PIECES 3          // 2 task stacks, and their control blocks, queues and line buffer

// A piece of ROM bank 0 in internal RAM, or NULL: then it is read from the ROM copy in PSRAM.
static uint8_t *bank0_piece(void)
{
    uint8_t *p = fh1_mem_fast(0x1000);
    if (p && !fh1_mem_is_fast(p)) {
        fh1_mem_free(p);
        p = NULL;
    }
    return p;
}

static void free_bank0(fh1_gb_t *e)
{
    for (int i = 0; i < 4; i++) {
        if (e->bank0[i] && fh1_mem_is_fast(e->bank0[i])) fh1_mem_free(e->bank0[i]);
        e->bank0[i] = NULL;
    }
}

static uint8_t rom_read(struct gb_s *gb, const uint_fast32_t addr)
{
    fh1_gb_t *e = gb->direct.priv;
    if (addr < 0x4000) return e->bank0[addr >> 12][addr & 0xfff];
    return addr < e->rom_size ? e->rom[addr] : 0xff;
}

static uint8_t ram_read(struct gb_s *gb, const uint_fast32_t addr)
{
    fh1_gb_t *e = gb->direct.priv;
    return addr < e->cart_ram_size ? e->cart_ram[addr] : 0xff;
}

static void ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val)
{
    fh1_gb_t *e = gb->direct.priv;
    if (addr < e->cart_ram_size) e->cart_ram[addr] = val;
}

static void on_error(struct gb_s *gb, const enum gb_error_e err, const uint16_t addr)
{
    fh1_gb_t *e = gb->direct.priv;
    if (!e->error) ESP_LOGW(TAG, "emulator error %d at 0x%04x", err, addr);
    e->error = true;
}

static void draw_line(struct gb_s *gb, const uint8_t *px, const uint_fast8_t ly)
{
    fh1_gb_t *e = gb->direct.priv;
    if (!e->draw) return;
    uint16_t *out = e->frame[e->back] + ly * LCD_WIDTH;
    if (gb->cgb.cgbMode) {
        for (int x = 0; x < LCD_WIDTH; x++) {
            uint16_t c = gb->cgb.fixPalette[px[x]];      // 0rrrrrgggggbbbbb
            uint16_t g5 = (c >> 5) & 31;
            out[x] = (uint16_t)(((c >> 10) & 31) << 11 | (g5 << 1 | g5 >> 4) << 5 | (c & 31));
        }
    } else {
        // The layer (sprite group 0 or 1, background) and the shade pick the colour.
        const uint16_t *lut = e->dmg_lut;
        for (int x = 0; x < LCD_WIDTH; x++) out[x] = lut[px[x] & 0x33];
    }
}

static void blit_task(void *arg)
{
    fh1_gb_t *e = arg;
    uint16_t *row = fh1_mem_fast(LCD_WIDTH * 2 * 2);
    while (!e->stop && row) {
        if (xSemaphoreTake(e->go, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        const uint16_t *src = e->frame[e->ready];
        for (int ly = 0; ly < LCD_HEIGHT; ly++) {
            const int r0 = e->row0[ly], r1 = e->row0[ly + 1];
            const uint16_t *line = src + ly * LCD_WIDTH;
            for (int dx = 0; dx < e->out_w; dx++) row[dx] = line[e->xmap[dx]];
            for (int r = r0; r < r1; r++) memcpy(e->fb + (e->y0 + r) * e->fb_w + e->x0, row, e->out_w * 2);
        }
        e->shown++;
        e->blit_busy = false;
    }
    heap_caps_free(row);
    xSemaphoreGive(e->gone);
    vTaskDelete(NULL);
}

// rendered: the emulator drew this frame's lines (at half rate it skips every other frame altogether).
static void frame_done(fh1_gb_t *e, bool rendered)
{
    e->frame_no++;
    if (!e->draw || e->blit_busy || !rendered) return;
    e->ready = e->back;
    e->back ^= 1;
    e->blit_busy = true;
    xSemaphoreGive(e->go);
}

// The picture: "fit" is the largest of 2x, 1.5x, 1x that fits the screen; "1x" is the original size.
static void layout(fh1_gb_t *e)
{
    static const int scales[][2] = { { 2, 1 }, { 3, 2 }, { 1, 1 } };
    int num = 1, den = 1;
    if (e->opts.scale == FH1_GB_SCALE_FIT) {
        for (size_t i = 0; i < sizeof(scales) / sizeof(scales[0]); i++) {
            if (LCD_WIDTH * scales[i][0] / scales[i][1] <= e->fb_w && LCD_HEIGHT * scales[i][0] / scales[i][1] <= e->fb_h) {
                num = scales[i][0];
                den = scales[i][1];
                break;
            }
        }
    }
    e->out_w = LCD_WIDTH * num / den;
    e->out_h = LCD_HEIGHT * num / den;
    e->x0 = (e->fb_w - e->out_w) / 2;
    e->y0 = (e->fb_h - e->out_h) / 2;
    for (int x = 0; x < LCD_WIDTH; x++) {
        for (int dx = x * num / den; dx < (x + 1) * num / den; dx++) e->xmap[dx] = x;
    }
    for (int y = 0; y <= LCD_HEIGHT; y++) e->row0[y] = y * num / den;
}

static void fill_fb(fh1_gb_t *e, uint16_t c)
{
    for (int i = 0; i < e->fb_w * e->fb_h; i++) e->fb[i] = c;
}

// ── FPS overlay: a 3 x 5 pixel font for digits, drawn in the band above the picture ──

static const uint16_t s_digits[12] = {       // 15 bits each, rows top to bottom, 3 bits per row
    075557, 026227, 071747, 071717, 055711, 074717, 074757, 071111, 075757, 075717,
    000000,  // space
    001240,  // '/'
};

static void draw_text(fh1_gb_t *e, int x, int y, const char *s)
{
    for (; *s; s++, x += 4) {
        int g = (*s >= '0' && *s <= '9') ? *s - '0' : (*s == '/' ? 11 : 10);
        for (int r = 0; r < 5; r++) {
            for (int c = 0; c < 3; c++) {
                bool on = (s_digits[g] >> (3 * (4 - r) + (2 - c))) & 1;
                if (y + r < e->fb_h && x + c < e->fb_w) e->fb[(y + r) * e->fb_w + x + c] = on ? 0xFFE0 : BORDER;
            }
        }
        for (int r = 0; r < 5; r++) {
            if (x + 3 < e->fb_w) e->fb[(y + r) * e->fb_w + x + 3] = BORDER;
        }
    }
}

static void show_fps(fh1_gb_t *e, float emulated, float shown)
{
    char t[16];
    snprintf(t, sizeof(t), "%2d/%2d", (int)(emulated + 0.5f), (int)(shown + 0.5f));
    // Top-left, in the border above or beside the picture; at 1x there is plenty of room.
    draw_text(e, 2, e->y0 >= 7 ? (e->y0 - 5) / 2 : 1, t);
}

// ── Saves ──

static void make_save_base(fh1_gb_t *e)
{
    char name[17];
    gb_get_rom_name(&e->gb, name);
    for (char *p = name; *p; p++) {
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'))) *p = '_';
    }
    char dir[80];
    fh1_data_dir("gb", dir, sizeof(dir));          // player 1's user's saves
    snprintf(e->save_base, sizeof(e->save_base), "%s/%s", dir, name[0] ? name : "game");
}

#define STATE_MAGIC 0x46483154u    // "FH1T": the machine, then its work and video RAM, then the cartridge RAM

esp_err_t fh1_gb_save_state(fh1_gb_t *e)
{
    char p[128];
    snprintf(p, sizeof(p), "%s.st2", e->save_base);
    fh1_mkdirs_parent(p);
    FILE *f = fopen(p, "wb");
    if (!f) return ESP_FAIL;
    uint32_t hdr[2] = { STATE_MAGIC, sizeof(struct gb_s) };
    bool ok = fwrite(hdr, sizeof(hdr), 1, f) == 1 && fwrite(&e->gb, sizeof(e->gb), 1, f) == 1 &&
              fwrite(e->gb.wram_half[0], WRAM_SIZE / 2, 1, f) == 1 && fwrite(e->gb.wram_half[1], WRAM_SIZE / 2, 1, f) == 1 &&
              fwrite(e->gb.vram, VRAM_SIZE, 1, f) == 1 &&
              (!e->cart_ram_size || fwrite(e->cart_ram, e->cart_ram_size, 1, f) == 1);
    fclose(f);
    fh1_int_info_changed();
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t fh1_gb_load_state(fh1_gb_t *e)
{
    char p[128];
    snprintf(p, sizeof(p), "%s.st2", e->save_base);
    FILE *f = fopen(p, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    uint32_t hdr[2];
    // Read into a copy, so a bad file leaves the running game alone.
    struct gb_s *tmp = heap_caps_malloc(sizeof(struct gb_s) + WRAM_SIZE + VRAM_SIZE, MALLOC_CAP_SPIRAM);
    uint8_t *ram = tmp ? (uint8_t *)(tmp + 1) : NULL;
    bool ok = tmp && fread(hdr, sizeof(hdr), 1, f) == 1 && hdr[0] == STATE_MAGIC && hdr[1] == sizeof(struct gb_s) &&
              fread(tmp, sizeof(*tmp), 1, f) == 1 &&
              fread(ram, WRAM_SIZE + VRAM_SIZE, 1, f) == 1 &&
              (!e->cart_ram_size || fread(e->cart_ram, e->cart_ram_size, 1, f) == 1);
    fclose(f);
    if (ok) {
        // The saved copy holds this firmware's function pointers from another run: keep the live ones.
        tmp->gb_rom_read = e->gb.gb_rom_read;
        tmp->gb_cart_ram_read = e->gb.gb_cart_ram_read;
        tmp->gb_cart_ram_write = e->gb.gb_cart_ram_write;
        tmp->gb_error = e->gb.gb_error;
        tmp->gb_serial_tx = e->gb.gb_serial_tx;
        tmp->gb_serial_rx = e->gb.gb_serial_rx;
        tmp->gb_bootrom_read = e->gb.gb_bootrom_read;
        tmp->display.lcd_draw_line = e->gb.display.lcd_draw_line;
        tmp->direct.priv = e;
        tmp->wram_half[0] = e->gb.wram_half[0];
        tmp->wram_half[1] = e->gb.wram_half[1];
        tmp->vram = e->gb.vram;
        memcpy(e->gb.wram_half[0], ram, WRAM_SIZE / 2);
        memcpy(e->gb.wram_half[1], ram + WRAM_SIZE / 2, WRAM_SIZE / 2);
        memcpy(e->gb.vram, ram + WRAM_SIZE, VRAM_SIZE);
        e->gb = *tmp;
    }
    heap_caps_free(tmp);
    return ok ? ESP_OK : ESP_FAIL;
}

void fh1_gb_reset(fh1_gb_t *e) { gb_reset(&e->gb); }

// ── Options ──

// ── Colours for original Game Boy games ──

// The Game Boy Color's choice for a game, as its boot ROM makes it: Nintendo titles are found by the sum
// of their 16 title bytes (and the 4th letter where two share a sum); everything else gets combination 0.
static uint8_t gbc_auto_combo(const uint8_t *rom)
{
    const uint8_t old_lic = rom[0x14B];
    bool nintendo = old_lic == 0x01 || (old_lic == 0x33 && rom[0x144] == '0' && rom[0x145] == '1');
    if (!nintendo) return 0;
    uint8_t sum = 0;
    for (int i = 0x134; i < 0x144; i++) sum += rom[i];
    for (int i = 0; i < (int)sizeof(gbc_title_checksums); i++) {
        if (gbc_title_checksums[i] != sum) continue;
        if (i >= GBC_DUP_START && gbc_dup_letters[i - GBC_DUP_START] != (char)rom[0x137]) continue;
        return gbc_combo_per_checksum[i] & 0x7f;
    }
    return 0;
}

static uint16_t rgb15_to_565(uint16_t c)      // red in the low bits
{
    uint16_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return (uint16_t)(r << 11 | (g << 1 | g >> 4) << 5 | b);
}

static void build_dmg_lut(fh1_gb_t *e)
{
    const int p = e->opts.palette % FH1_GB_PALETTES;
    for (int layer = 0; layer < 3; layer++) {          // 0 sprites 0, 1 sprites 1, 2 background
        for (int s = 0; s < 4; s++) {
            uint16_t c;
            if (p >= CLASSIC_FIRST) {
                c = s_palettes[p - CLASSIC_FIRST][s];
            } else {
                const uint8_t combo = p == 0 ? e->auto_combo : gbc_key_combos[p - 1];
                c = rgb15_to_565(gbc_colours[gbc_combos[combo][layer] + s]);
            }
            e->dmg_lut[layer << 4 | s] = c;
        }
    }
}

fh1_gb_opts_t *fh1_gb_options(fh1_gb_t *e) { return &e->opts; }
const char *fh1_gb_palette_name(int i) { return s_palette_names[i % FH1_GB_PALETTES]; }

void fh1_gb_save_options(fh1_gb_t *e)
{
    const fh1_gb_opts_t *o = &e->opts;
    // Bit 15 marks this layout: the palette moved to bits 16-20 when the GBC palettes came.
    fh1_settings_set_u32("gb", "opts", o->scale | o->drawing << 8 | o->turbo << 12 | o->show_fps << 13 | 1u << 15 |
                                       (uint32_t)o->palette << 16);
    build_dmg_lut(e);
}

static void load_options(fh1_gb_t *e)
{
    uint32_t v = fh1_settings_get_u32("gb", "opts", 0);
    e->opts.scale = (v & 15) % FH1_GB_SCALE_COUNT;
    // Saved before the GBC palettes (no bit 15): start on the GBC's own colours.
    e->opts.palette = (v & 1u << 15) ? (v >> 16 & 31) % FH1_GB_PALETTES : 0;
    e->opts.drawing = (v >> 8 & 15) % FH1_GB_DRAW_COUNT;
    e->opts.turbo = v >> 12 & 1;
    e->opts.show_fps = (v >> 13 & 1) || fh1_settings_get_u8("ui", "fps", 0);
}

// ── Session ──

// ── Sound ──
// The APU (minigb_apu) runs on core 0, beside the blitter, so the emulator on core 1 keeps its time for the
// game. During a frame the game's writes to the sound registers are logged; at the frame's end the log goes
// to the sound task, which replays it into the APU, renders the frame's samples and writes them out. The
// queue holds one log: when the sound task is behind, the emulator waits, so the audio clock still paces the
// game. Reads are answered here from a copy of the registers kept like the APU's own (games read back what
// they wrote); only NR52's channel bits come from the APU, a frame late.
uint8_t apu_core_read(uint16_t addr);
void apu_core_write(uint16_t addr, uint8_t val);
void apu_core_callback(void *user, uint8_t *data, int len);
void apu_core_init(void);
const uint8_t *apu_core_registers(void);

typedef struct {
    uint16_t addr;
    uint8_t val;
} snd_write_t;
#define SND_LOG 1024                   // writes in one frame (a busy frame has a few hundred)
static snd_write_t *s_log[3];          // filled, queued, being replayed
static uint16_t s_log_n[3];
static int s_log_cur;
static volatile bool s_log_on;         // the sound task runs: log writes (else they go to the APU directly)
static uint8_t s_regs[0x30];           // 0xFF10-0xFF3F as the APU holds them
static volatile uint32_t s_apu_us;     // the sound task's rendering time, for the 5 s report

uint8_t audio_read(const uint16_t addr)
{
    static const uint8_t ortab[] = { 0x80, 0x3f, 0x00, 0xff, 0xbf, 0xff, 0x3f, 0x00, 0xff, 0xbf, 0x7f, 0xff,
                                     0x9f, 0xff, 0xbf, 0xff, 0xff, 0x00, 0x00, 0xbf, 0x00, 0x00, 0x70,
                                     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    const unsigned i = addr - 0xFF10;
    uint8_t v = s_regs[i];
    if (addr == 0xFF26) v = (v & 0x80) | (apu_core_read(0xFF26) & 0x0F);    // which channels play
    return v | ortab[i];
}

void audio_write(const uint16_t addr, const uint8_t val)
{
    const unsigned i = addr - 0xFF10;
    if (addr == 0xFF26) {                               // the APU's own rules, for the copy
        s_regs[i] = val & 0x80;
        if (!(val & 0x80)) memset(s_regs, 0, 0xFF26 - 0xFF10);
    } else if (s_regs[0xFF26 - 0xFF10]) {
        s_regs[i] = val;
    }
    if (!s_log_on) {
        apu_core_write(addr, val);
    } else if (s_log_n[s_log_cur] < SND_LOG) {
        snd_write_t *w = &s_log[s_log_cur][s_log_n[s_log_cur]++];
        w->addr = addr;
        w->val = val;
    }
}

static void snd_task(void *arg)
{
    fh1_gb_t *e = arg;
    int idx;
    while (xQueueReceive(e->snd_q, &idx, portMAX_DELAY) == pdTRUE && idx >= 0) {
        int64_t t0 = esp_timer_get_time();
        for (unsigned i = 0; i < s_log_n[idx]; i++) apu_core_write(s_log[idx][i].addr, s_log[idx][i].val);
        apu_core_callback(NULL, (uint8_t *)e->snd, AUDIO_SAMPLES * 4);
        // minigb_apu's channels peak at an eighth of full scale each: 2x brings a busy mix near full scale;
        // the rare louder sample is clipped.
        for (unsigned i = 0; i < AUDIO_SAMPLES * 2; i++) {
            int v = e->snd[i] * 2;
            e->snd[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        s_apu_us += (uint32_t)(esp_timer_get_time() - t0);
        fh1_audio_write_stereo(e->audio, e->snd, AUDIO_SAMPLES);   // blocks while the output is full
    }
    xSemaphoreGive(e->snd_gone);
    vTaskDelete(NULL);
}

static bool snd_start(fh1_gb_t *e)
{
    if (!e->snd_q) e->snd_q = xQueueCreate(1, sizeof(int));
    if (!e->snd_gone) e->snd_gone = xSemaphoreCreateBinary();
    if (!e->snd_q || !e->snd_gone) return false;
    s_log_cur = 0;
    s_log_n[0] = 0;
    s_log_on = true;
    if (xTaskCreatePinnedToCore(snd_task, "gb_sound", 4096, e, 4, NULL, 0) != pdPASS) {
        s_log_on = false;
        return false;
    }
    return true;
}

// The frame's writes to the sound task; waits while it is a frame behind. Returns the time waited.
static int64_t snd_frame(fh1_gb_t *e)
{
    int idx = s_log_cur;
    int64_t t0 = esp_timer_get_time();
    xQueueSend(e->snd_q, &idx, portMAX_DELAY);
    s_log_cur = (idx + 1) % 3;
    s_log_n[s_log_cur] = 0;
    return esp_timer_get_time() - t0;
}

static void snd_stop(fh1_gb_t *e)
{
    if (!s_log_on) return;
    int quit = -1;
    xQueueSend(e->snd_q, &quit, pdMS_TO_TICKS(500));
    xSemaphoreTake(e->snd_gone, pdMS_TO_TICKS(500));
    s_log_on = false;
}

fh1_gb_t *fh1_gb_open(const char *rom_path, fh1_gb_result_t *info, char *err, size_t errlen)
{
    memset(info, 0, sizeof(*info));
    // Fastest memory first for what the emulated CPU touches most: its state, the first half of work RAM
    // (all of it on a Game Boy, banks 0-3 on a Color), video RAM, ROM bank 0 (much of a game's code), then
    // the second half of work RAM (Color banks 4-7, rarely used). The window system is frozen by now
    // (fh1_ui_pause), so internal RAM is as free as it gets.
    fh1_gb_t *e = fh1_mem_fast_calloc(1, sizeof(fh1_gb_t));
    if (!e) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    // The ROM first: reading the card takes buffers for a moment, and they should not come out of the
    // internal RAM the reserve keeps.
    FILE *f = fopen(rom_path, "rb");
    if (!f) {
        snprintf(err, errlen, "cannot open %s", rom_path);
        goto fail;
    }
    fseek(f, 0, SEEK_END);
    e->rom_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    e->rom = heap_caps_malloc(e->rom_size, MALLOC_CAP_SPIRAM);
    // Read through an internal buffer: the SD card reads by DMA (see fh1_ota.c).
    uint8_t *buf = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    size_t done = 0, r;
    while (e->rom && buf && (r = fread(buf, 1, 4096, f)) > 0) {
        memcpy(e->rom + done, buf, r);
        done += r;
    }
    heap_caps_free(buf);
    fclose(f);
    if (done != e->rom_size || e->rom_size < 0x8000) {
        snprintf(err, errlen, "could not read the ROM");
        goto fail;
    }
    // What the game takes later from internal RAM (the blitter's and the sound task's stacks, their queues)
    // is held aside while it picks fast memory, so the reserve is still free while it runs (it was not, and
    // Wi-Fi ran out during a long game). In 4 KB pieces: internal RAM is fragmented, and a stack is 4 KB.
    void *held[HELD_PIECES];
    for (int i = 0; i < HELD_PIECES; i++) held[i] = heap_caps_malloc(0x1000, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    e->gb.wram_half[0] = fh1_mem_fast(WRAM_SIZE / 2);
    e->gb.vram = fh1_mem_fast(VRAM_SIZE);
    for (int i = 0; i < 4; i++) e->bank0[i] = bank0_piece();
    e->gb.wram_half[1] = fh1_mem_fast(WRAM_SIZE / 2);
    for (int i = 0; i < HELD_PIECES; i++) heap_caps_free(held[i]);
    if (!e->gb.wram_half[0] || !e->gb.wram_half[1] || !e->gb.vram) {
        snprintf(err, errlen, "out of memory");
        goto fail;
    }
    for (int i = 0; i < 4; i++) {
        if (e->bank0[i]) memcpy(e->bank0[i], e->rom + i * 0x1000, 0x1000);
        else e->bank0[i] = e->rom + i * 0x1000;
    }

    for (int i = 0; i < 3; i++) {
        if (!s_log[i]) s_log[i] = heap_caps_malloc(SND_LOG * sizeof(snd_write_t), MALLOC_CAP_SPIRAM);
    }
    s_log_on = false;
    apu_core_init();                   // the APU's registers to power-on state (gb_init writes some of them)
    memcpy(s_regs, apu_core_registers(), sizeof(s_regs));
    enum gb_init_error_e ie = gb_init(&e->gb, rom_read, ram_read, ram_write, on_error, e);
    if (ie != GB_INIT_NO_ERROR) {
        snprintf(err, errlen, ie == GB_INIT_CARTRIDGE_UNSUPPORTED ? "cartridge type not supported" : "bad ROM (checksum)");
        goto fail;
    }
    e->cart_ram_size = gb_get_save_size(&e->gb);
    if (e->cart_ram_size) e->cart_ram = heap_caps_calloc(1, e->cart_ram_size, MALLOC_CAP_SPIRAM);   // rarely touched
    make_save_base(e);
    char sav[128];
    snprintf(sav, sizeof(sav), "%s.sav", e->save_base);
    if (e->cart_ram && (f = fopen(sav, "rb")) != NULL) {
        fread(e->cart_ram, 1, e->cart_ram_size, f);
        fclose(f);
    }
    gb_init_lcd(&e->gb, draw_line);
    gb_get_rom_name(&e->gb, info->title);
    info->color = e->gb.cgb.cgbMode;
    load_options(e);
    e->auto_combo = gbc_auto_combo(e->rom);
    build_dmg_lut(e);

    e->fb = fh1_board_framebuffer(&e->fb_w, &e->fb_h);
    for (int i = 0; i < 2; i++) e->frame[i] = heap_caps_calloc(LCD_WIDTH * LCD_HEIGHT, 2, MALLOC_CAP_SPIRAM);
    e->go = xSemaphoreCreateBinary();
    e->gone = xSemaphoreCreateBinary();
    e->snd = heap_caps_malloc(AUDIO_SAMPLES * 4, MALLOC_CAP_SPIRAM);
    e->audio = fh1_audio_open(AUDIO_SAMPLE_RATE);
    if (!e->frame[0] || !e->frame[1] || !e->go || !e->gone || !e->snd || !s_log[0] || !s_log[1] || !s_log[2] ||
        xTaskCreatePinnedToCore(blit_task, "gb_blit", 4096, e, 3, NULL, 0) != pdPASS) {
        snprintf(err, errlen, "out of memory");
        goto fail;
    }
    ESP_LOGI(TAG, "%s: %s, %u KB ROM, %u bytes save RAM; internal RAM for: state %s, WRAM %s/%s, VRAM %s, bank 0 %s",
             info->title, e->gb.cgb.cgbMode ? "Color" : "DMG", (unsigned)(e->rom_size / 1024), (unsigned)e->cart_ram_size,
             fh1_mem_is_fast(e) ? "yes" : "no", fh1_mem_is_fast(e->gb.wram_half[0]) ? "yes" : "no",
             fh1_mem_is_fast(e->gb.wram_half[1]) ? "yes" : "no", fh1_mem_is_fast(e->gb.vram) ? "yes" : "no",
             fh1_mem_is_fast(e->bank0[0]) + fh1_mem_is_fast(e->bank0[1]) + fh1_mem_is_fast(e->bank0[2]) + fh1_mem_is_fast(e->bank0[3]) == 4 ? "yes" : fh1_mem_is_fast(e->bank0[0]) ? "partly" : "no");
    fh1_mem_log("game loaded");
    return e;
fail:
    if (e->go) vSemaphoreDelete(e->go);
    if (e->gone) vSemaphoreDelete(e->gone);
    for (int i = 0; i < 2; i++) heap_caps_free(e->frame[i]);
    fh1_audio_close(e->audio);
    heap_caps_free(e->snd);
    free_bank0(e);
    heap_caps_free(e->rom);
    heap_caps_free(e->cart_ram);
    fh1_mem_free(e->gb.wram_half[0]);
    fh1_mem_free(e->gb.wram_half[1]);
    fh1_mem_free(e->gb.vram);
    fh1_mem_free(e);
    return NULL;
}

void fh1_gb_play(fh1_gb_t *e, fh1_gb_mode_t mode, fh1_gb_result_t *res)
{
    layout(e);
    fill_fb(e, BORDER);
    const int64_t frame_us = 16743;                 // 59.73 frames per second
    int64_t start = esp_timer_get_time(), next = start, last_report = start;
    uint32_t frames = 0, report_frames = 0, reports = 0, calm = 0;
    uint32_t calm_need = 10, full_secs = 0;   // auto drawing: seconds of calm before trying every frame again
    int64_t run_us = 0, snd_us = 0;
    s_apu_us = 0;
    bool menu_was_down = true;                      // the press that opened the game is not a MENU press
    int phase = 0;
    int64_t phase_start = start;
    e->draw = (mode == FH1_GB_PLAY);
    e->ready = -1;
    e->shown = 0;
    // Sound only while playing, and only with a ready audio device (it may have been switched on since).
    const bool sound = mode == FH1_GB_PLAY && fh1_audio_available() && snd_start(e);
    for (;;) {
        bool menu = fh1_input_is_down(FH1_KEY_MENU);
        if (menu && !menu_was_down && mode == FH1_GB_PLAY) break;
        menu_was_down = menu;
        // The system wants the screen (an event's overlay: playing only) or is shutting down.
        if (fh1_app_quit_requested() || (mode == FH1_GB_PLAY && fh1_app_should_yield())) break;
        struct gb_s *gb = &e->gb;
        gb->direct.joypad_bits.a = !fh1_input_is_down(FH1_KEY_A);
        gb->direct.joypad_bits.b = !fh1_input_is_down(FH1_KEY_B);
        gb->direct.joypad_bits.start = !fh1_input_is_down(FH1_KEY_START);
        gb->direct.joypad_bits.select = !fh1_input_is_down(FH1_KEY_SELECT);
        gb->direct.joypad_bits.up = !fh1_input_is_down(FH1_KEY_UP);
        gb->direct.joypad_bits.down = !fh1_input_is_down(FH1_KEY_DOWN);
        gb->direct.joypad_bits.left = !fh1_input_is_down(FH1_KEY_LEFT);
        gb->direct.joypad_bits.right = !fh1_input_is_down(FH1_KEY_RIGHT);
        // At half rate the emulator skips rendering every other frame (Peanut-GB's frame skip: lines are only
        // drawn while its counter is 1; it flips at each vertical blank), so the work is saved, not only
        // the blit.
        gb->direct.frame_skip = e->opts.drawing == FH1_GB_DRAW_HALF || (e->opts.drawing == FH1_GB_DRAW_AUTO && e->half_rate);
        const bool rendered = !gb->direct.frame_skip || gb->display.frame_skip_count;
        int64_t t_run = esp_timer_get_time();
        gb_run_frame(gb);
        if (sound) snd_us += snd_frame(e);
        run_us += esp_timer_get_time() - t_run;
        frame_done(e, rendered);
        frames++;
        report_frames++;
        int64_t now = esp_timer_get_time();
        if (mode == FH1_GB_PLAY) {
            next += frame_us;
            if (next > now && !e->opts.turbo) {
                vTaskDelay(pdMS_TO_TICKS((next - now) / 1000));
            } else {
                // Drawing every frame and 20 ms behind: back to half rate now, not at the next 1 s check (a
                // heavy scene would otherwise run slow for that second), and back off the next try.
                if (now - next > 20000 && e->opts.drawing == FH1_GB_DRAW_AUTO && !e->half_rate) {
                    e->half_rate = true;
                    calm = 0;
                    if (full_secs < 60) calm_need = calm_need * 2 > 600 ? 600 : calm_need * 2;
                }
                if (now - next > 100000) next = now;       // too far behind: stop catching up
                // A loop that never blocks starves core 1's idle task; the task watchdog then prints from an
                // interrupt for ~15 ms every 5 s, long enough to starve the panel's refill (screen glitches).
                if ((frames & 3) == 0) vTaskDelay(1);
            }
            if (now - last_report >= 1000000) {
                float emulated = report_frames * 1e6f / (now - last_report);
                float shown = e->shown * 1e6f / (now - last_report);
                // Auto drawing: below full speed, draw half the frames; with room to spare, all of them.
                // A try at every frame that fails within a minute doubles the wait before the next try (10 s up
                // to 10 min): drawing every frame can slow the machine itself (PSRAM), and retrying every 10 s
                // made the game slow down for a few seconds each time.
                if (!e->half_rate && emulated < 58.0f) {
                    e->half_rate = true;
                    if (full_secs < 60) calm_need = calm_need * 2 > 600 ? 600 : calm_need * 2;
                } else if (e->half_rate && emulated > 59.5f && ++calm >= calm_need) {
                    e->half_rate = false;
                    full_secs = 0;
                }
                if (!e->half_rate) {
                    calm = 0;
                    if (++full_secs >= 60) calm_need = 10;
                }
                if (e->opts.show_fps) show_fps(e, emulated, shown);
                if (++reports % 5 == 0) {
                    ESP_LOGI(TAG, "%.1f frames/s emulated (%.1f ms each, of which %.1f ms waiting on sound; sound on core 0: %.1f ms), %.1f shown",
                             emulated, report_frames ? run_us / 1000.0f / report_frames : 0.0f,
                             report_frames ? snd_us / 1000.0f / report_frames : 0.0f,
                             report_frames ? s_apu_us / 1000.0f / report_frames : 0.0f, shown);
                }
                run_us = snd_us = 0;
                s_apu_us = 0;
                last_report = now;
                report_frames = 0;
                e->shown = 0;
            }
        } else {
            if (now - phase_start >= 10000000) {
                float fps = frames * 1e6f / (now - phase_start);
                if (phase == 0) {
                    res->fps_emulation = fps;
                    ESP_LOGI(TAG, "benchmark: emulation only %.1f frames/s", fps);
                    phase = 1;
                    e->draw = true;
                } else {
                    res->fps_with_display = fps;
                    res->fps_shown = e->shown * 1e6f / (now - phase_start);
                    ESP_LOGI(TAG, "benchmark: emulation %.1f frames/s while drawing, %.1f shown", fps, res->fps_shown);
                    break;
                }
                frames = 0;
                e->shown = 0;
                phase_start = now;
            }
            if ((frames & 63) == 0) vTaskDelay(1);         // let the idle task feed the watchdog
        }
    }
    if (sound) snd_stop(e);
    // Let the last frame finish before the menu draws over the picture.
    for (int i = 0; i < 50 && e->blit_busy; i++) vTaskDelay(1);
}

void fh1_gb_close(fh1_gb_t *e)
{
    if (!e) return;
    if (e->cart_ram && e->cart_ram_size) {
        char sav[128];
        snprintf(sav, sizeof(sav), "%s.sav", e->save_base);
        fh1_mkdirs_parent(sav);
        FILE *f = fopen(sav, "wb");
        if (f) {
            fwrite(e->cart_ram, 1, e->cart_ram_size, f);
            fclose(f);
            fh1_int_info_changed();
            ESP_LOGI(TAG, "saved %s", sav);
        }
    }
    e->stop = true;
    xSemaphoreTake(e->gone, pdMS_TO_TICKS(1000));
    fill_fb(e, 0x10A2);
    vSemaphoreDelete(e->go);
    vSemaphoreDelete(e->gone);
    for (int i = 0; i < 2; i++) heap_caps_free(e->frame[i]);
    fh1_audio_close(e->audio);
    heap_caps_free(e->snd);
    if (e->snd_q) vQueueDelete(e->snd_q);
    if (e->snd_gone) vSemaphoreDelete(e->snd_gone);
    free_bank0(e);
    heap_caps_free(e->rom);
    heap_caps_free(e->cart_ram);
    fh1_mem_free(e->gb.wram_half[0]);
    fh1_mem_free(e->gb.wram_half[1]);
    fh1_mem_free(e->gb.vram);
    fh1_mem_free(e);
}
