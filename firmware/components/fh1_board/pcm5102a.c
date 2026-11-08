// PCM5102A: I2S standard (Philips) mode, 16-bit stereo, no MCLK (the chip derives its clock from BCK).
// write() takes mono frames, write_stereo() L/R pairs; the volume and the channel mode (stereo, mono, left or
// right only) are applied on the way out. One writer at a time (a lock). Stop = I2S off.
#include <string.h>
#include <stdio.h>
#include "fh1_pcm5102a.h"
#include "fh1_audio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "pcm5102a";
#define DEFAULT_RATE 32000
#define CHUNK 256                    // frames converted per I2S write

typedef struct {
    i2s_chan_handle_t tx;
    uint32_t rate;
    int32_t gain;                    // Q8: 256 = full scale
    int channels;                    // fh1_audio_channels_t
    SemaphoreHandle_t lock;          // one writer at a time (a game, a test tone)
} pcm_t;

static pcm_t s_pcm;                  // one DAC

// Health, logged every 5 s while sound plays: frames written per second (should match the rate), frames
// dropped (the DMA stayed full for 20 ms), and underruns (every DMA buffer played out with nothing new:
// a gap you would hear).
static volatile uint32_t s_underruns;
static uint32_t s_written, s_dropped, s_last_underruns;
static int32_t s_peak;                   // the loudest sample sent in this window, after the volume
static int64_t s_window_start, s_last_write;

static bool IRAM_ATTR on_underrun(i2s_chan_handle_t h, i2s_event_data_t *e, void *u)
{
    s_underruns++;
    return false;
}

static void health(pcm_t *p)
{
    int64_t now = esp_timer_get_time();
    if (!s_window_start) s_window_start = now;
    if (now - s_window_start < 5000000) return;
    // Integers only: this runs on the writer's task, whose stack may be small (no float formatting).
    uint32_t ms = (uint32_t)((now - s_window_start) / 1000);
    uint32_t u = s_underruns;
    ESP_LOGI(TAG, "%lu frames/s (rate %u), %lu dropped, %lu underruns in %lu ms, peak %ld%% of full scale",
             (unsigned long)((uint64_t)s_written * 1000 / ms), (unsigned)p->rate, (unsigned long)s_dropped,
             (unsigned long)(u - s_last_underruns), (unsigned long)ms, (long)(s_peak * 100 / 32767));
    s_peak = 0;
    s_written = s_dropped = 0;
    s_last_underruns = u;
    s_window_start = now;
}

static esp_err_t pcm_probe(fh1_device_t *d)
{
    const fh1_pcm5102a_cfg_t *c = d->cfg;
    snprintf(d->info, sizeof(d->info), "I2S BCK %d, LCK %d, DIN %d (not detectable)", c->bck, c->ws, c->dout);
    return ESP_OK;
}

static void pcm_volume_set(uint8_t pct)
{
    if (pct > 100) pct = 100;
    s_pcm.gain = (int32_t)pct * pct * 256 / 10000;     // roughly even steps to the ear
}

static esp_err_t pcm_start(fh1_device_t *d)
{
    const fh1_pcm5102a_cfg_t *c = d->cfg;
    pcm_t *p = &s_pcm;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = 6;
    cc.dma_frame_num = 240;          // 6 x 240 frames: ~45 ms at 32 kHz, enough to ride out a late frame
    cc.auto_clear = true;            // silence, not a stuck tone, when the app stops writing
    esp_err_t err = i2s_new_channel(&cc, &p->tx, NULL);
    if (err != ESP_OK) return err;
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(DEFAULT_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = c->bck, .ws = c->ws, .dout = c->dout, .din = I2S_GPIO_UNUSED },
    };
    err = i2s_channel_init_std_mode(p->tx, &sc);
    const i2s_event_callbacks_t cbs = { .on_send_q_ovf = on_underrun };
    if (err == ESP_OK) err = i2s_channel_register_event_callback(p->tx, &cbs, NULL);
    if (err == ESP_OK) err = i2s_channel_enable(p->tx);
    if (err != ESP_OK) {
        i2s_del_channel(p->tx);
        p->tx = NULL;
        return err;
    }
    p->rate = DEFAULT_RATE;
    pcm_volume_set(fh1_audio_master_volume());
    p->channels = fh1_audio_channels();
    if (!p->lock) p->lock = xSemaphoreCreateMutex();
    d->priv = p;
    ESP_LOGI(TAG, "on: %u Hz, 16-bit stereo", (unsigned)p->rate);
    return ESP_OK;
}

static void pcm_stop(fh1_device_t *d)
{
    pcm_t *p = &s_pcm;
    if (!p->tx) return;
    bool locked = p->lock && xSemaphoreTake(p->lock, pdMS_TO_TICKS(200)) == pdTRUE;   // no write in flight
    d->priv = NULL;
    i2s_channel_disable(p->tx);
    i2s_del_channel(p->tx);
    p->tx = NULL;
    if (locked) xSemaphoreGive(p->lock);
}

// Frames out, mono (lr == NULL: frames[]) or stereo (lr: L, R pairs), with volume and channel mode.
static int pcm_out(fh1_device_t *d, const int16_t *frames, const int16_t *lr, size_t count, uint32_t rate)
{
    pcm_t *p = d->priv;
    if (!p || !p->tx || !p->lock) return FH1_UNAVAILABLE;
    if (xSemaphoreTake(p->lock, pdMS_TO_TICKS(100)) != pdTRUE) return 0;
    if (rate && rate != p->rate) {
        i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
        i2s_channel_disable(p->tx);
        if (i2s_channel_reconfig_std_clock(p->tx, &clk) == ESP_OK) p->rate = rate;
        i2s_channel_enable(p->tx);
    }
    int16_t buf[CHUNK * 2];
    size_t done = 0;
    const int mode = p->channels;
    // After a pause (nothing written for 100 ms) a new stream starts: the queue running empty while idle is
    // not a gap, so the counts start over.
    const int64_t now = esp_timer_get_time();
    if (now - s_last_write > 100000) {
        s_written = s_dropped = 0;
        s_peak = 0;
        s_last_underruns = s_underruns;
        s_window_start = now;
    }
    s_last_write = now;
    while (done < count) {
        size_t n = count - done < CHUNK ? count - done : CHUNK;
        for (size_t i = 0; i < n; i++) {
            int32_t l, r;
            if (lr) {
                l = lr[2 * (done + i)];
                r = lr[2 * (done + i) + 1];
            } else {
                l = r = frames[done + i];
            }
            if (mode != FH1_AUDIO_STEREO) {
                int32_t m = (l + r) / 2;
                l = mode == FH1_AUDIO_RIGHT ? 0 : m;
                r = mode == FH1_AUDIO_LEFT ? 0 : m;
            }
            l = (l * p->gain) >> 8;
            r = (r * p->gain) >> 8;
            int32_t al = l < 0 ? -l : l, ar = r < 0 ? -r : r;
            if (al > s_peak) s_peak = al;
            if (ar > s_peak) s_peak = ar;
            buf[2 * i] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
            buf[2 * i + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
        }
        size_t written = 0;
        // A short wait at most: a game keeps its own time and must not stall on a full buffer.
        i2s_channel_write(p->tx, buf, n * 4, &written, 20);
        done += written / 4;
        if (written < n * 4) break;
    }
    s_written += done;
    s_dropped += count - done;
    health(p);
    xSemaphoreGive(p->lock);
    return (int)done;
}

static int pcm_write(fh1_device_t *d, const int16_t *frames, size_t count, uint32_t rate)
{
    return pcm_out(d, frames, NULL, count, rate);
}

static int pcm_write_stereo(fh1_device_t *d, const int16_t *lr, size_t count, uint32_t rate)
{
    return pcm_out(d, NULL, lr, count, rate);
}

static void pcm_channels(fh1_device_t *d, int mode) { s_pcm.channels = mode; }

static void pcm_volume(fh1_device_t *d, uint8_t pct) { pcm_volume_set(pct); }

static const fh1_audio_ops_t s_ops = { .write = pcm_write, .set_volume = pcm_volume, .write_stereo = pcm_write_stereo,
                                        .set_channels = pcm_channels };
const fh1_driver_t fh1_pcm5102a_driver = { .name = "pcm5102a", .probe = pcm_probe, .start = pcm_start,
                                           .stop = pcm_stop, .ops = &s_ops };
