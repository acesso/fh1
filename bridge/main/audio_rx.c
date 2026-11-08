// Sound from the FH1 to the board's codec (fh1_bridge_proto.h, FH1B_UDP_AUDIO), for any board with one
// (board.h). The UDP task hands each packet here: it is decoded (16-bit PCM stereo or mono, or IMA ADPCM)
// into a ring of stereo frames, silence standing in for lost packets (by the sequence). A task of its own
// plays the ring: it waits for PRIME_MS of sound before starting, plays silence through a gap (counted as an
// underrun), and stops the codec after IDLE_MS without packets. The ring is in PSRAM when the board has it
// (the A1S: ~680 ms at 48 kHz), else smaller in internal RAM.
#include <string.h>
#include "audio_rx.h"
#include "board.h"
#include "fh1_adpcm.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio_rx";

#define RING_PSRAM 32768               // frames: ~680 ms at 48 kHz
#define RING_INTERNAL 4096             // frames: ~170 ms at 24 kHz (no PSRAM)
#define PRIME_MS 200
#define IDLE_MS 1000
#define CHUNK 240                      // frames a write

static int16_t *s_ring;                // L, R pairs
static uint32_t s_ring_frames;
static volatile uint32_t s_head, s_tail;          // frames written / played (free-running)
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_rate, s_last_ms;
static volatile uint8_t s_volume = 70;
static uint16_t s_seq;
static bool s_have_seq;
static volatile bool s_playing;
static uint32_t s_packets, s_lost, s_underruns, s_late_drops;
static uint32_t s_frames_in, s_frames_out;     // this report window: arrived, and played
static uint8_t s_codec_seen;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// Stereo frames in (NULL: silence).
static void ring_put(const int16_t *lr, size_t n)
{
    portENTER_CRITICAL(&s_mux);
    for (size_t i = 0; i < n; i++) {
        if (s_head - s_tail >= s_ring_frames) {     // full: drop the oldest (the FH1 is ahead)
            s_tail++;
            s_late_drops++;
        }
        const uint32_t k = (s_head++ % s_ring_frames) * 2;
        s_ring[k] = lr ? lr[2 * i] : 0;
        s_ring[k + 1] = lr ? lr[2 * i + 1] : 0;
    }
    portEXIT_CRITICAL(&s_mux);
}

static size_t ring_get(int16_t *out, size_t n)
{
    portENTER_CRITICAL(&s_mux);
    size_t k = s_head - s_tail;
    if (k > n) k = n;
    for (size_t i = 0; i < k; i++) {
        const uint32_t j = (s_tail++ % s_ring_frames) * 2;
        out[2 * i] = s_ring[j];
        out[2 * i + 1] = s_ring[j + 1];
    }
    portEXIT_CRITICAL(&s_mux);
    return k;
}

static size_t ring_level(void) { return s_head - s_tail; }

void audio_rx_packet(const uint8_t *buf, size_t len)
{
    if (!s_ring || len < sizeof(fh1b_audio_hdr_t)) return;
    fh1b_audio_hdr_t h;
    memcpy(&h, buf, sizeof(h));
    size_t n = h.samples;              // frames
    size_t bytes;
    switch (h.codec) {
    case FH1B_AUDIO_PCM16_STEREO: bytes = n * 4; break;
    case FH1B_AUDIO_PCM16: bytes = n * 2; break;
    case FH1B_AUDIO_IMA_ADPCM: n &= ~1u; bytes = n / 2; break;
    default: return;
    }
    if (n > FH1B_AUDIO_SAMPLES || sizeof(h) + bytes > len || h.rate < 8000 || h.rate > 48000) return;
    if (h.rate != s_rate) {            // a new rate: start over
        portENTER_CRITICAL(&s_mux);
        s_head = s_tail = 0;
        portEXIT_CRITICAL(&s_mux);
        s_rate = h.rate;
        s_have_seq = false;
    }
    if (h.codec != s_codec_seen) {
        s_codec_seen = h.codec;
        ESP_LOGI(TAG, "stream: %s, %u Hz", h.codec == FH1B_AUDIO_PCM16_STEREO ? "PCM16 stereo" : h.codec == FH1B_AUDIO_PCM16 ? "PCM16 mono" : "ADPCM mono",
                 h.rate);
    }
    // Lost packets (up to four): their time as silence, so what follows plays when it should.
    if (s_have_seq) {
        const uint16_t gap = (uint16_t)(h.seq - s_seq - 1);
        if (gap && gap <= 4) {
            s_lost += gap;
            ring_put(NULL, (size_t)gap * n);
        }
    }
    s_seq = h.seq;
    s_have_seq = true;
    static int16_t lr[2 * FH1B_AUDIO_SAMPLES];
    const uint8_t *p = buf + sizeof(h);
    if (h.codec == FH1B_AUDIO_PCM16_STEREO) {
        memcpy(lr, p, n * 4);
    } else {
        static int16_t mono[FH1B_AUDIO_SAMPLES];
        if (h.codec == FH1B_AUDIO_PCM16) {
            memcpy(mono, p, n * 2);
        } else {
            fh1_adpcm_t st = { .predictor = h.predictor, .index = h.index > 88 ? 88 : h.index };
            fh1_adpcm_decode(&st, p, n, mono);
        }
        for (size_t i = 0; i < n; i++) lr[2 * i] = lr[2 * i + 1] = mono[i];
    }
    ring_put(lr, n);
    s_frames_in += n;
    s_volume = h.volume;
    s_last_ms = now_ms();
    s_packets++;
}

bool audio_rx_playing(void) { return s_playing; }

static void play_task(void *arg)
{
    static int16_t buf[2 * CHUNK];
    uint32_t report = now_ms();
    for (;;) {
        const uint32_t t = now_ms();
        if (t - report >= 5000) {
            if (s_playing || s_packets)
                ESP_LOGI(TAG, "%lu packets, %lu lost, %lu underruns, %lu dropped (late), buffered %u ms; frames/s in %lu, played %lu",
                         (unsigned long)s_packets, (unsigned long)s_lost, (unsigned long)s_underruns, (unsigned long)s_late_drops,
                         s_rate ? (unsigned)(ring_level() * 1000 / s_rate) : 0, (unsigned long)(s_frames_in / 5),
                         (unsigned long)(s_frames_out / 5));
            s_packets = s_lost = s_underruns = s_late_drops = s_frames_in = s_frames_out = 0;
            report = t;
        }
        if (!s_playing) {
            // Start once PRIME_MS of sound is in.
            if (s_rate && ring_level() >= s_rate * PRIME_MS / 1000 && t - s_last_ms < IDLE_MS) {
                s_playing = board_audio_start(s_rate);
                if (!s_playing) ESP_LOGW(TAG, "the codec did not start");
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (t - s_last_ms > IDLE_MS) {         // the FH1 stopped sending
            board_audio_stop();
            s_playing = false;
            portENTER_CRITICAL(&s_mux);
            s_head = s_tail = 0;
            portEXIT_CRITICAL(&s_mux);
            s_have_seq = false;
            s_codec_seen = 0;
            continue;
        }
        board_audio_volume(s_volume);
        size_t k = ring_get(buf, CHUNK);
        if (k < CHUNK) {                        // ran dry: pad with silence
            s_underruns++;
            memset(buf + 2 * k, 0, (CHUNK - k) * 4);
        }
        s_frames_out += (uint32_t)board_audio_write(buf, CHUNK);   // blocks: the codec's clock paces this
    }
}

void audio_rx_init(void)
{
    if (s_ring) return;
    s_ring_frames = RING_PSRAM;
    s_ring = heap_caps_malloc(s_ring_frames * 4, MALLOC_CAP_SPIRAM);
    if (!s_ring) {
        s_ring_frames = RING_INTERNAL;
        s_ring = heap_caps_malloc(s_ring_frames * 4, MALLOC_CAP_8BIT);
    }
    if (!s_ring) {
        ESP_LOGE(TAG, "no memory for the sound buffer");
        return;
    }
    ESP_LOGI(TAG, "sound buffer: %lu frames in %s", (unsigned long)s_ring_frames, s_ring_frames == RING_PSRAM ? "PSRAM" : "internal RAM");
    xTaskCreatePinnedToCore(play_task, "audio_rx", 3072, NULL, 6, NULL, 1);
}
