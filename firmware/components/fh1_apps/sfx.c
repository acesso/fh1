// Sound effects for packaged apps (SDK 0.7): up to 8 sounds at once, mixed at 44.1 kHz by a task of its own on
// core 0 and played through fh1_audio. The samples are the app's (in its memory); the mixer only reads them, so
// every voice is stopped before the app is unloaded (sfx_shutdown). Its stack is 6 KB: the audio driver logs
// (printf) from inside fh1_audio_write and fh1_audio_open, and 3 KB overflowed.
#include <string.h>
#include "fh1_sdk.h"
#include "apps_priv.h"
#include "fh1_audio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define VOICES 8
#define RATE 44100
#define BLOCK 256                      // frames a pass: 5.8 ms

typedef struct {
    const int16_t *pcm;
    uint32_t frames;
    uint64_t pos;                      // 32.16 frames (16.16 in 32 bits wrapped after 65 536 frames, 1.5 s: a longer
                                       // sound replayed its start forever)
    uint32_t step;                     // 16.16: rate / RATE
    uint16_t volume;                   // 0..256
    uint32_t serial;                   // changes when the voice is reused (a pass's result is dropped then)
    bool on;
} voice_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static voice_t s_v[VOICES];
static uint32_t s_serial;
static TaskHandle_t s_task;
static volatile bool s_quit;

static void mixer(void *arg)
{
    static int32_t acc[BLOCK];
    static int16_t out[BLOCK];
    fh1_audio_t *audio = NULL;
    int idle = 0;
    while (!s_quit) {
        voice_t v[VOICES];
        portENTER_CRITICAL(&s_mux);
        memcpy(v, s_v, sizeof(v));
        portEXIT_CRITICAL(&s_mux);
        memset(acc, 0, sizeof(acc));
        bool any = false;
        for (int k = 0; k < VOICES; k++) {
            if (!v[k].on) continue;
            any = true;
            uint64_t pos = v[k].pos;
            for (int i = 0; i < BLOCK; i++) {
                const uint64_t f = pos >> 16;
                if (f >= v[k].frames) {
                    v[k].on = false;
                    break;
                }
                acc[i] += (v[k].pcm[f] * (int32_t)v[k].volume) >> 8;
                pos += v[k].step;
            }
            v[k].pos = pos;
        }
        portENTER_CRITICAL(&s_mux);
        for (int k = 0; k < VOICES; k++)
            if (s_v[k].serial == v[k].serial && s_v[k].on) {   // not stopped or reused meanwhile
                s_v[k].pos = v[k].pos;
                s_v[k].on = v[k].on;
            }
        portEXIT_CRITICAL(&s_mux);
        if (!any) {
            if (audio && ++idle > 200) {                       // a second of silence: the output goes back
                fh1_audio_close(audio);
                audio = NULL;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        idle = 0;
        // Overlapping sounds: a soft limit above 24000 (a quarter of the excess), then a clamp.
        for (int i = 0; i < BLOCK; i++) {
            int32_t a = acc[i];
            if (a > 24000) a = 24000 + (a - 24000) / 4;
            else if (a < -24000) a = -24000 + (a + 24000) / 4;
            out[i] = (int16_t)(a > 32767 ? 32767 : a < -32768 ? -32768 : a);
        }
        if (!audio) audio = fh1_audio_open(RATE);
        // The device paces the writes; without one (the null driver) the pass is paced here.
        if (fh1_audio_write(audio, out, BLOCK) != BLOCK) vTaskDelay(pdMS_TO_TICKS(BLOCK * 1000 / RATE));
    }
    if (audio) fh1_audio_close(audio);
    s_task = NULL;
    vTaskDelete(NULL);
}

int fh1_sfx_play(const int16_t *pcm, uint32_t frames, uint32_t rate, uint8_t volume)
{
    if (!pcm || !frames || !rate) return -1;
    if (!s_task) {
        s_quit = false;
        if (xTaskCreatePinnedToCore(mixer, "sfx", 6144, NULL, 5, &s_task, 0) != pdPASS) {
            s_task = NULL;
            return -1;
        }
    }
    portENTER_CRITICAL(&s_mux);
    int k = -1;
    uint64_t oldest = 0;
    for (int i = 0; i < VOICES && k < 0; i++)
        if (!s_v[i].on) k = i;
    if (k < 0)                                              // all busy: the one furthest along goes
        for (int i = 0; i < VOICES; i++)
            if (s_v[i].pos >= oldest) {
                oldest = s_v[i].pos;
                k = i;
            }
    s_v[k] = (voice_t){ .pcm = pcm, .frames = frames, .pos = 0, .step = (uint32_t)(((uint64_t)rate << 16) / RATE),
                        .volume = (uint16_t)(volume + (volume >> 7)), .serial = ++s_serial, .on = true };
    portEXIT_CRITICAL(&s_mux);
    return k;
}

void fh1_sfx_stop(int voice)
{
    if (voice < 0 || voice >= VOICES) return;
    portENTER_CRITICAL(&s_mux);
    s_v[voice].on = false;
    s_v[voice].serial = ++s_serial;
    portEXIT_CRITICAL(&s_mux);
}

void fh1_sfx_stop_all(void)
{
    for (int k = 0; k < VOICES; k++) fh1_sfx_stop(k);
}

bool fh1_sfx_playing(int voice)
{
    if (voice < 0 || voice >= VOICES) return false;
    portENTER_CRITICAL(&s_mux);
    const bool on = s_v[voice].on;
    portEXIT_CRITICAL(&s_mux);
    return on;
}

void sfx_shutdown(void)
{
    fh1_sfx_stop_all();
    if (!s_task) return;
    s_quit = true;
    // A pass reads the app's samples for at most a block: wait for the task to end (up to 100 ms).
    for (int i = 0; i < 20 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(5));
}
