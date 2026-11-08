// The ESP32-A1S Audio Kit's own hardware (built with -DBRIDGE_BOARD=A1S). Pins as the AI-Thinker Audio Kit
// (v2.2, ES8388) wires them, the layout ESP-ADF uses for this board:
//   codec I2C  SDA 33, SCL 32 (ES8388 at 0x10; the older AC101 module at 0x1A)
//   keys       REC 36, MODE 13, PLAY 19, SET 23, VOL- 18, VOL+ 5 (to ground when pressed; 36 has no pull-up)
//   LED        22 (green, on when low)
//   amplifier  enable 21; headphone detect 39 (low when plugged)
//   I2S        MCLK 0, BCK 27, WS 25, DOUT 26 (DIN 35, unused)
//   SD slot    1-bit SDMMC: CLK 14, CMD 15, D0 2; card detect 34 (low when a card is in)
//   header     IO0, 5, 18, 19, 21, 22, 23 (Signal-Decoder's esp32-cat-bridge README): the keys on 5/18/19/23
//              share them (an I2S copy there for an external PCM5102A was tried and dropped: it sounded crippled)
// The amplifier-enable and headphone-detect polarities are not confirmed for this board (the parent project
// guessed too): both are controls of the speaker, to set by ear.
// The codec plays what the FH1 sends (audio_rx.c): ES8388 as an I2S slave, set up as ESP-ADF does for this
// board; its outputs all on, the speaker amplifier only while playing without headphones. The SD card is
// read for its size (no file system yet). The line-in jack has no detect pin.
#include <string.h>
#include "board.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "a1s";

#define PIN_SDA 33
#define PIN_SCL 32
#define PIN_LED 22
#define PIN_PA 21
#define PIN_HP 39
#define PIN_SD_DET 34

static const struct {
    int pin;
    const char *name;
} s_keys[] = { { 36, "REC" }, { 13, "MODE" }, { 19, "PLAY" }, { 23, "SET" }, { 18, "VOL-" }, { 5, "VOL+" } };
#define NKEYS (int)(sizeof(s_keys) / sizeof(s_keys[0]))

static uint8_t s_codec_addr;           // 0: none found
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_codec;
static i2s_chan_handle_t s_tx;
static uint32_t s_rate;
static bool s_playing;
static uint8_t s_volume = 70;
static volatile uint32_t s_sd_mb;

// ── The codec's controls (shown in the speaker's view on the FH1; saved in NVS "a1s", "c<id>") ──
// Outputs: LOUT1/ROUT1 are the headphone jack, LOUT2/ROUT2 the speaker amplifier's input (the Audio Kit's
// wiring as ESP-ADF drives it).
enum { C_OUTPUT, C_LEVEL, C_DEEMPH, C_CLICKFREE, C_RAMP, C_MONO, C_WIDTH, C_PA, C_HP, C_COUNT };
enum { OUT_AUTO, OUT_SPEAKER, OUT_PHONES, OUT_BOTH };
static const fh1b_ctrl_t s_ctrl_def[C_COUNT] = {
    { BOARD_DEV_OUT, C_OUTPUT, FH1B_CTRL_CHOICE, 0, 3, OUT_AUTO, "Output", "Auto|Speaker|Headphones|Both" },
    { BOARD_DEV_OUT, C_LEVEL, FH1B_CTRL_RANGE, -30, 4, 0, "Output level", "dB" },
    { BOARD_DEV_OUT, C_DEEMPH, FH1B_CTRL_CHOICE, 0, 3, 0, "De-emphasis", "Off|32 kHz|44.1 kHz|48 kHz" },
    { BOARD_DEV_OUT, C_CLICKFREE, FH1B_CTRL_TOGGLE, 0, 1, 1, "Click-free", "" },
    { BOARD_DEV_OUT, C_RAMP, FH1B_CTRL_TOGGLE, 0, 1, 1, "Soft ramp", "" },
    { BOARD_DEV_OUT, C_MONO, FH1B_CTRL_TOGGLE, 0, 1, 0, "Mono", "" },
    { BOARD_DEV_OUT, C_WIDTH, FH1B_CTRL_RANGE, 0, 7, 0, "3D width", "" },
    { BOARD_DEV_OUT, C_PA, FH1B_CTRL_CHOICE, 0, 1, 0, "Amp enable", "Active high|Active low" },
    { BOARD_DEV_OUT, C_HP, FH1B_CTRL_CHOICE, 0, 1, 0, "Phones detect", "Low = in|High = in" },
};
static int16_t s_ctrl[C_COUNT];
static bool s_ctrl_loaded;

static void ctrl_load(void)
{
    if (s_ctrl_loaded) return;
    s_ctrl_loaded = true;
    for (int i = 0; i < C_COUNT; i++) s_ctrl[i] = s_ctrl_def[i].value;
    nvs_handle_t n;
    if (nvs_open("a1s", NVS_READONLY, &n) != ESP_OK) return;
    for (int i = 0; i < C_COUNT; i++) {
        char key[6];
        snprintf(key, sizeof(key), "c%d", i);
        int16_t v;
        if (nvs_get_i16(n, key, &v) == ESP_OK && v >= s_ctrl_def[i].min && v <= s_ctrl_def[i].max) s_ctrl[i] = v;
    }
    nvs_close(n);
}

static void codec_write(uint8_t reg, uint8_t val)
{
    const uint8_t b[2] = { reg, val };
    if (s_codec && i2c_master_transmit(s_codec, b, 2, 50) != ESP_OK) ESP_LOGW(TAG, "codec register 0x%02x not written", reg);
}

// The ES8388 as ESP-ADF sets it up for playback (es8388_init + start): I2S slave, 16-bit, MCLK 256 x fs, the
// DACs to the mixers, every output on.
static void codec_setup(void)
{
    codec_write(0x19, 0x04);           // DACCONTROL3: mute
    codec_write(0x01, 0x50);           // CONTROL2
    codec_write(0x02, 0x00);           // CHIPPOWER: all on
    codec_write(0x35, 0xA0);           // internal DLL off (better at low rates)
    codec_write(0x37, 0xD0);
    codec_write(0x39, 0xD0);
    codec_write(0x08, 0x00);           // MASTERMODE: slave
    codec_write(0x04, 0xC0);           // DACPOWER: off while setting up
    codec_write(0x00, 0x12);           // CONTROL1: play and record
    codec_write(0x17, 0x18);           // DACCONTROL1: 16-bit I2S
    codec_write(0x18, 0x02);           // DACCONTROL2: single speed, MCLK / LRCK 256
    codec_write(0x26, 0x00);           // DACCONTROL16
    codec_write(0x27, 0x90);           // DACCONTROL17: left DAC to left mixer, 0 dB
    codec_write(0x2A, 0x90);           // DACCONTROL20: right DAC to right mixer
    codec_write(0x2B, 0x80);           // DACCONTROL21: ADC and DAC share LRCK
    codec_write(0x2D, 0x00);           // DACCONTROL23
    codec_write(0x1A, 0x00);           // DACCONTROL4/5: digital volume 0 dB
    codec_write(0x1B, 0x00);
    codec_write(0x04, 0x3C);           // DACPOWER: DACs and LOUT1/ROUT1/LOUT2/ROUT2 on
    codec_write(0x02, 0x00);           // start
}

// The volume in the DACs (0.5 dB steps down from 0 dB), the outputs fixed at 0 dB: turning the analog outputs
// up and down instead kept the amplifier's hiss loud at every volume.
// The volume in the DACs (0.5 dB steps down from 0 dB); the analog outputs at the "Output level" control
// (1.5 dB steps, 0x1E = 0 dB): setting the analog gain by the volume kept the amplifier's hiss loud.
static void codec_volume(uint8_t pct)
{
    if (pct > 100) pct = 100;
    const int att = pct ? (100 - pct) * 100 / 100 : 192;    // half-dB steps: 100 % 0 dB, 50 % -25 dB, 0 muted
    codec_write(0x1A, (uint8_t)att);   // DACCONTROL4: left
    codec_write(0x1B, (uint8_t)att);   // DACCONTROL5: right
    int lvl = (s_ctrl[C_LEVEL] + 45) * 2 / 3;                // dB -> 1.5 dB steps from -45 dB
    lvl = lvl < 0 ? 0 : lvl > 0x21 ? 0x21 : lvl;
    for (uint8_t r = 0x2E; r <= 0x31; r++) codec_write(r, (uint8_t)lvl);   // LOUT1, ROUT1, LOUT2, ROUT2
}

// The speaker amplifier: on while playing, when the output includes the speaker (Auto: no headphones in).
static void amp_update(void)
{
    const int out = s_ctrl[C_OUTPUT];
    const bool speaker = out == OUT_SPEAKER || out == OUT_BOTH || (out == OUT_AUTO && !board_headphones());
    const bool on = s_playing && speaker;
    gpio_set_level(PIN_PA, s_ctrl[C_PA] ? !on : on);       // "Active low": the level inverted
}


// The outputs powered while playing (DACPOWER): the unused ones off (less noise).
static uint8_t dac_power(void)
{
    switch (s_ctrl[C_OUTPUT]) {
    case OUT_SPEAKER: return 0x0C;     // LOUT2, ROUT2
    case OUT_PHONES: return 0x30;      // LOUT1, ROUT1
    default: return 0x3C;              // all (Auto: the amplifier decides the speaker)
    }
}

static void codec_volume(uint8_t pct);

// The controls into the codec's registers (I2C: works whether playing or not).
static void ctrl_apply(void)
{
    ctrl_load();
    // DACCONTROL6: de-emphasis (bits 7-6), click-free (bit 3). DACCONTROL7: zero-cross (bits 7-6, with
    // click-free), mono (bit 5), 3D width (bits 4-2).
    codec_write(0x1C, (uint8_t)(s_ctrl[C_DEEMPH] << 6 | (s_ctrl[C_CLICKFREE] ? 0x08 : 0)));
    codec_write(0x1D, (uint8_t)((s_ctrl[C_CLICKFREE] ? 0xC0 : 0) | (s_ctrl[C_MONO] ? 0x20 : 0) | (s_ctrl[C_WIDTH] & 7) << 2));
    codec_write(0x19, (uint8_t)((s_ctrl[C_RAMP] ? 0x20 : 0) | (s_playing ? 0 : 0x04)));     // soft ramp; muted unless playing
    if (s_playing) {
        codec_write(0x04, dac_power());
        codec_volume(s_volume);        // the output level with it
    }
    amp_update();
}

int board_ctrl_count(uint8_t dev) { return dev == BOARD_DEV_OUT && s_codec ? C_COUNT : 0; }

bool board_ctrl_get(uint8_t dev, int i, fh1b_ctrl_t *out)
{
    if (dev != BOARD_DEV_OUT || i < 0 || i >= C_COUNT) return false;
    ctrl_load();
    *out = s_ctrl_def[i];
    out->value = s_ctrl[i];
    return true;
}

bool board_ctrl_set(uint8_t dev, uint8_t id, int16_t value)
{
    if (dev != BOARD_DEV_OUT || id >= C_COUNT) return false;
    ctrl_load();
    if (value < s_ctrl_def[id].min) value = s_ctrl_def[id].min;
    if (value > s_ctrl_def[id].max) value = s_ctrl_def[id].max;
    s_ctrl[id] = value;
    ctrl_apply();
    nvs_handle_t n;
    if (nvs_open("a1s", NVS_READWRITE, &n) == ESP_OK) {
        char key[6];
        snprintf(key, sizeof(key), "c%d", id);
        nvs_set_i16(n, key, value);
        nvs_commit(n);
        nvs_close(n);
    }
    ESP_LOGI(TAG, "control %s = %d", s_ctrl_def[id].name, value);
    return true;
}

void board_init(void)
{
    i2c_master_bus_config_t bc = { .i2c_port = 0, .sda_io_num = PIN_SDA, .scl_io_num = PIN_SCL,
                                   .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
                                   .flags.enable_internal_pullup = true };
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bc, &bus) == ESP_OK) {
        s_bus = bus;
        char found[64] = "";
        int n = 0;
        for (int a = 0x08; a < 0x78; a++) {
            if (i2c_master_probe(bus, a, 20) == ESP_OK) {
                n += snprintf(found + n, sizeof(found) - n, " 0x%02x", a);
                if (a == 0x10 || a == 0x1A) s_codec_addr = (uint8_t)a;
            }
        }
        ESP_LOGI(TAG, "I2C devices:%s", n ? found : " none");
        ESP_LOGI(TAG, "codec: %s", s_codec_addr == 0x10 ? "ES8388" : s_codec_addr == 0x1A ? "AC101" : "not found");
        if (s_codec_addr == 0x10) {       // the AC101 is not driven (not on this board)
            i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x10, .scl_speed_hz = 100000 };
            i2c_master_bus_add_device(bus, &dc, &s_codec);
        }
    }
    for (int i = 0; i < NKEYS; i++) {
        gpio_config_t c = { .pin_bit_mask = 1ULL << s_keys[i].pin, .mode = GPIO_MODE_INPUT,
                            .pull_up_en = s_keys[i].pin < 34 ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE };
        gpio_config(&c);
    }
    gpio_config_t hp = { .pin_bit_mask = 1ULL << PIN_HP, .mode = GPIO_MODE_INPUT };
    gpio_config(&hp);
    gpio_config_t out = { .pin_bit_mask = (1ULL << PIN_LED) | (1ULL << PIN_PA), .mode = GPIO_MODE_OUTPUT };
    gpio_config(&out);
    gpio_set_level(PIN_LED, 1);        // off
    ctrl_load();
    amp_update();                      // amplifier off until there is sound (by its polarity)
    char keys[64] = "";
    int n = 0;
    for (int i = 0; i < NKEYS; i++) n += snprintf(keys + n, sizeof(keys) - n, " %s=%d", s_keys[i].name, gpio_get_level(s_keys[i].pin));
    ESP_LOGI(TAG, "keys (1 up):%s; headphones %s", keys, gpio_get_level(PIN_HP) ? "out" : "in");
    void sd_task(void *arg);
    xTaskCreatePinnedToCore(sd_task, "a1s_sd", 3072, NULL, 2, NULL, 0);
}

// ── Sound out ──

bool board_audio_start(uint32_t rate)
{
    if (!s_codec) return false;
    if (!s_tx) {
        i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
        cc.dma_desc_num = 6;
        cc.dma_frame_num = 240;
        cc.auto_clear = true;          // silence, not a loop of the last buffer, when nothing comes
        if (i2s_new_channel(&cc, &s_tx, NULL) != ESP_OK) return false;
        i2s_std_config_t sc = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
            .gpio_cfg = { .mclk = GPIO_NUM_0, .bclk = GPIO_NUM_27, .ws = GPIO_NUM_25, .dout = GPIO_NUM_26, .din = I2S_GPIO_UNUSED },
        };
        sc.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
        sc.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
        if (i2s_channel_init_std_mode(s_tx, &sc) != ESP_OK) {
            i2s_del_channel(s_tx);
            s_tx = NULL;
            return false;
        }
        codec_setup();
        ctrl_load();
        s_rate = rate;
    } else if (rate != s_rate) {
        if (s_playing) i2s_channel_disable(s_tx);
        s_playing = false;
        i2s_std_clk_config_t ck = I2S_STD_CLK_DEFAULT_CONFIG(rate);
        ck.clk_src = I2S_CLK_SRC_APLL;
        ck.mclk_multiple = I2S_MCLK_MULTIPLE_256;
        i2s_channel_reconfig_std_clock(s_tx, &ck);
        s_rate = rate;
    }
    if (!s_playing) {
        i2s_channel_enable(s_tx);      // the clocks first, then the outputs
        s_playing = true;
        codec_write(0x02, 0x00);       // CHIPPOWER: on
        ctrl_apply();                  // outputs as chosen, level, filters; unmuted (playing)
        ESP_LOGI(TAG, "playing at %lu Hz%s", (unsigned long)rate, board_headphones() ? " (headphones)" : "");
    }
    amp_update();
    return true;
}

void board_audio_stop(void)
{
    if (!s_playing) return;
    codec_write(0x19, (uint8_t)((s_ctrl[C_RAMP] ? 0x20 : 0) | 0x04));   // mute
    s_playing = false;
    amp_update();                      // amplifier off first
    codec_write(0x04, 0xC0);           // DACPOWER: DACs and outputs off, then the clocks (a codec left powered
    codec_write(0x02, 0xF3);           // CHIPPOWER: analog and DAC paths down   without its clock can hiss)
    i2s_channel_disable(s_tx);
    ESP_LOGI(TAG, "stopped");
}

// Stereo frames in, as they are.
int board_audio_write(const int16_t *lr, size_t frames)
{
    if (!s_playing) return -1;
    size_t w = 0;
    i2s_channel_write(s_tx, lr, frames * 4, &w, pdMS_TO_TICKS(200));
    amp_update();                      // headphones in or out meanwhile
    return (int)(w / 4);
}

void board_audio_volume(uint8_t pct)
{
    if (pct == s_volume) return;
    s_volume = pct;
    if (s_playing) codec_volume(pct);
}

// ── SD card ──

bool board_has_sd(void) { return true; }
uint32_t board_sd_mb(void) { return s_sd_mb; }

// Every 2 s: a card in (detect pin low) is read once for its size; out, 0.
void sd_task(void *arg)
{
    gpio_config_t det = { .pin_bit_mask = 1ULL << PIN_SD_DET, .mode = GPIO_MODE_INPUT };
    gpio_config(&det);
    bool was_in = false, host = false;
    for (;;) {
        const bool in = !gpio_get_level(PIN_SD_DET);
        if (in && !was_in) {
            vTaskDelay(pdMS_TO_TICKS(300));        // settled in the slot
            if (!host) host = sdmmc_host_init() == ESP_OK;
            sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
            slot.width = 1;
            slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
            sdmmc_host_t h = SDMMC_HOST_DEFAULT();
            h.flags = SDMMC_HOST_FLAG_1BIT;
            sdmmc_card_t card;
            esp_err_t err = host ? sdmmc_host_init_slot(h.slot, &slot) : ESP_FAIL;
            if (err == ESP_OK) err = sdmmc_card_init(&h, &card);
            if (err == ESP_OK) {
                s_sd_mb = (uint32_t)((uint64_t)card.csd.capacity * card.csd.sector_size / (1024 * 1024));
                ESP_LOGI(TAG, "SD card: %s, %lu MB", card.cid.name, (unsigned long)s_sd_mb);
            } else {
                s_sd_mb = 0;
                ESP_LOGW(TAG, "SD card in, not readable (%s)", esp_err_to_name(err));
            }
        } else if (!in && was_in) {
            s_sd_mb = 0;
            ESP_LOGI(TAG, "SD card out");
        }
        was_in = in;
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

uint32_t board_keys(void)
{
    uint32_t k = 0;
    for (int i = 0; i < NKEYS; i++) {
        if (!gpio_get_level(s_keys[i].pin)) k |= 1u << i;
    }
    return k;
}

int board_key_count(void) { return NKEYS; }
const char *board_key_name(int i) { return i >= 0 && i < NKEYS ? s_keys[i].name : "?"; }
bool board_headphones(void) { return s_ctrl[C_HP] ? gpio_get_level(PIN_HP) : !gpio_get_level(PIN_HP); }
void board_led(bool on) { gpio_set_level(PIN_LED, !on); }
uint8_t board_codec(void) { return s_codec_addr; }
