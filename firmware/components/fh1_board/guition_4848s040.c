// Guition ESP32-4848S040: ESP32-S3 N16R8, 4" 480 x 480 ST7701S on the parallel RGB bus, GT911 touch,
// micro-SD over SPI, CH340 on UART0. Pins from ESPHome's GUITION-4848S040 preset and the vendor table
// (firmware/PLAN.md, "Boards").
#include <stdio.h>
#include <string.h>
#include "fh1_board.h"
#include "fh1_devices.h"
#include "fh1_settings.h"
#include "fh1_storage.h"
#include "fh1_pcm5102a.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_timer.h"
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/freertos_debug.h"

static const char *TAG = "board";

#define LCD_W 480
#define LCD_H 480
#define PIN_LCD_CS   39       // 3-wire SPI for the ST7701S commands; SCK and SDA are later the SD card's
#define PIN_LCD_SCK  48
#define PIN_LCD_SDA  47
#define PIN_BL       38
#define PIN_TP_SDA   19
#define PIN_TP_SCL   45
#define PIN_SD_MISO  41
#define PIN_SD_CS    42

// Screen modes. The screen is kept at its logical size in a small PSRAM buffer; the panel driver asks
// for each band of lines just before it sends them, and bounce_fill() scales them into internal RAM.
// No full-size frame buffer: the panel reads a quarter of the PSRAM it used to (the glitches came from
// it starving while PSRAM was busy). The first mode is the default.
static const fh1_display_mode_t s_modes[] = {
    { "native", "Native 480x480 (2x)", 240, 240, 1, 2, 1 },
    { "fh1", "FH1 320x240 (1.5x)", 320, 240, 1, 3, 2 },
};
static const fh1_display_mode_t *s_mode = &s_modes[0];
static int s_x0, s_y0;
#define BORDER 0x10A2

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_src;                       // the logical screen, PSRAM
static DRAM_ATTR uint16_t s_xmap[LCD_W];      // panel column -> screen column, 0xFFFF outside
static DRAM_ATTR int16_t s_ymap[LCD_H];       // panel row -> screen row, -1 outside
static DRAM_ATTR int s_src_w;
static DRAM_ATTR bool s_double;               // exact 2x across the whole panel: a fast path

int fh1_board_display_modes(const fh1_display_mode_t **modes)
{
    *modes = s_modes;
    return sizeof(s_modes) / sizeof(s_modes[0]);
}

const fh1_display_mode_t *fh1_board_display_mode(void) { return s_mode; }

void fh1_board_display_set_mode(const char *id)
{
    for (size_t i = 0; i < sizeof(s_modes) / sizeof(s_modes[0]); i++) {
        if (id && strcmp(s_modes[i].id, id) == 0) s_mode = &s_modes[i];
    }
    const int num = s_mode->num, den = s_mode->den;
    s_x0 = (LCD_W - s_mode->w * num / den) / 2;
    s_y0 = (LCD_H - s_mode->h * num / den) / 2;
    // Logical pixel x covers panel columns [x * num / den, (x + 1) * num / den).
    for (int i = 0; i < LCD_W; i++) s_xmap[i] = 0xFFFF;
    for (int i = 0; i < LCD_H; i++) s_ymap[i] = -1;
    for (int x = 0; x < s_mode->w; x++) {
        for (int d = x * num / den; d < (x + 1) * num / den; d++) s_xmap[s_x0 + d] = x;
    }
    for (int y = 0; y < s_mode->h; y++) {
        for (int d = y * num / den; d < (y + 1) * num / den; d++) s_ymap[s_y0 + d] = y;
    }
    s_src_w = s_mode->w;
    s_double = (num == 2 * den && s_x0 == 0);
}

const char *fh1_board_name(void) { return "Guition ESP32-4848S040 (dev)"; }

// ── ST7701S init over 9-bit 3-wire SPI (first bit: 0 command, 1 data), bit-banged ──

static void spi9(bool data, uint8_t b)
{
    uint16_t word = (data ? 0x100 : 0) | b;
    for (int i = 8; i >= 0; i--) {
        gpio_set_level(PIN_LCD_SCK, 0);
        gpio_set_level(PIN_LCD_SDA, (word >> i) & 1);
        esp_rom_delay_us(1);
        gpio_set_level(PIN_LCD_SCK, 1);
        esp_rom_delay_us(1);
    }
}

static void lcd_cmd(const uint8_t *c, int n)
{
    gpio_set_level(PIN_LCD_CS, 0);
    spi9(false, c[0]);
    for (int i = 1; i < n; i++) spi9(true, c[i]);
    gpio_set_level(PIN_LCD_CS, 1);
    esp_rom_delay_us(2);
}

#define CMD(...) do { static const uint8_t c_[] = { __VA_ARGS__ }; lcd_cmd(c_, sizeof(c_)); } while (0)

static void st7701s_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_LCD_CS) | (1ULL << PIN_LCD_SCK) | (1ULL << PIN_LCD_SDA),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(PIN_LCD_CS, 1);
    gpio_set_level(PIN_LCD_SCK, 1);

    CMD(0x01);                                  // software reset (no reset pin on this board)
    vTaskDelay(pdMS_TO_TICKS(50));
    // ESPHome's ST7701S sequence
    CMD(0xFF, 0x77, 0x01, 0x00, 0x00, 0x10);    // page 0
    CMD(0xC0, 0x3B, 0x00); CMD(0xC1, 0x0D, 0x02); CMD(0xC2, 0x31, 0x05);
    CMD(0xB0, 0x00, 0x11, 0x18, 0x0E, 0x11, 0x06, 0x07, 0x08, 0x07, 0x22, 0x04, 0x12, 0x0F, 0xAA, 0x31, 0x18);
    CMD(0xB1, 0x00, 0x11, 0x19, 0x0E, 0x12, 0x07, 0x08, 0x08, 0x08, 0x22, 0x04, 0x11, 0x11, 0xA9, 0x32, 0x18);
    CMD(0xFF, 0x77, 0x01, 0x00, 0x00, 0x11);    // page 1
    CMD(0xB0, 0x60); CMD(0xB1, 0x32); CMD(0xB2, 0x07); CMD(0xB3, 0x80); CMD(0xB5, 0x49); CMD(0xB7, 0x85);
    CMD(0xB8, 0x21); CMD(0xC1, 0x78); CMD(0xC2, 0x78); CMD(0xE0, 0x00, 0x1B, 0x02);
    CMD(0xE1, 0x08, 0xA0, 0x00, 0x00, 0x07, 0xA0, 0x00, 0x00, 0x00, 0x44, 0x44);
    CMD(0xE2, 0x11, 0x11, 0x44, 0x44, 0xED, 0xA0, 0x00, 0x00, 0xEC, 0xA0, 0x00, 0x00);
    CMD(0xE3, 0x00, 0x00, 0x11, 0x11);
    CMD(0xE4, 0x44, 0x44);
    CMD(0xE5, 0x0A, 0xE9, 0xD8, 0xA0, 0x0C, 0xEB, 0xD8, 0xA0, 0x0E, 0xED, 0xD8, 0xA0, 0x10, 0xEF, 0xD8, 0xA0);
    CMD(0xE6, 0x00, 0x00, 0x11, 0x11); CMD(0xE7, 0x44, 0x44);
    CMD(0xE8, 0x09, 0xE8, 0xD8, 0xA0, 0x0B, 0xEA, 0xD8, 0xA0, 0x0D, 0xEC, 0xD8, 0xA0, 0x0F, 0xEE, 0xD8, 0xA0);
    CMD(0xEB, 0x02, 0x00, 0xE4, 0xE4, 0x88, 0x00, 0x40); CMD(0xEC, 0x3C, 0x00);
    CMD(0xED, 0xAB, 0x89, 0x76, 0x54, 0x02, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x20, 0x45, 0x67, 0x98, 0xBA);
    CMD(0xFF, 0x77, 0x01, 0x00, 0x00, 0x13);    // page 3
    CMD(0xE5, 0xE4);
    CMD(0xFF, 0x77, 0x01, 0x00, 0x00, 0x10);    // page 0
    CMD(0xCD, 0x08);
    CMD(0xCD, 0x00);                            // Guition 4848S040: 16-bit bus
    CMD(0x3A, 0x66);                            // pixel format (as ESPHome's "18bit")
    CMD(0x36, 0x00);                            // MADCTL
    CMD(0xC7, 0x00);                            // SDIR: normal scan (0x04 mirrored the image left-right)
    CMD(0x20);                                  // inversion off
    vTaskDelay(pdMS_TO_TICKS(120));
    CMD(0x11);                                  // sleep out
    vTaskDelay(pdMS_TO_TICKS(120));
    CMD(0x29);                                  // display on
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_LCD_CS, 1);
    // SCK and SDA go to the SD card's SPI bus from here on; CS stays high.
    gpio_reset_pin(PIN_LCD_SCK);
    gpio_reset_pin(PIN_LCD_SDA);
    gpio_reset_pin(PIN_SD_CS);      // the SD driver owns its chip select from here on
}

// ── Display health: is the panel being fed in time? ──
// The DMA sends one band (20 lines) in about 20 * 520 px / 10 MHz = 1.04 ms while the other band is being
// filled; a fill that takes longer, or a frame that does not get all its bands, is a visible glitch.
#define BAND_ROWS 20
static DRAM_ATTR volatile uint32_t st_frames, st_fills, st_late, st_short_frames, st_max_fill_cyc;
static DRAM_ATTR volatile uint32_t st_vs_min_us = 0xffffffff, st_vs_max_us, st_frame_fills;
static DRAM_ATTR volatile int64_t st_last_vs;
static DRAM_ATTR uint32_t s_late_cyc;          // cycles in 0.9 ms
// Refills that start late: one band takes ~1.04 ms to send, so the next refill should start about that
// long after the previous one. A longer gap means the interrupt was held up and the DMA may have sent a
// band that was not ready.
static DRAM_ATTR volatile uint32_t st_late_starts, st_max_gap_cyc, st_prev_fill_cyc;
static DRAM_ATTR volatile bool st_first_in_frame = true;

// The glitch recorder: each late refill and each short frame, with the task running on each core at that
// moment (the interrupt only stores; a 1 s timer logs them as "disp:" lines). Who held the interrupt up, or
// what was hammering PSRAM from the other core, is usually one of the two.
typedef struct {
    uint32_t t_ms;
    uint32_t gap_us;          // 0 for a short frame
    void *task[2];
} glitch_t;
#define GLITCHES 32
static DRAM_ATTR glitch_t s_gl[GLITCHES];
static DRAM_ATTR volatile uint32_t s_gl_w;    // events written (index = count % GLITCHES)

// FreeRTOS's running task per core (a global its port uses; xTaskGetCurrentTaskHandleForCore is in flash,
// which an interrupt that must run while flash is busy cannot call).
extern void *volatile pxCurrentTCBs[];

static void IRAM_ATTR glitch_note(uint32_t gap_us)
{
    glitch_t *g = &s_gl[s_gl_w % GLITCHES];
    g->t_ms = (uint32_t)(esp_timer_get_time() / 1000);
    g->gap_us = gap_us;
    g->task[0] = pxCurrentTCBs[0];
    g->task[1] = pxCurrentTCBs[1];
    s_gl_w++;
}

static bool IRAM_ATTR on_vsync(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t *ed, void *ctx)
{
    int64_t now = esp_timer_get_time();
    if (st_last_vs) {
        uint32_t d = (uint32_t)(now - st_last_vs);
        if (d < st_vs_min_us) st_vs_min_us = d;
        if (d > st_vs_max_us) st_vs_max_us = d;
    }
    st_last_vs = now;
    if (st_frames && st_frame_fills != LCD_H / BAND_ROWS) {
        st_short_frames++;
        glitch_note(0);
    }
    st_frame_fills = 0;
    st_first_in_frame = true;
    st_frames++;
    return false;
}

// A task's name, if it still exists (the handle was taken in the interrupt; the task may be gone since).
static const char *task_name(void *h)
{
    if (!h) return "-";
    UBaseType_t n = uxTaskGetNumberOfTasks() + 4;
    TaskSnapshot_t snaps[48];
    if (n > 48) n = 48;
    UBaseType_t tcb;
    n = uxTaskGetSnapshotAll(snaps, n, &tcb);
    for (UBaseType_t i = 0; i < n; i++) {
        if (snaps[i].pxTCB == h) return pcTaskGetName((TaskHandle_t)h);
    }
    return "?";
}

static void glitch_log(void *arg)
{
    static uint32_t r;
    uint32_t w = s_gl_w;
    if (w - r > GLITCHES) {
        ESP_LOGW("disp", "%lu glitches not shown (too many)", (unsigned long)(w - r - GLITCHES));
        r = w - GLITCHES;
    }
    for (int shown = 0; r != w; r++) {
        glitch_t g = s_gl[r % GLITCHES];
        if (++shown > 6) {                      // a burst: the count is enough
            ESP_LOGW("disp", "... and %lu more", (unsigned long)(w - r));
            r = w;
            break;
        }
        if (g.gap_us) ESP_LOGW("disp", "late refill %lu us at %lu ms; running: core0 %s, core1 %s", (unsigned long)g.gap_us,
                               (unsigned long)g.t_ms, task_name(g.task[0]), task_name(g.task[1]));
        else ESP_LOGW("disp", "short frame at %lu ms; running: core0 %s, core1 %s", (unsigned long)g.t_ms,
                      task_name(g.task[0]), task_name(g.task[1]));
    }
}

void fh1_board_display_stats(fh1_display_stats_t *out, bool reset)
{
    out->frames = st_frames;
    out->fills = st_fills;
    out->late_fills = st_late;
    out->odd_frames = st_short_frames;
    out->max_fill_us = st_max_fill_cyc / 240;
    out->late_starts = st_late_starts;
    out->max_gap_us = st_max_gap_cyc / 240;
    out->vsync_min_us = st_vs_min_us == 0xffffffff ? 0 : st_vs_min_us;
    out->vsync_max_us = st_vs_max_us;
    if (reset) {
        st_frames = st_fills = st_late = st_short_frames = st_max_fill_cyc = 0;
        st_late_starts = st_max_gap_cyc = 0;
        st_vs_min_us = 0xffffffff;
        st_vs_max_us = 0;
    }
}

// Fill one band of panel lines (called from the panel's interrupt, so in IRAM and quick): rows that show
// the same screen row are copied, the 2x mode doubles pixels with 32-bit writes.
static bool IRAM_ATTR bounce_fill(esp_lcd_panel_handle_t panel, void *buf, int pos_px, int len_bytes, void *ctx)
{
    const uint32_t c0 = esp_cpu_get_cycle_count();
    if (!st_first_in_frame) {
        const uint32_t gap = c0 - st_prev_fill_cyc;
        if (gap > st_max_gap_cyc) st_max_gap_cyc = gap;
        if (gap > 240 * 1500) {
            st_late_starts++;
            glitch_note(gap / 240);
        }
    }
    st_first_in_frame = false;
    st_prev_fill_cyc = c0;
    uint16_t *out = buf;
    const int row0 = pos_px / LCD_W, rows = len_bytes / (LCD_W * 2);
    int prev = -2;
    const uint16_t *prev_out = NULL;
    for (int r = 0; r < rows; r++, out += LCD_W) {
        const int sy = s_ymap[row0 + r];
        if (sy < 0) {
            uint32_t *w = (uint32_t *)out;
            for (int i = 0; i < LCD_W / 2; i++) w[i] = BORDER | BORDER << 16;
            prev = -2;
            continue;
        }
        if (sy == prev) {
            memcpy(out, prev_out, LCD_W * 2);
            continue;
        }
        const uint16_t *src = s_src + sy * s_src_w;
        if (s_double) {
            uint32_t *w = (uint32_t *)out;
            for (int x = 0; x < LCD_W / 2; x++) {
                uint32_t p = src[x];
                w[x] = p | p << 16;
            }
        } else {
            for (int x = 0; x < LCD_W; x++) {
                const uint16_t m = s_xmap[x];
                out[x] = m == 0xFFFF ? BORDER : src[m];
            }
        }
        prev = sy;
        prev_out = out;
    }
    const uint32_t c = esp_cpu_get_cycle_count() - c0;
    if (c > st_max_fill_cyc) st_max_fill_cyc = c;
    if (c > s_late_cyc) st_late++;
    st_fills++;
    st_frame_fills++;
    return false;
}

static esp_err_t rgb_init_here(void)
{
    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = 10 * 1000 * 1000,      // ~40 frames/s
            .h_res = LCD_W, .v_res = LCD_H,
            .hsync_pulse_width = 10, .hsync_back_porch = 10, .hsync_front_porch = 20,
            .vsync_pulse_width = 10, .vsync_back_porch = 10, .vsync_front_porch = 10,
            .flags.pclk_active_neg = 0,
        },
        .data_width = 16,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .bounce_buffer_size_px = LCD_W * BAND_ROWS,  // two of these in internal RAM; 480 rows = 24 bands
        .dma_burst_size = 64,
        .hsync_gpio_num = 16, .vsync_gpio_num = 17, .de_gpio_num = 18, .pclk_gpio_num = 21,
        .disp_gpio_num = -1,
        // D0..D15 = B0..B4, G0..G5, R0..R4
        .data_gpio_nums = { 4, 5, 6, 7, 15, 8, 20, 3, 46, 9, 10, 11, 12, 13, 14, 0 },
        .flags.no_fb = 1,                        // bounce_fill() makes every line
    };
    s_src = heap_caps_malloc(s_mode->w * s_mode->h * 2, MALLOC_CAP_SPIRAM);
    if (!s_src) return ESP_ERR_NO_MEM;
    for (int i = 0; i < s_mode->w * s_mode->h; i++) s_src[i] = BORDER;
    esp_err_t err = esp_lcd_new_rgb_panel(&cfg, &s_panel);
    if (err != ESP_OK) return err;
    s_late_cyc = 240 * 900;
    esp_lcd_rgb_panel_event_callbacks_t cbs = { .on_bounce_empty = bounce_fill, .on_vsync = on_vsync };
    static esp_timer_handle_t gl_timer;
    if (!gl_timer) {
        esp_timer_create_args_t ta = { .callback = glitch_log, .name = "disp_glitch" };
        if (esp_timer_create(&ta, &gl_timer) == ESP_OK) esp_timer_start_periodic(gl_timer, 1000 * 1000);
    }
    err = esp_lcd_rgb_panel_register_event_callbacks(s_panel, &cbs, NULL);
    if (err == ESP_OK) err = esp_lcd_panel_reset(s_panel);
    if (err == ESP_OK) err = esp_lcd_panel_init(s_panel);
    return err;
}

// The panel's interrupt (it refills the bounce buffers from PSRAM) is set up on the core that creates
// the panel. Create it on core 1, away from Wi-Fi, which lives on core 0.
typedef struct {
    esp_err_t err;
    SemaphoreHandle_t done;
} rgb_job_t;

static void rgb_task(void *arg)
{
    rgb_job_t *job = arg;
    job->err = rgb_init_here();
    xSemaphoreGive(job->done);
    vTaskDelete(NULL);
}

static esp_err_t rgb_init(void)
{
    rgb_job_t job = { .err = ESP_FAIL, .done = xSemaphoreCreateBinary() };
    xTaskCreatePinnedToCore(rgb_task, "rgb_init", 4096, &job, 5, NULL, 1);
    xSemaphoreTake(job.done, portMAX_DELAY);
    vSemaphoreDelete(job.done);
    return job.err;
}

uint16_t *fh1_board_framebuffer(int *w, int *h)
{
    *w = s_mode->w;
    *h = s_mode->h;
    return s_src;
}

// The RGB panel scans the framebuffer by itself: nothing to send.
void fh1_board_direct(bool on) {}
void fh1_board_changed(int y1, int y2) {}

void fh1_board_flush(int x1, int y1, int x2, int y2, const uint16_t *px)
{
    // The panel reads the screen through the CPU cache (bounce_fill), so a plain copy is enough.
    const int w = x2 - x1 + 1;
    for (int y = y1; y <= y2; y++) memcpy(s_src + y * s_mode->w + x1, px + (y - y1) * w, w * 2);
}

// ── Backlight (display.backlight) ──

static bool s_backlight_ready;

uint8_t fh1_board_backlight_level(void) { return fh1_settings_get_u8("display", "bl", 80); }

static uint8_t bl_level(void) { return fh1_settings_get_u8("display", "bl", 80); }

static void bl_apply(uint8_t pct)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (pct * 1023) / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static esp_err_t bl_probe(fh1_device_t *d)
{
    static bool done;
    if (!done) {
        ledc_timer_config_t t = { .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
                                  .timer_num = LEDC_TIMER_0, .freq_hz = 150, .clk_cfg = LEDC_AUTO_CLK };
        // 150 Hz like ESPHome's config for this board: the backlight driver wants a slow PWM.
        ledc_channel_config_t c = { .gpio_num = PIN_BL, .speed_mode = LEDC_LOW_SPEED_MODE,
                                    .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 0 };
        ESP_ERROR_CHECK(ledc_timer_config(&t));
        ESP_ERROR_CHECK(ledc_channel_config(&c));
        done = true;
    }
    strlcpy(d->info, "PWM on GPIO38", sizeof(d->info));
    return ESP_OK;
}
static esp_err_t bl_start(fh1_device_t *d) { s_backlight_ready = true; bl_apply(bl_level()); return ESP_OK; }

void fh1_board_backlight_preview(uint8_t pct)
{
    if (s_backlight_ready) bl_apply(pct > 100 ? 100 : pct);
}

void fh1_board_backlight(uint8_t pct)
{
    fh1_settings_set_u8("display", "bl", pct > 100 ? 100 : pct);
    if (s_backlight_ready) bl_apply(pct > 100 ? 100 : pct);
}
static void bl_stop(fh1_device_t *d) { bl_apply(0); s_backlight_ready = false; }
static const fh1_driver_t s_bl_drv = { .name = "ledc", .probe = bl_probe, .start = bl_start, .stop = bl_stop };

// ── Touch controller (input.touch): probed, but not used as input ──

static i2c_master_bus_handle_t s_i2c;     // the board's only I2C bus (touch connector), created at init

static void i2c_init_and_scan(void)
{
    i2c_master_bus_config_t bc = { .i2c_port = 0, .sda_io_num = PIN_TP_SDA, .scl_io_num = PIN_TP_SCL,
                                   .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
                                   .flags.enable_internal_pullup = true };
    if (i2c_new_master_bus(&bc, &s_i2c) != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus not available");
        return;
    }
    char found[96] = "";
    for (int a = 0x08; a < 0x78; a++) {
        if (i2c_master_probe(s_i2c, a, 20) == ESP_OK) {
            char t[8];
            snprintf(t, sizeof(t), " 0x%02X", a);
            strlcat(found, t, sizeof(found));
        }
    }
    ESP_LOGI(TAG, "I2C scan (SDA %d, SCL %d):%s", PIN_TP_SDA, PIN_TP_SCL, found[0] ? found : " nothing");
}

static esp_err_t gt911_probe(fh1_device_t *d)
{
    if (!s_i2c) return ESP_ERR_INVALID_STATE;
    static const uint8_t addrs[] = { 0x5D, 0x14 };
    for (size_t i = 0; i < sizeof(addrs); i++) {
        if (i2c_master_probe(s_i2c, addrs[i], 50) == ESP_OK) {
            snprintf(d->info, sizeof(d->info), "GT911 at I2C 0x%02X", addrs[i]);
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}
static const fh1_driver_t s_gt911_drv = { .name = "gt911", .probe = gt911_probe };

// ── Real-time clock (rtc): looked for on the I2C bus; the vendor does not list one ──
// PCF8563 / BM8563 at 0x51 is read at start-up to set the system clock until network time arrives.

static i2c_master_dev_handle_t s_rtc;

static int bcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0f); }

static esp_err_t rtc_probe(fh1_device_t *d)
{
    if (!s_i2c) return ESP_ERR_INVALID_STATE;
    if (i2c_master_probe(s_i2c, 0x51, 30) == ESP_OK) {
        strlcpy(d->info, "PCF8563/BM8563 at I2C 0x51", sizeof(d->info));
        return ESP_OK;
    }
    if (i2c_master_probe(s_i2c, 0x68, 30) == ESP_OK) {
        strlcpy(d->info, "DS3231/DS1307 at I2C 0x68", sizeof(d->info));
        return ESP_ERR_NOT_SUPPORTED;     // found, but no driver yet
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t rtc_start(fh1_device_t *d)
{
    if (!s_rtc) {
        i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x51, .scl_speed_hz = 100000 };
        ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &dc, &s_rtc), TAG, "rtc");
    }
    uint8_t reg = 0x02, v[7];
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(s_rtc, &reg, 1, v, 7, 100), TAG, "rtc read");
    if (v[0] & 0x80) {
        strlcat(d->info, ", time not set", sizeof(d->info));
        return ESP_OK;
    }
    struct tm tm = { .tm_sec = bcd(v[0] & 0x7f), .tm_min = bcd(v[1] & 0x7f), .tm_hour = bcd(v[2] & 0x3f),
                     .tm_mday = bcd(v[3] & 0x3f), .tm_mon = bcd(v[5] & 0x1f) - 1, .tm_year = 100 + bcd(v[6]) };
    if (time(NULL) < 1700000000) {
        struct timeval tv = { .tv_sec = mktime(&tm) };
        settimeofday(&tv, NULL);
    }
    return ESP_OK;
}
static const fh1_driver_t s_rtc_drv = { .name = "rtc-i2c", .probe = rtc_probe, .start = rtc_start };

// ── Display (display) ──

static esp_err_t disp_probe(fh1_device_t *d)
{
    snprintf(d->info, sizeof(d->info), "ST7701S 480x480, %s", s_mode->name);
    return s_panel ? ESP_OK : ESP_FAIL;
}
static const fh1_driver_t s_disp_drv = { .name = "st7701s", .probe = disp_probe };

// ── Devices ──

static fh1_device_t s_display = { .id = "display0", .cls = "display", .caps = FH1_CAP_DISPLAY,
                                  .drv = &s_disp_drv, .default_on = true, .essential = true, .ma = 40 };
static fh1_device_t s_backlight = { .id = "backlight0", .cls = "display.backlight", .caps = FH1_CAP_BACKLIGHT,
                                    .drv = &s_bl_drv, .default_on = true, .ma = 120 };
static fh1_device_t s_rtc_dev = { .id = "rtc0", .cls = "rtc", .caps = FH1_CAP_RTC, .drv = &s_rtc_drv,
                                  .default_on = true, .ma = 1 };
static fh1_device_t s_touch = { .id = "touch0", .cls = "input.touch", .caps = FH1_CAP_TOUCH,
                                .drv = &s_gt911_drv, .default_on = false, .ma = 5 };
static const fh1_sd_spi_cfg_t s_sd_cfg = { .host = 1 /* SPI2_HOST */, .mosi = PIN_LCD_SDA, .miso = PIN_SD_MISO,
                                           .sclk = PIN_LCD_SCK, .cs = PIN_SD_CS };
static fh1_device_t s_sd = { .id = "sd0", .cls = "storage.sd", .caps = FH1_CAP_SD, .drv = &fh1_sd_spi_driver,
                             .cfg = &s_sd_cfg, .default_on = true, .ma = 30 };

// A PCM5102A on the H1 header (GPIO1 BCK, GPIO2 LCK, GPIO40 DIN): off until the user enables it.
static const fh1_pcm5102a_cfg_t s_pcm_cfg = { .bck = 1, .ws = 2, .dout = 40 };
static fh1_device_t s_audio = { .id = "audio0", .cls = "audio.out", .caps = FH1_CAP_AUDIO_OUT,
                                .drv = &fh1_pcm5102a_driver, .cfg = &s_pcm_cfg, .default_on = false, .ma = 10 };

esp_err_t fh1_board_init(void)
{
    // SD chip select high before anything touches the shared lines.
    gpio_set_direction(PIN_SD_CS, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_SD_CS, 1);
    char mode[16];
    fh1_settings_get_str("display", "mode", mode, sizeof(mode));
    fh1_board_display_set_mode(mode);
    st7701s_init();
    i2c_init_and_scan();
    esp_err_t err = rgb_init();
    if (err != ESP_OK) ESP_LOGE(TAG, "RGB panel: %s", esp_err_to_name(err));
    fh1_dev_register(&s_display);
    fh1_dev_register(&s_backlight);
    fh1_dev_register(&s_touch);
    fh1_dev_register(&s_rtc_dev);
    fh1_dev_register(&s_sd);
    fh1_dev_register(&s_audio);
    return err;
}
