// The FH1 itself (hardware/wiring.md): an ST7789 320 x 240 over SPI, the face buttons and their lights on
// MCP23017 #1, the stick on the ADC as the D-pad, Menu on GPIO0, a PCF8563 clock and the codec's I2S, with the
// amplifier switched on through MCP23017 #2. Not yet: the ES8311's own set-up, micro-SD (1-bit SDMMC), the
// battery, the IMU, the radio.
//
// The display is fed as the dev board's RGB panel is: everything draws into one 320 x 240 screen in PSRAM (the
// UI's flush, full-screen apps, the emulators), and a task sends it to the panel about 30 times a second over
// SPI DMA, in bands of lines copied (byte-swapped: the ST7789 takes the high byte first) into internal RAM.
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "fh1_board.h"
#include "fh1_pcm5102a.h"
#include "fh1_devices.h"
#include "fh1_input.h"
#include "fh1_settings.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_adc/adc_oneshot.h"

static const char *TAG = "board";

// GPIOs (hardware/wiring.md, "GPIO map").
#define PIN_LCD_SCK   18
#define PIN_LCD_MOSI  39
#define PIN_LCD_RST   48
#define PIN_LCD_DC    47
#define PIN_LCD_CS    38
#define PIN_LCD_BL    40
#define PIN_I2S_BCK   14
#define PIN_I2S_WS    10
#define PIN_I2S_DOUT  12
#define PIN_I2C_SCL   1
#define PIN_I2C_SDA   3
#define PIN_STICK_X   2
#define PIN_STICK_Y   4
#define PIN_LED_PWM   44
#define PIN_MENU      0

#define LCD_W 320
#define LCD_H 240
#define BAND 20                         // lines per SPI transfer: two bands in internal RAM, 25 KB
#define FRAME_MS 33

static const fh1_display_mode_t s_modes[] = { { "native", "FH1 320x240", LCD_W, LCD_H, 1, 1, 1 } };

int fh1_board_display_modes(const fh1_display_mode_t **modes)
{
    *modes = s_modes;
    return 1;
}
const fh1_display_mode_t *fh1_board_display_mode(void) { return &s_modes[0]; }
void fh1_board_display_set_mode(const char *id) {}
const char *fh1_board_name(void) { return "FH1"; }

// ── Display (display): the ST7789 and the task that keeps it fed ──

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_src;                         // the screen, PSRAM
static uint16_t *s_band[2];                     // DMA bands, internal RAM
static SemaphoreHandle_t s_band_free[2];
static volatile uint32_t st_frames, st_fills, st_late, st_max_fill_us;
static int s_sending;                           // the band whose transfer completes next
#define BANDS (LCD_H / BAND)
// What changed since it was last sent, as one rectangle per band (x1 > x2: nothing): only that rectangle goes to
// the panel, so a blinking cursor costs its own pixels, not a band of 320 x 20. While a full-screen program draws
// into the framebuffer directly (s_direct), every band goes whole each frame.
typedef struct { int16_t x1, y1, x2, y2; } rect_t;
static rect_t s_dirty[BANDS] = { [0 ... BANDS - 1] = { 1, 0, 0, 0 } };   // empty: x1 > x2
static portMUX_TYPE s_dirty_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_direct;

static void mark(int x1, int y1, int x2, int y2)
{
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= LCD_W) x2 = LCD_W - 1;
    if (y2 >= LCD_H) y2 = LCD_H - 1;
    if (x1 > x2 || y1 > y2) return;
    taskENTER_CRITICAL(&s_dirty_mux);
    for (int b = y1 / BAND; b <= y2 / BAND; b++) {
        rect_t *r = &s_dirty[b];
        const int by1 = y1 > b * BAND ? y1 : b * BAND, by2 = y2 < b * BAND + BAND - 1 ? y2 : b * BAND + BAND - 1;
        if (r->x1 > r->x2) *r = (rect_t){ x1, by1, x2, by2 };
        else {
            if (x1 < r->x1) r->x1 = x1;
            if (x2 > r->x2) r->x2 = x2;
            if (by1 < r->y1) r->y1 = by1;
            if (by2 > r->y2) r->y2 = by2;
        }
    }
    taskEXIT_CRITICAL(&s_dirty_mux);
}

static bool on_band_sent(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *ed, void *ctx)
{
    BaseType_t woke = pdFALSE;
    xSemaphoreGiveFromISR(s_band_free[s_sending], &woke);
    s_sending ^= 1;
    return woke == pdTRUE;
}

static void push_task(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    int k = 0;
    rect_t todo[BANDS];
    for (;;) {
        bool any = false;
        if (s_direct) {
            for (int b = 0; b < BANDS; b++) todo[b] = (rect_t){ 0, b * BAND, LCD_W - 1, b * BAND + BAND - 1 };
            any = true;
        } else {
            taskENTER_CRITICAL(&s_dirty_mux);
            for (int b = 0; b < BANDS; b++) {
                todo[b] = s_dirty[b];
                if (todo[b].x1 <= todo[b].x2) any = true;
                s_dirty[b] = (rect_t){ 1, 0, 0, 0 };
            }
            taskEXIT_CRITICAL(&s_dirty_mux);
        }
        for (int b = 0; b < BANDS && any; b++) {
            rect_t r = todo[b];
            if (r.x1 > r.x2) continue;
            // never more than the band: the DMA buffer holds one
            if (r.y1 < b * BAND) r.y1 = b * BAND;
            if (r.y2 > b * BAND + BAND - 1) r.y2 = b * BAND + BAND - 1;
            const int w = r.x2 - r.x1 + 1, h = r.y2 - r.y1 + 1;
            xSemaphoreTake(s_band_free[k], portMAX_DELAY);
            const int64_t t0 = esp_timer_get_time();
            uint16_t *dst = s_band[k];
            for (int y = 0; y < h; y++) {
                const uint16_t *src = s_src + (r.y1 + y) * LCD_W + r.x1;
                for (int x = 0; x < w; x++) *dst++ = (uint16_t)((src[x] >> 8) | (src[x] << 8));
            }
            const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
            st_fills++;
            if (us > st_max_fill_us) st_max_fill_us = us;
            if (esp_lcd_panel_draw_bitmap(s_panel, r.x1, r.y1, r.x2 + 1, r.y2 + 1, s_band[k]) != ESP_OK) {
                xSemaphoreGive(s_band_free[k]);
                st_late++;
            }
            k ^= 1;
        }
        if (any) st_frames++;
        vTaskDelayUntil(&last, pdMS_TO_TICKS(FRAME_MS));
    }
}

static esp_err_t lcd_init(void)
{
    s_src = heap_caps_calloc(LCD_W * LCD_H, 2, MALLOC_CAP_SPIRAM);
    for (int k = 0; k < 2; k++) {
        s_band[k] = heap_caps_malloc(BAND * LCD_W * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        s_band_free[k] = xSemaphoreCreateBinary();
        if (!s_band[k] || !s_band_free[k]) return ESP_ERR_NO_MEM;
        xSemaphoreGive(s_band_free[k]);
    }
    if (!s_src) return ESP_ERR_NO_MEM;
    const spi_bus_config_t bus = { .mosi_io_num = PIN_LCD_MOSI, .miso_io_num = -1, .sclk_io_num = PIN_LCD_SCK,
                                   .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = BAND * LCD_W * 2 };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");
    esp_lcd_panel_io_handle_t io;
    const esp_lcd_panel_io_spi_config_t ioc = { .cs_gpio_num = PIN_LCD_CS, .dc_gpio_num = PIN_LCD_DC, .spi_mode = 0,
                                                .pclk_hz = 40 * 1000 * 1000, .trans_queue_depth = 4,
                                                .on_color_trans_done = on_band_sent, .lcd_cmd_bits = 8, .lcd_param_bits = 8 };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &ioc, &io), TAG, "panel io");
    const esp_lcd_panel_dev_config_t pc = { .reset_gpio_num = PIN_LCD_RST, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
                                            .bits_per_pixel = 16 };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &pc, &s_panel), TAG, "st7789");
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);           // IPS modules show true colours inverted
    esp_lcd_panel_swap_xy(s_panel, true);                // the 240 x 320 module, landscape
    esp_lcd_panel_mirror(s_panel, false, true);
    esp_lcd_panel_disp_on_off(s_panel, true);
    mark(0, 0, LCD_W - 1, LCD_H - 1);                    // the first frame: all of it
    xTaskCreatePinnedToCore(push_task, "lcd_push", 3072, NULL, 4, NULL, 0);
    return ESP_OK;
}

uint16_t *fh1_board_framebuffer(int *w, int *h)
{
    *w = LCD_W;
    *h = LCD_H;
    return s_src;
}

void fh1_board_flush(int x1, int y1, int x2, int y2, const uint16_t *px)
{
    const int w = x2 - x1 + 1;
    for (int y = y1; y <= y2; y++) memcpy(s_src + y * LCD_W + x1, px + (y - y1) * w, w * 2);
    mark(x1, y1, x2, y2);            // after the copy: an area the push task is sending meanwhile goes again next frame
}

void fh1_board_changed(int y1, int y2) { mark(0, y1, LCD_W - 1, y2); }

void fh1_board_direct(bool on)
{
    s_direct = on;
    if (!on) mark(0, 0, LCD_W - 1, LCD_H - 1);       // the program's last picture, once more
}

void fh1_board_display_stats(fh1_display_stats_t *out, bool reset)
{
    memset(out, 0, sizeof(*out));
    out->frames = st_frames;
    out->fills = st_fills;
    out->late_fills = st_late;
    out->max_fill_us = st_max_fill_us;
    if (reset) st_frames = st_fills = st_late = st_max_fill_us = 0;
}

static esp_err_t disp_probe(fh1_device_t *d)
{
    strlcpy(d->info, "ST7789 320x240, SPI 40 MHz", sizeof(d->info));
    return s_panel ? ESP_OK : ESP_FAIL;
}
static const fh1_driver_t s_disp_drv = { .name = "st7789", .probe = disp_probe };

// ── Backlight (display.backlight): PWM on GPIO40 ──

static bool s_backlight_ready;
uint8_t fh1_board_backlight_level(void) { return fh1_settings_get_u8("display", "bl", 80); }

static void bl_apply(uint8_t pct)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (pct * 1023) / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void pwm_init(int pin, ledc_timer_t timer, ledc_channel_t ch, uint32_t hz)
{
    const ledc_timer_config_t t = { .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
                                    .timer_num = timer, .freq_hz = hz, .clk_cfg = LEDC_AUTO_CLK };
    const ledc_channel_config_t c = { .gpio_num = pin, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = ch,
                                      .timer_sel = timer, .duty = 0 };
    ledc_timer_config(&t);
    ledc_channel_config(&c);
}

static esp_err_t bl_probe(fh1_device_t *d)
{
    static bool done;
    if (!done) pwm_init(PIN_LCD_BL, LEDC_TIMER_0, LEDC_CHANNEL_0, 5000), done = true;
    strlcpy(d->info, "PWM on GPIO40", sizeof(d->info));
    return ESP_OK;
}
static esp_err_t bl_start(fh1_device_t *d) { s_backlight_ready = true; bl_apply(fh1_board_backlight_level()); return ESP_OK; }
static void bl_stop(fh1_device_t *d) { bl_apply(0); s_backlight_ready = false; }
void fh1_board_backlight_preview(uint8_t pct) { if (s_backlight_ready) bl_apply(pct > 100 ? 100 : pct); }
void fh1_board_backlight(uint8_t pct)
{
    fh1_settings_set_u8("display", "bl", pct > 100 ? 100 : pct);
    if (s_backlight_ready) bl_apply(pct > 100 ? 100 : pct);
}
static const fh1_driver_t s_bl_drv = { .name = "ledc", .probe = bl_probe, .start = bl_start, .stop = bl_stop };

// ── I2C (SDA 3, SCL 1): the codec, both expanders, the clock and the IMU ──

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_mcp1, s_mcp2;

enum { MCP_IODIRA = 0x00, MCP_IODIRB = 0x01, MCP_GPPUA = 0x0C, MCP_GPPUB = 0x0D, MCP_GPIOA = 0x12, MCP_OLATA = 0x14,
       MCP_OLATB = 0x15 };

static esp_err_t mcp_write(i2c_master_dev_handle_t d, uint8_t reg, uint8_t v)
{
    const uint8_t b[2] = { reg, v };
    return i2c_master_transmit(d, b, 2, 50);
}

static esp_err_t mcp_read2(i2c_master_dev_handle_t d, uint8_t reg, uint8_t out[2])
{
    return i2c_master_transmit_receive(d, &reg, 1, out, 2, 50);
}

static i2c_master_dev_handle_t i2c_dev(uint8_t addr)
{
    i2c_master_dev_handle_t h = NULL;
    if (i2c_master_probe(s_i2c, addr, 30) != ESP_OK) return NULL;
    const i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr, .scl_speed_hz = 400000 };
    return i2c_master_bus_add_device(s_i2c, &dc, &h) == ESP_OK ? h : NULL;
}

static void i2c_init(void)
{
    const i2c_master_bus_config_t bc = { .i2c_port = 0, .sda_io_num = PIN_I2C_SDA, .scl_io_num = PIN_I2C_SCL,
                                         .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
                                         .flags.enable_internal_pullup = true };
    if (i2c_new_master_bus(&bc, &s_i2c) != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus failed");
        return;
    }
    s_mcp1 = i2c_dev(0x20);
    s_mcp2 = i2c_dev(0x21);
    if (s_mcp1) {
        // GPA0-5 buttons, GPA6 card detect: inputs with pull-ups; GPA7 stick power, out, on. GPB0-3 the LED
        // cathodes, out, high = off.
        mcp_write(s_mcp1, MCP_OLATA, 0x80);
        mcp_write(s_mcp1, MCP_IODIRA, 0x7F);
        mcp_write(s_mcp1, MCP_GPPUA, 0x7F);
        mcp_write(s_mcp1, MCP_OLATB, 0x0F);
        mcp_write(s_mcp1, MCP_IODIRB, 0xF0);
        mcp_write(s_mcp1, MCP_GPPUB, 0xF0);
    }
    if (s_mcp2) {
        // GPA0 amp enable (on), GPA3 E28 NRESET (high), GPA7 E28 RX_EN, GPB7 E28 TX_EN: outputs. The rest inputs.
        mcp_write(s_mcp2, MCP_OLATA, 0x09);
        mcp_write(s_mcp2, MCP_IODIRA, 0x76);
        mcp_write(s_mcp2, MCP_GPPUA, 0x76);
        mcp_write(s_mcp2, MCP_OLATB, 0x00);
        mcp_write(s_mcp2, MCP_IODIRB, 0x7F);
        mcp_write(s_mcp2, MCP_GPPUB, 0x7F);
    }
    ESP_LOGI(TAG, "I2C: MCP23017 #1 %s, #2 %s", s_mcp1 ? "found" : "missing", s_mcp2 ? "found" : "missing");
}

// ── Buttons (input.buttons), stick (input.stick) and the button lights (light.buttons) ──
// Polled every 10 ms: the face buttons on MCP #1 GPA0-5 (low = pressed), Menu on GPIO0, the stick's two axes on
// the ADC as the D-pad. A button's light is on while it is held.

static adc_oneshot_unit_handle_t s_adc;
static adc_channel_t s_ch_x, s_ch_y;
static bool s_stick, s_lights_ready;

static void lights_apply(uint8_t pct)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, (pct * 1023) / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

static void keys_task(void *arg)
{
    static const fh1_key_t face[6] = { FH1_KEY_A, FH1_KEY_B, FH1_KEY_X, FH1_KEY_Y, FH1_KEY_START, FH1_KEY_SELECT };
    uint32_t held = 0;
    uint8_t leds = 0x0F;
    for (;;) {
        uint32_t now = 0;
        uint8_t ab[2];
        if (s_mcp1 && mcp_read2(s_mcp1, MCP_GPIOA, ab) == ESP_OK)
            for (int i = 0; i < 6; i++)
                if (!(ab[0] & (1u << i))) now |= 1u << face[i];
        if (!gpio_get_level(PIN_MENU)) now |= 1u << FH1_KEY_MENU;
        if (s_stick) {
            int x = 2048, y = 2048;
            adc_oneshot_read(s_adc, s_ch_x, &x);
            adc_oneshot_read(s_adc, s_ch_y, &y);
            // Pushed a third of the way or more, with the previous state kept between the two thresholds.
            const bool l = x < (held & (1u << FH1_KEY_LEFT) ? 1500 : 1100), r = x > (held & (1u << FH1_KEY_RIGHT) ? 2600 : 3000);
            const bool u = y < (held & (1u << FH1_KEY_UP) ? 1500 : 1100), d = y > (held & (1u << FH1_KEY_DOWN) ? 2600 : 3000);
            now |= (l << FH1_KEY_LEFT) | (r << FH1_KEY_RIGHT) | (u << FH1_KEY_UP) | (d << FH1_KEY_DOWN);
        }
        const uint32_t changed = now ^ held;
        for (int k = 0; k < FH1_KEY_COUNT; k++)
            if (changed & (1u << k)) fh1_input_post((fh1_key_t)k, (now >> k) & 1);
        held = now;
        // Lights: GPB0-3 are A, B, X, Y's cathodes, low = on.
        uint8_t want = 0x0F;
        if (s_lights_ready)
            for (int i = 0; i < 4; i++)
                if (now & (1u << face[i])) want &= ~(1u << i);
        if (s_mcp1 && want != leds && mcp_write(s_mcp1, MCP_OLATB, want) == ESP_OK) leds = want;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static esp_err_t buttons_probe(fh1_device_t *d)
{
    strlcpy(d->info, s_mcp1 ? "MCP23017 0x20, Menu on GPIO0" : "MCP23017 0x20 missing", sizeof(d->info));
    return s_mcp1 ? ESP_OK : ESP_ERR_NOT_FOUND;
}
static const fh1_driver_t s_buttons_drv = { .name = "mcp23017", .probe = buttons_probe };

static esp_err_t stick_probe(fh1_device_t *d)
{
    if (!s_adc) {
        const adc_oneshot_unit_init_cfg_t uc = { .unit_id = ADC_UNIT_1 };
        adc_unit_t unit;
        if (adc_oneshot_new_unit(&uc, &s_adc) != ESP_OK || adc_oneshot_io_to_channel(PIN_STICK_X, &unit, &s_ch_x) != ESP_OK ||
            adc_oneshot_io_to_channel(PIN_STICK_Y, &unit, &s_ch_y) != ESP_OK)
            return ESP_FAIL;
        const adc_oneshot_chan_cfg_t cc = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
        adc_oneshot_config_channel(s_adc, s_ch_x, &cc);
        adc_oneshot_config_channel(s_adc, s_ch_y, &cc);
    }
    strlcpy(d->info, "ADC GPIO2 (X), GPIO4 (Y)", sizeof(d->info));
    return ESP_OK;
}
static esp_err_t stick_start(fh1_device_t *d) { s_stick = true; return ESP_OK; }
static void stick_stop(fh1_device_t *d) { s_stick = false; }
static const fh1_driver_t s_stick_drv = { .name = "adc-stick", .probe = stick_probe, .start = stick_start, .stop = stick_stop };

static esp_err_t lights_probe(fh1_device_t *d)
{
    static bool done;
    if (!s_mcp1) return ESP_ERR_NOT_FOUND;
    if (!done) pwm_init(PIN_LED_PWM, LEDC_TIMER_1, LEDC_CHANNEL_1, 2000), done = true;
    strlcpy(d->info, "MCP23017 GPB0-3, brightness PWM on GPIO44", sizeof(d->info));
    return ESP_OK;
}
static esp_err_t lights_start(fh1_device_t *d)
{
    lights_apply(fh1_settings_get_u8("buttons", "light", 50));
    s_lights_ready = true;
    return ESP_OK;
}
static void lights_stop(fh1_device_t *d) { s_lights_ready = false; lights_apply(0); }
static const fh1_driver_t s_lights_drv = { .name = "mcp23017", .probe = lights_probe, .start = lights_start, .stop = lights_stop };

// ── Real-time clock (rtc): PCF8563 at 0x51, on the second board ──

static i2c_master_dev_handle_t s_rtc;
static int bcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0f); }

static esp_err_t rtc_probe(fh1_device_t *d)
{
    if (!s_i2c) return ESP_ERR_INVALID_STATE;
    if (!s_rtc) s_rtc = i2c_dev(0x51);
    if (!s_rtc) return ESP_ERR_NOT_FOUND;
    strlcpy(d->info, "PCF8563 at I2C 0x51", sizeof(d->info));
    return ESP_OK;
}

static esp_err_t rtc_start(fh1_device_t *d)
{
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
static const fh1_driver_t s_rtc_drv = { .name = "pcf8563", .probe = rtc_probe, .start = rtc_start };

// ── Devices ──

static fh1_device_t s_display = { .id = "display0", .cls = "display", .caps = FH1_CAP_DISPLAY,
                                  .drv = &s_disp_drv, .default_on = true, .essential = true, .ma = 20 };
static fh1_device_t s_backlight = { .id = "backlight0", .cls = "display.backlight", .caps = FH1_CAP_BACKLIGHT,
                                    .drv = &s_bl_drv, .default_on = true, .ma = 40 };
static fh1_device_t s_buttons = { .id = "buttons0", .cls = "input.buttons", .caps = FH1_CAP_BUTTONS,
                                  .drv = &s_buttons_drv, .default_on = true, .essential = true, .ma = 1 };
static fh1_device_t s_stick_dev = { .id = "stick0", .cls = "input.stick", .caps = FH1_CAP_STICK_ANALOG,
                                    .drv = &s_stick_drv, .default_on = true, .ma = 1 };
static fh1_device_t s_lights = { .id = "lights0", .cls = "light.buttons", .caps = FH1_CAP_BUTTON_LIGHTS,
                                 .drv = &s_lights_drv, .default_on = true, .ma = 8 };
static fh1_device_t s_rtc_dev = { .id = "rtc0", .cls = "rtc", .caps = FH1_CAP_RTC, .drv = &s_rtc_drv,
                                  .default_on = true, .ma = 1 };
// The ES8311's DAC through I2S (its I2C set-up still to come), the amplifier on through MCP #2.
static const fh1_pcm5102a_cfg_t s_audio_cfg = { .bck = PIN_I2S_BCK, .ws = PIN_I2S_WS, .dout = PIN_I2S_DOUT };
static fh1_device_t s_audio = { .id = "audio0", .cls = "audio.out", .caps = FH1_CAP_AUDIO_OUT,
                                .drv = &fh1_pcm5102a_driver, .cfg = &s_audio_cfg, .default_on = true, .ma = 30 };

esp_err_t fh1_board_init(void)
{
    gpio_config_t menu = { .pin_bit_mask = 1ULL << PIN_MENU, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&menu);
    i2c_init();
    const esp_err_t err = lcd_init();
    if (err != ESP_OK) ESP_LOGE(TAG, "display: %s", esp_err_to_name(err));
    xTaskCreatePinnedToCore(keys_task, "fh1_keys", 3072, NULL, 5, NULL, 0);
    fh1_dev_register(&s_display);
    fh1_dev_register(&s_backlight);
    fh1_dev_register(&s_buttons);
    fh1_dev_register(&s_stick_dev);
    fh1_dev_register(&s_lights);
    fh1_dev_register(&s_rtc_dev);
    fh1_dev_register(&s_audio);
    return err;
}
