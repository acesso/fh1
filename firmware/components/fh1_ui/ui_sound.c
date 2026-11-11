// System sounds: a short sound for moving, choosing, going back, an alert and a notice, in the voice of the theme.
// Made here, as 16-bit samples at 22050 Hz, the first time a set is needed (no files); played by whoever registered
// fh1_ui_set_sound_fn (fh1_apps: its mixer, beside the apps' own sounds). Off unless the "ui"/"sounds" setting is on.
//
// Voices: "soft" (Poke Gen1, Poke Gen2), gentle pulse-wave blips like a handheld's; "alarm" (Hex), crisp square-wave ticks
// and a two-tone warning, as an early-warning console sounds; "garden", wooden marimba notes, birdsong for a notice
// and a rustle of leaves as a window opens; "command" (Command), clicks and sweeps, a warbling alert, a two-ping
// chime and, as a window builds itself, a clack, a servo's whine and a clack. Only the last two have an opening
// sound.
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "ui_priv.h"
#include "fh1_settings.h"
#include "esp_heap_caps.h"

#define RATE 22050

typedef struct { int16_t *pcm; uint32_t frames; } snd_t;
static snd_t s_set[UI_VOICES][UI_SND_COUNT];
static fh1_ui_sound_fn s_play;
static int s_on = -1;                  // -1: not read from the settings yet

// One tone into buf: frequency f (Hz), ms long, wave: 0 square, 1 pulse 25%, 2 sine; amplitude 0..1, with a 2 ms
// attack and a release over the last `rel` ms (no clicks at the ends).
static uint32_t tone(int16_t *buf, float f, int ms, int wave, float amp, int rel)
{
    const uint32_t n = (uint32_t)(RATE * ms / 1000), att = RATE * 2 / 1000, rl = (uint32_t)(RATE * rel / 1000);
    for (uint32_t i = 0; i < n; i++) {
        const float ph = fmodf((float)i * f / RATE, 1.0f);
        float v = wave == 2 ? sinf(ph * 6.2831853f) : (ph < (wave == 1 ? 0.25f : 0.5f) ? 1.0f : -1.0f);
        float env = 1.0f;
        if (i < att) env = (float)i / att;
        if (rl && i + rl > n) env *= (float)(n - i) / rl;
        buf[i] = (int16_t)(v * env * amp * 32767.0f);
    }
    return n;
}

static uint32_t silence(int16_t *buf, int ms)
{
    const uint32_t n = (uint32_t)(RATE * ms / 1000);
    memset(buf, 0, n * sizeof(*buf));
    return n;
}

// A sine ping that dies away: f and its fifth above, `ms` long.
static uint32_t ping(int16_t *buf, float f, int ms, float amp)
{
    const uint32_t n = (uint32_t)(RATE * ms / 1000);
    for (uint32_t i = 0; i < n; i++) {
        const float t = (float)i / RATE, env = expf(-t * 9.0f) * (i < 44 ? i / 44.0f : 1.0f);
        const float v = 0.7f * sinf(6.2831853f * f * t) + 0.3f * sinf(6.2831853f * f * 1.5f * t);
        buf[i] = (int16_t)(v * env * amp * 32767.0f);
    }
    return n;
}

// A marimba note: the fundamental and a quick fourth partial (the bar's), struck, dying away.
static uint32_t marimba(int16_t *buf, float f, int ms, float amp)
{
    const uint32_t n = (uint32_t)(RATE * ms / 1000), rl = RATE * 6 / 1000;
    for (uint32_t i = 0; i < n; i++) {
        const float t = (float)i / RATE;
        float v = sinf(6.2831853f * f * t) * expf(-t * 22.0f) + 0.35f * sinf(6.2831853f * f * 3.93f * t) * expf(-t * 70.0f);
        if (i < 22) v *= i / 22.0f;
        if (i + rl > n) v *= (float)(n - i) / rl;
        buf[i] = (int16_t)(v * amp * 32767.0f);
    }
    return n;
}

// A sweep from f0 to f1 Hz, wave 0 square, 1 pulse, 2 sine, with a sine-shaped swell (a chirp) or a flat level.
static uint32_t sweep(int16_t *buf, float f0, float f1, int ms, int wave, float amp, bool swell)
{
    const uint32_t n = (uint32_t)(RATE * ms / 1000), att = RATE * 2 / 1000;
    float ph = 0;
    for (uint32_t i = 0; i < n; i++) {
        const float k = (float)i / n;
        ph = fmodf(ph + (f0 + (f1 - f0) * k) / RATE, 1.0f);
        float v = wave == 2 ? sinf(ph * 6.2831853f) : (ph < (wave == 1 ? 0.25f : 0.5f) ? 1.0f : -1.0f);
        float env = swell ? sinf(k * 3.1415926f) : 1.0f;
        if (!swell && i < att) env = (float)i / att;
        if (!swell && i + att > n) env *= (float)(n - i) / att;
        buf[i] = (int16_t)(v * env * amp * 32767.0f);
    }
    return n;
}

// Noise, softened by a one-pole low-pass (lp 0..1: lower is darker), swelling in over `rise` of it and dying away.
static uint32_t noise(int16_t *buf, int ms, float amp, float lp, float rise)
{
    const uint32_t n = (uint32_t)(RATE * ms / 1000);
    uint32_t r = 22222;
    float y = 0;
    for (uint32_t i = 0; i < n; i++) {
        r = r * 1664525u + 1013904223u;
        y += lp * ((float)(int32_t)r / 2147483648.0f - y);
        const float k = (float)i / n, env = k < rise ? k / rise : (1.0f - k) / (1.0f - rise);
        buf[i] = (int16_t)(y * env * amp * 32767.0f);
    }
    return n;
}

static void make_set(int voice)
{
    // Longest sound: the alarm's six 90 ms tones; 0.6 s at 22050 Hz.
    int16_t *tmp = heap_caps_malloc(RATE * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!tmp) return;
    for (int s = 0; s < UI_SND_COUNT; s++) {
        uint32_t n = 0;
        if (voice == UI_VOICE_GARDEN) {
            switch (s) {
            case UI_SND_MOVE: n = marimba(tmp, 1568, 70, 0.2f); break;                          // G6
            case UI_SND_SELECT: n = marimba(tmp, 1046.5f, 60, 0.2f); n += marimba(tmp + n, 1568, 130, 0.2f); break;
            case UI_SND_BACK: n = marimba(tmp, 1568, 60, 0.2f); n += marimba(tmp + n, 1046.5f, 130, 0.2f); break;
            case UI_SND_ALERT:                                                                  // E5 C5 E5 C5
                for (int k = 0; k < 4; k++) n += marimba(tmp + n, k & 1 ? 523.25f : 659.25f, k == 3 ? 260 : 140, 0.28f);
                break;
            case UI_SND_NOTE:                                                                   // a bird
                n = sweep(tmp, 2600, 4200, 50, 2, 0.22f, true); n += silence(tmp + n, 35);
                n += sweep(tmp + n, 2800, 4600, 55, 2, 0.22f, true); n += silence(tmp + n, 30);
                n += sweep(tmp + n, 3600, 2400, 80, 2, 0.2f, true);
                break;
            case UI_SND_OPEN: n = noise(tmp, 200, 0.16f, 0.18f, 0.4f); n += sweep(tmp + n, 3000, 4300, 45, 2, 0.12f, true); break;
            }
        } else if (voice == UI_VOICE_SCIFI) {
            switch (s) {
            case UI_SND_MOVE: n = noise(tmp, 3, 0.18f, 0.9f, 0.1f); n += sweep(tmp + n, 3200, 2400, 9, 0, 0.1f, false); break;
            case UI_SND_SELECT: n = sweep(tmp, 700, 1800, 60, 1, 0.14f, false); n += tone(tmp + n, 2400, 40, 2, 0.2f, 25); break;
            case UI_SND_BACK: n = sweep(tmp, 1800, 600, 70, 1, 0.14f, false); break;
            case UI_SND_ALERT:
                for (int k = 0; k < 3; k++) { n += sweep(tmp + n, 520, 880, 120, 0, 0.18f, false); n += silence(tmp + n, 40); }
                break;
            case UI_SND_NOTE: n = ping(tmp, 1200, 130, 0.3f); n += ping(tmp + n, 1800, 220, 0.3f); break;
            case UI_SND_OPEN:                                                                   // clack, servo, clack
                n = noise(tmp, 14, 0.35f, 0.5f, 0.05f);
                n += sweep(tmp + n, 220, 520, 260, 1, 0.07f, true);
                n += noise(tmp + n, 18, 0.4f, 0.35f, 0.05f);
                break;
            }
        } else if (voice == UI_VOICE_ALARM) {
            switch (s) {
            case UI_SND_MOVE: n = tone(tmp, 2000, 14, 0, 0.16f, 8); break;
            case UI_SND_SELECT: n = tone(tmp, 1500, 24, 0, 0.2f, 6); n += silence(tmp + n, 8); n += tone(tmp + n, 2250, 36, 0, 0.2f, 14); break;
            case UI_SND_BACK: n = tone(tmp, 2250, 24, 0, 0.2f, 6); n += silence(tmp + n, 8); n += tone(tmp + n, 1500, 36, 0, 0.2f, 14); break;
            case UI_SND_ALERT:
                for (int k = 0; k < 3; k++) { n += tone(tmp + n, 880, 90, 0, 0.22f, 6); n += tone(tmp + n, 587, 90, 0, 0.22f, 6); }
                break;
            case UI_SND_NOTE: n = ping(tmp, 1760, 320, 0.35f); break;
            }
        } else {
            switch (s) {
            case UI_SND_MOVE: n = tone(tmp, 1046.5f, 10, 1, 0.12f, 6); break;
            case UI_SND_SELECT: n = tone(tmp, 1046.5f, 28, 1, 0.15f, 8); n += tone(tmp + n, 1568, 40, 1, 0.15f, 20); break;
            case UI_SND_BACK: n = tone(tmp, 1568, 28, 1, 0.15f, 8); n += tone(tmp + n, 1046.5f, 40, 1, 0.15f, 20); break;
            case UI_SND_ALERT: n = tone(tmp, 784, 80, 1, 0.18f, 20); n += silence(tmp + n, 40); n += tone(tmp + n, 784, 120, 1, 0.18f, 50); break;
            case UI_SND_NOTE: n = tone(tmp, 1318.5f, 60, 2, 0.3f, 20); n += tone(tmp + n, 1760, 140, 2, 0.3f, 100); break;
            }
        }
        if (!n) continue;                  // no such sound in this voice
        int16_t *pcm = heap_caps_malloc(n * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (pcm) {
            memcpy(pcm, tmp, n * sizeof(int16_t));
            s_set[voice][s] = (snd_t){ pcm, n };
        }
    }
    heap_caps_free(tmp);
}

void fh1_ui_set_sound_fn(fh1_ui_sound_fn fn) { s_play = fn; }

bool fh1_ui_sounds(void)
{
    if (s_on < 0) {
        char v[8] = "";
        s_on = fh1_settings_get_str("ui", "sounds", v, sizeof(v)) && strcmp(v, "on") == 0;
    }
    return s_on;
}

void fh1_ui_set_sounds(bool on)
{
    s_on = on;
    fh1_settings_set_str("ui", "sounds", on ? "on" : "off");
}

void ui_sound(int voice, int what)
{
    if (!s_play || what < 0 || what >= UI_SND_COUNT || voice < 0 || voice >= UI_VOICES || !fh1_ui_sounds()) return;
    static bool made[UI_VOICES];
    if (!made[voice]) {
        made[voice] = true;
        make_set(voice);
    }
    const snd_t *s = &s_set[voice][what];
    if (s->pcm) s_play(s->pcm, s->frames, RATE);
}
