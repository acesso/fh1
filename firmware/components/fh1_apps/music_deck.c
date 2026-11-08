// The Music app's deck: a full-screen, pixel-drawn hi-fi front panel. Seven-segment time, track and mode,
// a stereo level meter, the title scrolling, and four visualisers of what is heard (X cycles them):
//   SPECTRUM   32 LED bands with falling peak caps (40 at 320 wide)
//   WATERFALL  a spectrum trace over a scrolling spectrogram, like an SDR's
//   SCOPE      a dual-trace oscilloscope (left above, right below) with phosphor afterglow
//   VECTOR     a goniometer (stereo image) with left/right levels and the channels' correlation
// The analysis is a 2048-point FFT of the player's tap (fh1_player_tap), 30 times a second, on core 1.
// The window system is paused meanwhile (like the emulators); music keeps playing when the deck closes.
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "apps_priv.h"
#include "music_priv.h"
#include "fh1_ui.h"
#include "fh1_board.h"
#include "fh1_input.h"
#include "fh1_audio.h"
#include "fh1_app.h"
#include "fh1_player.h"
#include "fh1_settings.h"
#include "fh1_i18n.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

const uint8_t *fh1_font8_glyph(uint32_t cp);          // fh1_ui: unscii-8, ASCII + Latin-1

#define FFT_N 2048
#define FRAME_MS 33
#define F_LO 30.0f                     // the analysers' frequency range
#define F_HI 16000.0f

enum { VIS_SPECTRUM, VIS_WATERFALL, VIS_SCOPE, VIS_VECTOR, VIS_COUNT };
static const char *const s_vis_names[VIS_COUNT] = { "SPECTRUM", "WATERFALL", "SCOPE", "VECTOR" };

// ── Colours ──

typedef struct {
    const char *name;
    uint32_t bg, panel, frame, grid, text, dim, accent, hi, lo, mid, top;
    uint32_t heat[5];                  // the spectrogram's colour map, quiet to loud
} scheme_t;

static const scheme_t s_schemes[] = {
    { "AMBER", 0x0b0703, 0x1c1108, 0x6a4012, 0x2c1c0a, 0xffb43c, 0x8a5a22, 0xffc850, 0xfff2c8, 0xff8c1a, 0xffd040,
      0xff4a20, { 0x0b0703, 0x4a1400, 0xb04800, 0xffa828, 0xfff4d0 } },
    { "PHOSPHOR", 0x020a04, 0x07190c, 0x1f6a30, 0x0c2c16, 0x5cff8a, 0x2f8048, 0x8affa8, 0xdcffe8, 0x30d060, 0xc8ff48,
      0xff5a30, { 0x020a04, 0x003a14, 0x00a844, 0x90ff60, 0xf4fff4 } },
    { "SYNTH", 0x0b0418, 0x180b2c, 0x6a32a0, 0x26143e, 0x48e4ff, 0x7a5aa8, 0xff3cac, 0xffffff, 0x2b9be0, 0xc848ff,
      0xff3c6a, { 0x0b0418, 0x2a1058, 0x8a40b0, 0xff3cac, 0xfff0ff } },
    { "LCD", 0x9bbc0f, 0x8bac0f, 0x306230, 0x8aa80e, 0x0f380f, 0x4c7a1c, 0x0f380f, 0x081c08, 0x306230, 0x1f4a1f,
      0x0f380f, { 0x9bbc0f, 0x7a9a12, 0x4c7a1c, 0x306230, 0x0f380f } },
};
#define SCHEMES (int)(sizeof(s_schemes) / sizeof(s_schemes[0]))

typedef struct {
    uint16_t bg, panel, frame, grid, text, dim, accent, hi, lo, mid, top, ghost;
    uint16_t heat[64];                 // spectrogram
    uint16_t glow_a[64], glow_b[64];   // afterglow: background to trace colour
} pal_t;

static uint32_t mix(uint32_t a, uint32_t b, float t)
{
    int r = (int)((a >> 16 & 255) + ((int)(b >> 16 & 255) - (int)(a >> 16 & 255)) * t);
    int g = (int)((a >> 8 & 255) + ((int)(b >> 8 & 255) - (int)(a >> 8 & 255)) * t);
    int l = (int)((a & 255) + ((int)(b & 255) - (int)(a & 255)) * t);
    return (uint32_t)(r << 16 | g << 8 | l);
}
static uint16_t c565(uint32_t c) { return (uint16_t)((c >> 8 & 0xF800) | (c >> 5 & 0x07E0) | (c >> 3 & 0x001F)); }

// ── State ──

typedef struct {
    float re[FFT_N], im[FFT_N], win[FFT_N], cs[FFT_N / 2], sn[FFT_N / 2];
    uint16_t rev[FFT_N];
    float level[FFT_N / 2];            // per bin, 0..1 (-84..-6 dBFS)
    int16_t tap[FFT_N * 2];
    float band[48], peak[48];
    int peak_hold[48];
    float vu[2], vu_peak[2];
    int vu_hold[2];
    float corr;
    float colbin[480];                 // per screen column: the FFT bin (log frequency)
    uint32_t colbin_rate;
    int colbin_w;
} dsp_t;

static TaskHandle_t s_task;
static uint16_t *s_fb, *s_bb;          // the screen; the back buffer (PSRAM), copied at once to avoid flicker
static int W, H;
static dsp_t *s_d;
static uint16_t *s_wf;                 // waterfall rows (ring), PSRAM
static int s_wf_w, s_wf_h, s_wf_top;
static uint8_t *s_glow;                // afterglow intensity per pixel of the visualiser, PSRAM
static int s_vis, s_scheme;
static pal_t s_p;

static void set_scheme(int i)
{
    s_scheme = (i % SCHEMES + SCHEMES) % SCHEMES;
    const scheme_t *s = &s_schemes[s_scheme];
    s_p.bg = c565(s->bg);
    s_p.panel = c565(s->panel);
    s_p.frame = c565(s->frame);
    s_p.grid = c565(s->grid);
    s_p.text = c565(s->text);
    s_p.dim = c565(s->dim);
    s_p.accent = c565(s->accent);
    s_p.hi = c565(s->hi);
    s_p.lo = c565(s->lo);
    s_p.mid = c565(s->mid);
    s_p.top = c565(s->top);
    s_p.ghost = c565(mix(s->panel, s->text, 0.13f));
    for (int i = 0; i < 64; i++) {
        float t = i / 63.0f * 4;
        int k = t >= 4 ? 3 : (int)t;
        s_p.heat[i] = c565(mix(s->heat[k], s->heat[k + 1], t - k));
        float g = i / 63.0f;
        s_p.glow_a[i] = c565(g < 0.7f ? mix(s->bg, s->accent, g / 0.7f) : mix(s->accent, s->hi, (g - 0.7f) / 0.3f));
        s_p.glow_b[i] = c565(g < 0.7f ? mix(s->bg, s->lo, g / 0.7f) : mix(s->lo, s->hi, (g - 0.7f) / 0.3f));
    }
}

// ── Drawing (into the back buffer, clipped) ──

static void rect(int x, int y, int w, int h, uint16_t c)
{
    if (x < 0) w += x, x = 0;
    if (y < 0) h += y, y = 0;
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    for (int j = 0; j < h; j++) {
        uint16_t *p = s_bb + (y + j) * W + x;
        for (int i = 0; i < w; i++) p[i] = c;
    }
}

static inline void px(int x, int y, uint16_t c)
{
    if ((unsigned)x < (unsigned)W && (unsigned)y < (unsigned)H) s_bb[y * W + x] = c;
}

static void frame_box(int x, int y, int w, int h, uint16_t fill, uint16_t edge)
{
    rect(x, y, w, h, edge);
    rect(x + 1, y + 1, w - 2, h - 2, fill);
}

// The next code point of a UTF-8 string (Latin-1 bytes that are not UTF-8 pass as themselves).
static uint32_t utf8_next(const char **s)
{
    const uint8_t *p = (const uint8_t *)*s;
    uint32_t c = *p++;
    if (c >= 0xC0 && c < 0xE0 && (*p & 0xC0) == 0x80) c = (c & 0x1F) << 6 | (*p++ & 0x3F);
    else if (c >= 0xE0 && c < 0xF0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80) {
        c = (c & 0x0F) << 12 | (p[0] & 0x3F) << 6 | (p[1] & 0x3F);
        p += 2;
    } else if (c >= 0xF0 && (p[0] & 0xC0) == 0x80) {
        while ((*p & 0xC0) == 0x80) p++;
        c = '?';
    }
    *s = (const char *)p;
    return c;
}

static int text_len(const char *s)
{
    int n = 0;
    while (*s) {
        utf8_next(&s);
        n++;
    }
    return n * 8;
}

// Text in the 8 x 8 font, cut to columns [cx0, cx1); returns the x after it.
static int text_clip(int x, int y, const char *s, uint16_t c, int cx0, int cx1)
{
    while (*s) {
        uint32_t cp = utf8_next(&s);
        if (cp > 0xFF) cp = '?';
        if (x + 8 > cx0 && x < cx1) {
            const uint8_t *g = fh1_font8_glyph(cp);
            for (int r = 0; r < 8; r++) {
                for (int b = 0; b < 8; b++) {
                    if (g[r] & (0x80 >> b) && x + b >= cx0 && x + b < cx1) px(x + b, y + r, c);
                }
            }
        }
        x += 8;
    }
    return x;
}
static int text(int x, int y, const char *s, uint16_t c) { return text_clip(x, y, s, c, 0, W); }
static void text_right(int x_end, int y, const char *s, uint16_t c) { text(x_end - text_len(s), y, s, c); }

// Triangles pointing right, left, up or down, h pixels tall (or wide).
static void tri(int x, int y, int h, int dir, uint16_t c)
{
    for (int r = 0; r < h; r++) {
        int len = (r < h - 1 - r ? r : h - 1 - r) + 1;
        if (dir == 0) rect(x, y + r, len, 1, c);
        else if (dir == 1) rect(x + h / 2 - len + 1, y + r, len, 1, c);
        else if (dir == 2) rect(x + r, y + h / 2 - len + 1, 1, len, c);
        else rect(x + r, y, 1, len, c);
    }
}

// A button chip: its colour, its letter; returns the x after it.
static int chip(int x, int y, const char *label, uint32_t colour)
{
    int w = text_len(label) + 3;
    rect(x, y - 1, w, 10, c565(colour));
    text(x + 2, y, label, c565(colour == 0xf4c21f || colour == 0xf2f2f2 ? 0x000000 : 0xffffff));
    return x + w + 3;
}

static int arrows_chip(int x, int y, bool horizontal)
{
    rect(x, y - 1, 17, 10, c565(0x6a6f78));
    if (horizontal) {
        tri(x + 2, y + 1, 7, 1, 0xFFFF);
        tri(x + 11, y + 1, 7, 0, 0xFFFF);
    } else {
        tri(x + 1, y + 1, 7, 2, 0xFFFF);
        tri(x + 9, y + 1, 7, 3, 0xFFFF);
    }
    return x + 20;
}

// Seven segments: bits a..g; unlit ones in the ghost colour, as on an LCD.
static void seg7(int x, int y, int w, int h, int t, int digit)
{
    static const uint8_t masks[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    const uint8_t m = digit >= 0 && digit <= 9 ? masks[digit] : 0x40;      // '-' otherwise
    const int hh = h / 2;
    const struct { int x, y, w, h; } s[7] = {
        { x + t, y, w - 2 * t, t },                    // a
        { x + w - t, y + t, t, hh - t },               // b
        { x + w - t, y + hh + 1, t, h - hh - t - 1 },  // c
        { x + t, y + h - t, w - 2 * t, t },            // d
        { x, y + hh + 1, t, h - hh - t - 1 },          // e
        { x, y + t, t, hh - t },                       // f
        { x + t, y + hh - t / 2, w - 2 * t, t },       // g
    };
    for (int i = 0; i < 7; i++) rect(s[i].x, s[i].y, s[i].w, s[i].h, m >> i & 1 ? s_p.text : s_p.ghost);
}

// ── Analysis ──

static void dsp_init(dsp_t *d)
{
    for (int i = 0; i < FFT_N; i++) d->win[i] = 0.5f - 0.5f * cosf(2 * (float)M_PI * i / (FFT_N - 1));
    for (int i = 0; i < FFT_N / 2; i++) {
        d->cs[i] = cosf(2 * (float)M_PI * i / FFT_N);
        d->sn[i] = sinf(2 * (float)M_PI * i / FFT_N);
    }
    int bits = 0;
    while ((1 << bits) < FFT_N) bits++;
    for (int i = 0; i < FFT_N; i++) {
        int r = 0;
        for (int b = 0; b < bits; b++) r |= ((i >> b) & 1) << (bits - 1 - b);
        d->rev[i] = (uint16_t)r;
    }
}

static void fft(dsp_t *d)
{
    float *re = d->re, *im = d->im;
    for (int i = 0; i < FFT_N; i++) {
        int j = d->rev[i];
        if (j > i) {
            float t = re[i];
            re[i] = re[j];
            re[j] = t;
            t = im[i];
            im[i] = im[j];
            im[j] = t;
        }
    }
    for (int size = 2; size <= FFT_N; size <<= 1) {
        const int half = size >> 1, step = FFT_N / size;
        for (int i = 0; i < FFT_N; i += size) {
            for (int j = 0; j < half; j++) {
                const float c = d->cs[j * step], s = d->sn[j * step];
                const int a = i + j, b = a + half;
                const float tr = re[b] * c + im[b] * s, ti = im[b] * c - re[b] * s;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
            }
        }
    }
}

static float interp(const float *v, int n, float i)
{
    if (i <= 0) return v[0];
    if (i >= n - 1) return v[n - 1];
    int k = (int)i;
    return v[k] + (v[k + 1] - v[k]) * (i - k);
}

// Level of the frequencies [f0, f1): the loudest bin, or between bins where the band is narrower than one.
static float band_level(dsp_t *d, float f0, float f1, uint32_t rate)
{
    const float b0 = f0 * FFT_N / rate, b1 = f1 * FFT_N / rate;
    if ((int)b1 - (int)b0 < 1) return interp(d->level, FFT_N / 2, (b0 + b1) / 2);
    float m = 0;
    for (int k = (int)ceilf(b0); k <= (int)b1 && k < FFT_N / 2; k++) m = d->level[k] > m ? d->level[k] : m;
    return m;
}

// What the visualisers need from the last FFT_N frames heard: per-bin levels, levels per channel, the
// channels' correlation. False when nothing plays.
static bool analyse(dsp_t *d, uint32_t *rate_out)
{
    uint32_t rate = 0;
    size_t n = fh1_player_tap(d->tap, FFT_N, &rate);
    if (!rate) rate = 44100;
    *rate_out = rate;
    if (n < FFT_N) {
        memset(d->level, 0, sizeof(d->level));
        memset(d->tap, 0, sizeof(d->tap));
        d->vu[0] = d->vu[1] = 0;
        return n > 0;
    }
    double sl = 0, sr = 0, slr = 0;
    for (int i = 0; i < FFT_N; i++) {
        const float l = d->tap[2 * i], r = d->tap[2 * i + 1];
        d->re[i] = (l + r) * 0.5f * d->win[i];
        d->im[i] = 0;
        if (i >= FFT_N / 2) {                          // the levels over the newest 1024 frames (23 ms)
            sl += l * l;
            sr += r * r;
            slr += l * r;
        }
    }
    fft(d);
    const float scale = 4.0f / (FFT_N * 32768.0f);    // full-scale sine = 1 (Hann window: gain 1/2)
    for (int k = 0; k < FFT_N / 2; k++) {
        float m = sqrtf(d->re[k] * d->re[k] + d->im[k] * d->im[k]) * scale;
        float db = 20 * log10f(m + 1e-9f);
        float v = (db + 84) / 78;
        d->level[k] = v < 0 ? 0 : v > 1 ? 1 : v;
    }
    const float half = FFT_N / 2;
    for (int c = 0; c < 2; c++) {
        float rms = sqrtf((float)(c ? sr : sl) / half) / 32768.0f;
        float v = (20 * log10f(rms + 1e-9f) + 48) / 48;  // -48..0 dBFS
        v = v < 0 ? 0 : v > 1 ? 1 : v;
        d->vu[c] = v > d->vu[c] ? v : d->vu[c] * 0.8f + v * 0.2f;
        if (d->vu[c] >= d->vu_peak[c]) {
            d->vu_peak[c] = d->vu[c];
            d->vu_hold[c] = 20;
        } else if (--d->vu_hold[c] < 0) {
            d->vu_peak[c] -= 0.02f;
        }
    }
    const double den = sqrt(sl * sr);
    const float corr = den > 1e3 ? (float)(slr / den) : 0;
    d->corr = d->corr * 0.85f + corr * 0.15f;
    return true;
}

static void colbin_update(dsp_t *d, int w, uint32_t rate)
{
    if (d->colbin_rate == rate && d->colbin_w == w) return;
    const float hi = F_HI < rate / 2.0f ? F_HI : rate / 2.0f;
    for (int x = 0; x < w && x < 480; x++) d->colbin[x] = F_LO * powf(hi / F_LO, (float)x / (w - 1)) * FFT_N / rate;
    d->colbin_rate = rate;
    d->colbin_w = w;
}

// x of a frequency on the log axis [F_LO, hi] across w pixels.
static int freq_x(float f, int w, uint32_t rate)
{
    const float hi = F_HI < rate / 2.0f ? F_HI : rate / 2.0f;
    return (int)(logf(f / F_LO) / logf(hi / F_LO) * (w - 1));
}

static void freq_axis(int x0, int y, int w, uint32_t rate)
{
    static const struct { float f; const char *t; } marks[] = { { 100, "100" }, { 300, "300" }, { 1000, "1K" },
                                                               { 3000, "3K" }, { 10000, "10K" } };
    for (size_t i = 0; i < sizeof(marks) / sizeof(marks[0]); i++) {
        if (marks[i].f > rate / 2) continue;
        int x = x0 + freq_x(marks[i].f, w, rate);
        rect(x, y - 2, 1, 2, s_p.dim);
        int tw = text_len(marks[i].t);
        text_clip(x - tw / 2, y + 1, marks[i].t, s_p.dim, x0, x0 + w);
    }
}

// ── Visualisers ──

static void vis_spectrum(dsp_t *d, int x0, int y0, int w, int h, uint32_t rate)
{
    const int nb = w >= 300 ? 40 : 32, gap = 2;
    const int bw = (w - (nb + 1) * gap) / nb;
    const int bx = x0 + (w - (nb * bw + (nb + 1) * gap)) / 2 + gap;
    const int ah = h - 12;                             // above the frequency axis
    const int seg = 4, rows = ah / seg;
    const int by = y0 + ah;                            // bars stand on this line
    for (int r = 1; r < 4; r++) {                      // a faint scale every quarter
        for (int x = x0 + 2; x < x0 + w - 2; x += 3) px(x, by - r * rows * seg / 4, s_p.grid);
    }
    const float hi = F_HI < rate / 2.0f ? F_HI : rate / 2.0f;
    for (int b = 0; b < nb; b++) {
        float f0 = F_LO * powf(hi / F_LO, (float)b / nb), f1 = F_LO * powf(hi / F_LO, (float)(b + 1) / nb);
        float v = band_level(d, f0, f1, rate);
        d->band[b] = v > d->band[b] ? v : d->band[b] - 0.025f;
        if (d->band[b] < 0) d->band[b] = 0;
        if (d->band[b] >= d->peak[b]) {
            d->peak[b] = d->band[b];
            d->peak_hold[b] = 18;
        } else if (--d->peak_hold[b] < 0) {
            d->peak[b] -= 0.012f * (1 - d->peak_hold[b] / 10.0f);   // falls, faster and faster
            if (d->peak[b] < 0) d->peak[b] = 0;
        }
        const int x = bx + b * (bw + gap);
        const int lit = (int)(d->band[b] * rows + 0.5f);
        for (int s = 0; s < rows; s++) {
            const uint16_t c = s < lit ? (s < rows * 6 / 10 ? s_p.lo : s < rows * 85 / 100 ? s_p.mid : s_p.top) : s_p.grid;
            if (s < lit || (s & 1) == 0) rect(x, by - (s + 1) * seg + 1, bw, seg - 1, c);
        }
        const int pk = (int)(d->peak[b] * rows);
        if (pk > 0) rect(x, by - (pk + 1) * seg + 1, bw, seg - 2, s_p.hi);
    }
    freq_axis(x0, y0 + ah + 2, w, rate);
}

static void vis_waterfall(dsp_t *d, int x0, int y0, int w, int h, uint32_t rate)
{
    colbin_update(d, w, rate);
    const int sh = h * 35 / 100;                       // the spectrum trace
    const int wy = y0 + sh + 2, wh = h - sh - 2 - 12;  // the waterfall, then the axis
    // dB lines on the trace: -24, -48 dB.
    for (int r = 1; r < 3; r++) {
        for (int x = x0; x < x0 + w; x += 2) px(x, y0 + sh - (int)(sh * (78 - 24 * r) / 78.0f), s_p.grid);
    }
    int prev = -1;
    for (int x = 0; x < w; x++) {
        float v = interp(d->level, FFT_N / 2, d->colbin[x]);
        int top = y0 + sh - (int)(v * (sh - 1));
        rect(x0 + x, top + 1, 1, y0 + sh - top, s_p.grid);
        if (prev >= 0) rect(x0 + x, top < prev ? top : prev, 1, (top < prev ? prev - top : top - prev) + 1, s_p.accent);
        else px(x0 + x, top, s_p.accent);
        prev = top;
    }
    rect(x0, wy - 1, w, 1, s_p.frame);
    // A new row on top each frame (~30 rows a second); the oldest falls off the bottom.
    if (s_wf && s_wf_w == w && s_wf_h == wh) {
        s_wf_top = (s_wf_top + s_wf_h - 1) % s_wf_h;
        uint16_t *row = s_wf + s_wf_top * w;
        for (int x = 0; x < w; x++) {
            float v = interp(d->level, FFT_N / 2, d->colbin[x]);
            row[x] = s_p.heat[(int)(v * 63)];
        }
        for (int r = 0; r < wh; r++) memcpy(s_bb + (wy + r) * W + x0, s_wf + ((s_wf_top + r) % s_wf_h) * w, w * 2);
    }
    freq_axis(x0, wy + wh + 2, w, rate);
}

// Afterglow: the intensity buffer fades every frame and traces light it up; drawn through a colour ramp.
static void glow_fade(int n)
{
    for (int i = 0; i < n; i++) s_glow[i] = (uint8_t)((s_glow[i] * 5) >> 3);
}

static void glow_line(int w, int h, int x0, int y0, int x1, int y1)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx - dy;
    for (int guard = 0; guard < 2048; guard++) {
        if ((unsigned)x0 < (unsigned)w && (unsigned)y0 < (unsigned)h) s_glow[y0 * w + x0] = 255;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) err -= dy, x0 += sx;
        if (e2 < dx) err += dx, y0 += sy;
    }
}

static void glow_draw(int x0, int y0, int w, int h, int split)
{
    for (int y = 0; y < h; y++) {
        const uint16_t *ramp = y < split ? s_p.glow_a : s_p.glow_b;
        const uint8_t *g = s_glow + y * w;
        uint16_t *o = s_bb + (y0 + y) * W + x0;
        for (int x = 0; x < w; x++) {
            if (g[x] > 8) o[x] = ramp[g[x] >> 2];
        }
    }
}

static void graticule(int x0, int y0, int w, int h, int cols, int rows)
{
    for (int c = 1; c < cols; c++) {
        for (int y = y0; y < y0 + h; y += 2) px(x0 + c * w / cols, y, s_p.grid);
    }
    for (int r = 1; r < rows; r++) {
        for (int x = x0; x < x0 + w; x += 2) px(x, y0 + r * h / rows, s_p.grid);
    }
}

static void vis_scope(dsp_t *d, int x0, int y0, int w, int h, uint32_t rate)
{
    graticule(x0, y0, w, h, 10, 8);
    const int span = 512;                              // samples across the screen (~12 ms at 44.1 kHz)
    // Trigger: the left channel rising through zero, in the older half, so the trace stands still.
    int start = FFT_N - span - 1;
    for (int i = FFT_N - span - 1; i > FFT_N / 4; i--) {
        if (d->tap[2 * (i - 1)] < 0 && d->tap[2 * i] >= 0) {
            start = i;
            break;
        }
    }
    glow_fade(w * h);
    // Automatic gain, as a scope's auto-set would: the louder channel fills most of its half.
    static float gain = 1;
    int peak = 1;
    for (int i = start; i < start + span; i++) {
        int a = d->tap[2 * i] < 0 ? -d->tap[2 * i] : d->tap[2 * i];
        int b = d->tap[2 * i + 1] < 0 ? -d->tap[2 * i + 1] : d->tap[2 * i + 1];
        peak = a > peak ? a : b > peak ? b : peak;
    }
    float want = 26000.0f / peak;
    want = want < 1 ? 1 : want > 8 ? 8 : want;
    gain = want < gain ? want : gain * 0.95f + want * 0.05f;      // down at once, up slowly
    const int half = h / 2;
    for (int ch = 0; ch < 2; ch++) {
        const int mid = ch ? half + half / 2 : half / 2;
        int py = mid;
        for (int x = 0; x < w; x++) {
            int s = (int)(d->tap[2 * (start + x * span / w) + ch] * gain);
            int y = mid - s * (half / 2 - 2) / 32768;
            if (x) glow_line(w, h, x - 1, py, x, y);
            py = y;
        }
    }
    glow_draw(x0, y0, w, h, half);
    rect(x0, y0 + half, w, 1, s_p.frame);
    char t[24];
    snprintf(t, sizeof(t), "%.1fMS/DIV", span * 100.0f / rate);
    text_right(x0 + w - 2, y0 + h - 9, t, s_p.dim);
    snprintf(t, sizeof(t), "x%.1f", gain);
    text_right(x0 + w - 2, y0 + 2, t, s_p.dim);
    text(x0 + 2, y0 + 12, "CH1 L", s_p.dim);
    text(x0 + 2, y0 + half + 2, "CH2 R", s_p.dim);
}

static void vis_vector(dsp_t *d, int x0, int y0, int w, int h)
{
    const int side = h - 4, cx = x0 + w / 2, cy = y0 + h / 2;
    const int bx = cx - side / 2;
    // The axes: mono on the vertical, the channels on the diagonals.
    for (int i = -side / 2; i <= side / 2; i += 2) {
        px(cx, cy + i, s_p.grid);
        px(cx + i, cy + i, s_p.grid);
        px(cx + i, cy - i, s_p.grid);
    }
    glow_fade(w * h);
    int pxx = -1, pyy = -1;
    for (int i = FFT_N / 2; i < FFT_N; i++) {
        float l = d->tap[2 * i] / 32768.0f, r = d->tap[2 * i + 1] / 32768.0f;
        int x = cx - x0 + (int)((r - l) * 0.7071f * side / 2);
        int y = cy - y0 - (int)((l + r) * 0.7071f * side / 2);
        if (pxx >= 0) glow_line(w, h, pxx, pyy, x, y);
        pxx = x;
        pyy = y;
    }
    glow_draw(x0, y0, w, h, h);
    text(bx + 4, y0 + 14, "L", s_p.dim);
    text(bx + side - 12, y0 + 14, "R", s_p.dim);
    // Level bars at the sides.
    const int bh = h - 24;
    for (int c = 0; c < 2; c++) {
        int x = c ? x0 + w - 16 : x0 + 6;
        int lit = (int)(d->vu[c] * bh), pk = (int)(d->vu_peak[c] * bh);
        rect(x, y0 + 12, 10, bh, s_p.grid);
        rect(x, y0 + 12 + bh - lit, 10, lit, d->vu[c] > 0.9f ? s_p.top : s_p.lo);
        if (pk > 0) rect(x, y0 + 12 + bh - pk, 10, 2, s_p.hi);
        text(x + 1, y0 + 12 + bh + 2, c ? "R" : "L", s_p.dim);
    }
    char t[16];
    snprintf(t, sizeof(t), "%+.2f", d->corr);
    text_right(x0 + w - 26, y0 + h - 9, t, d->corr < 0 ? s_p.top : s_p.dim);
    text(x0 + 22, y0 + h - 9, "CORR", s_p.dim);
}

// ── The deck ──

typedef struct {
    uint32_t serial;
    int marquee, marquee_wait;
    int64_t vol_shown_until;
    uint8_t volume;
    bool volume_changed;
    int blink;
} deck_t;

static void fmt_time(char *out, size_t n, uint32_t ms)
{
    uint32_t s = ms / 1000;
    snprintf(out, n, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

static void draw_panel(deck_t *k, const fh1_player_status_t *st, dsp_t *d)
{
    const int M = 4;
    rect(0, 0, W, H, s_p.bg);

    // Header: the name, and what is playing in which format.
    tri(M, 2, 8, 0, s_p.accent);
    char f[40] = "";
    if (st->format[0] && st->state != FH1_PLAYER_STOPPED) {
        char r[12];
        if (st->rate % 1000 >= 100) snprintf(r, sizeof(r), "%lu.%luK", (unsigned long)(st->rate / 1000), (unsigned long)(st->rate % 1000 / 100));
        else snprintf(r, sizeof(r), "%luK", (unsigned long)(st->rate / 1000));
        if (st->kbps) snprintf(f, sizeof(f), "%s %uK %s %s", st->format, st->kbps, r, st->channels > 1 ? "ST" : "MONO");
        else snprintf(f, sizeof(f), "%s %s %s", st->format, r, st->channels > 1 ? "ST" : "MONO");
        text_right(W - M, 2, f, s_p.text);
    }
    if (M + 7 + text_len("FH1 AUDIO") + 8 + text_len(f) <= W - M) text(M + 7, 2, "FH1 AUDIO", s_p.accent);   // when it fits

    // The LCD: time, track, state, mode; the level meter.
    const int ly = 13, lh = 36;
    frame_box(M, ly, W - 2 * M, lh, s_p.panel, s_p.frame);
    const bool live = st->state == FH1_PLAYER_PLAYING || st->state == FH1_PLAYER_PAUSED;
    const uint32_t secs = st->pos_ms / 1000;
    const int dw = 13, dh = 26, dt = 3, tx = M + 5, ty = ly + 5;
    const int mm = live ? (int)(secs / 60 % 100) : -1, ss = live ? (int)(secs % 60) : -1;
    const bool show = !(st->state == FH1_PLAYER_PAUSED && (k->blink / 15) & 1);   // paused: the time blinks
    seg7(tx, ty, dw, dh, dt, show && mm >= 0 ? mm / 10 : -1);
    seg7(tx + dw + 4, ty, dw, dh, dt, show && mm >= 0 ? mm % 10 : -1);
    rect(tx + 2 * dw + 9, ty + 7, 3, 3, show && live ? s_p.text : s_p.ghost);
    rect(tx + 2 * dw + 9, ty + 17, 3, 3, show && live ? s_p.text : s_p.ghost);
    seg7(tx + 2 * dw + 16, ty, dw, dh, dt, show && ss >= 0 ? ss / 10 : -1);
    seg7(tx + 3 * dw + 20, ty, dw, dh, dt, show && ss >= 0 ? ss % 10 : -1);

    const int cx = tx + 4 * dw + 28;
    int idx, count;
    music_position(&idx, &count);
    char t[48];
    snprintf(t, sizeof(t), "TRK %02d/%02d", idx >= 0 ? idx + 1 : 0, count);
    text(cx, ly + 4, t, s_p.text);
    // State: a symbol and a word.
    const int sy = ly + 14;
    if (st->state == FH1_PLAYER_PLAYING) {
        tri(cx, sy, 8, 0, s_p.accent);
        text(cx + 9, sy, "PLAY", s_p.text);
    } else if (st->state == FH1_PLAYER_PAUSED) {
        rect(cx, sy, 2, 8, s_p.accent);
        rect(cx + 4, sy, 2, 8, s_p.accent);
        text(cx + 9, sy, "PAUSE", s_p.text);
    } else if (st->state == FH1_PLAYER_LOADING) {
        text(cx, sy, "LOAD", (k->blink >> 2) & 1 ? s_p.text : s_p.dim);
    } else {
        rect(cx, sy, 7, 7, s_p.dim);
        text(cx + 9, sy, "STOP", s_p.dim);
    }
    static const char *const modes[MUSIC_MODES] = { "ALL", "ONE", "SHUF" };
    text(cx, ly + 25, modes[music_mode()], s_p.dim);
    if (live && st->dur_ms) {
        fmt_time(t, sizeof(t), st->dur_ms);
        text(cx + 40, ly + 25, t, s_p.dim);
    }
    // Levels: L and R, 12 segments each, the last two red.
    const int vx = W - M - 4 - (W >= 300 ? 96 : 50), vw = W - M - 4 - vx - 10;
    for (int c = 0; c < 2; c++) {
        const int y = ly + 5 + c * 9;
        text(vx, y, c ? "R" : "L", s_p.dim);
        const int segs = 12, sw = vw / segs;
        int lit = (int)(d->vu[c] * segs + 0.5f), pk = (int)(d->vu_peak[c] * segs);
        for (int s = 0; s < segs; s++) {
            uint16_t col = s < lit || s == pk ? (s >= segs - 2 ? s_p.top : s >= segs - 4 ? s_p.mid : s_p.lo) : s_p.ghost;
            rect(vx + 10 + s * sw, y + 1, sw - 1, 6, col);
        }
    }
    text(vx, ly + 25, "dB", s_p.ghost);
    // A clipping light.
    rect(W - M - 9, ly + 26, 5, 5, d->vu_peak[0] > 0.98f || d->vu_peak[1] > 0.98f ? s_p.top : s_p.ghost);

    // Title (scrolling when it does not fit), then artist and album.
    const int tyy = 52, tw = text_len(st->title), room = W - 2 * M;
    if (st->serial != k->serial) {
        k->serial = st->serial;
        k->marquee = 0;
        k->marquee_wait = 60;
    }
    if (tw > room) {
        if (k->marquee_wait > 0) k->marquee_wait--;
        else if (++k->marquee > tw - room) {
            k->marquee = 0;
            k->marquee_wait = 60;
        } else if (k->marquee == tw - room) {
            k->marquee_wait = 60;
        }
    }
    const char *title = st->state == FH1_PLAYER_STOPPED && !st->title[0] ? "- NO TRACK -" : st->title;
    text_clip(M - (tw > room ? k->marquee : 0), tyy, title, s_p.hi, M, W - M);
    char sub[140];
    if (st->artist[0] && st->album[0]) snprintf(sub, sizeof(sub), "%s \xC2\xB7 %s", st->artist, st->album);
    else snprintf(sub, sizeof(sub), "%s", st->artist[0] ? st->artist : st->album);
    text_clip(M, tyy + 10, sub, s_p.dim, M, W - M);

    // Position: a segmented bar.
    const int py = H - 30, pw = W - 2 * M;
    const int pos = st->dur_ms ? (int)((uint64_t)st->pos_ms * pw / st->dur_ms) : 0;
    for (int x = 0; x < pw; x += 3) rect(M + x, py, 2, 5, x < pos ? s_p.accent : s_p.grid);

    // The keys.
    const int hy = H - 20;
    int x = chip(M, hy, "A", 0xe5302c);
    x = text(x, hy, st->state == FH1_PLAYER_PLAYING ? "PAUSE" : "PLAY", s_p.dim) + 6;
    x = arrows_chip(x, hy, true);
    x = text(x, hy, "TRACK", s_p.dim) + 6;
    x = arrows_chip(x, hy, false);
    text(x, hy, "VOL", s_p.dim);
    x = chip(M, hy + 11, "X", 0x2f6fe8);
    x = text(x, hy + 11, "VIS", s_p.dim) + 6;
    x = chip(x, hy + 11, "Y", 0x2fb36a);
    x = text(x, hy + 11, "COLOR", s_p.dim) + 6;
    x = chip(x, hy + 11, "SEL", 0xf2f2f2);
    x = text(x, hy + 11, "MODE", s_p.dim) + 6;
    x = chip(x, hy + 11, "B", 0xf4c21f);
    text(x, hy + 11, "BACK", s_p.dim);
}

static void draw_vis(deck_t *k, const fh1_player_status_t *st, dsp_t *d, bool sound, uint32_t rate)
{
    const int x0 = 4, y0 = 73, w = W - 8, h = H - 73 - 34;
    frame_box(x0, y0, w, h, s_p.bg, s_p.frame);
    const int ix = x0 + 1, iy = y0 + 1, iw = w - 2, ih = h - 2;
    if (!sound && st->state != FH1_PLAYER_PAUSED) {
        const char *msg = st->state == FH1_PLAYER_LOADING ? "LOADING" : st->error[0] ? st->error : "NO SIGNAL";
        text(ix + (iw - text_len(msg)) / 2, iy + ih / 2 - 8, msg, st->error[0] ? s_p.top : s_p.dim);
        if ((k->blink / 20) & 1 && st->state == FH1_PLAYER_STOPPED) {
            const char *p = "PRESS A";
            text(ix + (iw - text_len(p)) / 2, iy + ih / 2 + 6, p, s_p.text);
        }
        if (s_glow) memset(s_glow, 0, iw * ih);
    } else {
        switch (s_vis) {
        case VIS_SPECTRUM: vis_spectrum(d, ix, iy, iw, ih, rate); break;
        case VIS_WATERFALL: vis_waterfall(d, ix, iy, iw, ih, rate); break;
        case VIS_SCOPE: vis_scope(d, ix, iy, iw, ih, rate); break;
        default: vis_vector(d, ix, iy, iw, ih); break;
        }
    }
    text(ix + 2, iy + 2, s_vis_names[s_vis], s_p.dim);
    // Volume, for a moment after it changes.
    if (esp_timer_get_time() < k->vol_shown_until) {
        const int bw = iw * 2 / 3, bx = ix + (iw - bw) / 2, by = iy + ih / 2 - 12;
        frame_box(bx - 4, by - 4, bw + 8, 30, s_p.panel, s_p.frame);
        char t[16];
        snprintf(t, sizeof(t), "VOLUME %u%%", k->volume);
        text(bx, by, t, s_p.text);
        for (int s = 0; s < 20; s++) rect(bx + s * bw / 20, by + 12, bw / 20 - 1, 8, s < k->volume / 5 ? s_p.accent : s_p.ghost);
    }
}

// ── The task ──

static bool pressed(bool *was, int key)
{
    bool now = fh1_input_is_down(key);
    bool edge = now && !*was;
    *was = now;
    return edge;
}

static void deck_closed_ui(void *arg) { music_deck_closed(); }

static void deck_task(void *arg)
{
    deck_t k = { .serial = UINT32_MAX };
    k.volume = fh1_audio_master_volume();
    bool was[FH1_KEY_COUNT];
    for (int i = 0; i < FH1_KEY_COUNT; i++) was[i] = fh1_input_is_down(i);   // a key held on entry is not a press
    int64_t held_since[2] = { 0, 0 };
    int64_t last_seek = 0;
    bool seeking[2] = { false, false };
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        if (fh1_app_quit_requested()) break;
        if (fh1_app_should_yield()) {
            fh1_ui_lend_screen();
            if (fh1_app_quit_requested()) break;
            wake = xTaskGetTickCount();
            continue;
        }
        fh1_player_status_t st;
        fh1_player_status(&st);
        const int64_t now = esp_timer_get_time();

        if (pressed(&was[FH1_KEY_B], FH1_KEY_B) || pressed(&was[FH1_KEY_MENU], FH1_KEY_MENU)) break;
        if (pressed(&was[FH1_KEY_A], FH1_KEY_A)) {
            if (st.state == FH1_PLAYER_STOPPED) music_play_current();
            else fh1_player_toggle();
        }
        if (pressed(&was[FH1_KEY_X], FH1_KEY_X)) {
            s_vis = (s_vis + 1) % VIS_COUNT;
            if (s_glow) memset(s_glow, 0, (W - 10) * (H - 109));
            fh1_settings_set_u8("music", "vis", (uint8_t)s_vis);
        }
        if (pressed(&was[FH1_KEY_Y], FH1_KEY_Y)) {
            set_scheme(s_scheme + 1);
            fh1_settings_set_u8("music", "scheme", (uint8_t)s_scheme);
        }
        if (pressed(&was[FH1_KEY_SELECT], FH1_KEY_SELECT)) music_set_mode((music_mode_t)((music_mode() + 1) % MUSIC_MODES));
        for (int v = 0; v < 2; v++) {
            const int key = v ? FH1_KEY_DOWN : FH1_KEY_UP;
            if (pressed(&was[key], key)) {
                int vol = k.volume + (v ? -5 : 5);
                k.volume = (uint8_t)(vol < 0 ? 0 : vol > 100 ? 100 : vol);
                fh1_audio_preview_volume(k.volume);
                k.volume_changed = true;
                k.vol_shown_until = now + 1500000;
            }
        }
        // Left/Right: a tap changes track (back: to the start first, after 3 s); held, they seek.
        for (int dir = 0; dir < 2; dir++) {
            const int key = dir ? FH1_KEY_RIGHT : FH1_KEY_LEFT;
            const bool down = fh1_input_is_down(key);
            if (down && !was[key]) {
                held_since[dir] = now;
                seeking[dir] = false;
            } else if (down && now - held_since[dir] > 450000 && now - last_seek > 200000) {
                fh1_player_seek(dir ? 5000 : -5000);
                seeking[dir] = true;
                last_seek = now;
            } else if (!down && was[key] && !seeking[dir]) {
                if (dir) music_step(1);
                else if (st.pos_ms > 3000) fh1_player_seek(-(int32_t)st.pos_ms);
                else music_step(-1);
            }
            was[key] = down;
        }

        uint32_t rate;
        const bool sound = analyse(s_d, &rate) && st.state != FH1_PLAYER_STOPPED;
        draw_panel(&k, &st, s_d);
        draw_vis(&k, &st, s_d, sound, rate);
        memcpy(s_fb, s_bb, W * H * 2);
        k.blink++;
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(FRAME_MS));
    }
    if (k.volume_changed) fh1_audio_set_master_volume(k.volume);      // saved once, on the way out
    heap_caps_free(s_bb);
    heap_caps_free(s_d);
    heap_caps_free(s_wf);
    heap_caps_free(s_glow);
    s_bb = NULL;
    s_d = NULL;
    s_wf = NULL;
    s_glow = NULL;
    s_task = NULL;
    fh1_ui_pause(false, true);
    fh1_ui_async(deck_closed_ui, NULL);
    vTaskDelete(NULL);
}

void music_deck_open(void)
{
    if (s_task) return;
    s_fb = fh1_board_framebuffer(&W, &H);
    // The visualiser's inner area (see draw_vis): w - 2 and h - 2 of the box.
    const int iw = W - 10, ih = H - 109;
    s_bb = heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM);
    s_d = heap_caps_calloc(1, sizeof(dsp_t), MALLOC_CAP_SPIRAM);
    s_wf_w = iw;
    s_wf_h = ih - ih * 35 / 100 - 2 - 12;
    s_wf_top = 0;
    s_wf = heap_caps_calloc(s_wf_w * s_wf_h, 2, MALLOC_CAP_SPIRAM);
    s_glow = heap_caps_calloc(iw * ih, 1, MALLOC_CAP_SPIRAM);
    if (!s_fb || !s_bb || !s_d || !s_wf || !s_glow) {
        heap_caps_free(s_bb);
        heap_caps_free(s_d);
        heap_caps_free(s_wf);
        heap_caps_free(s_glow);
        s_bb = NULL;
        s_d = NULL;
        s_wf = NULL;
        s_glow = NULL;
        fh1_ui_message(T("Music"), T("Not enough memory for the deck."));
        return;
    }
    dsp_init(s_d);
    s_vis = fh1_settings_get_u8("music", "vis", VIS_SPECTRUM) % VIS_COUNT;
    set_scheme(fh1_settings_get_u8("music", "scheme", 0));
    // The heat map's quietest colour is the background: an empty waterfall shows as such.
    for (int i = 0; i < s_wf_w * s_wf_h; i++) s_wf[i] = s_p.heat[0];
    fh1_ui_pause(true, false);
    if (xTaskCreatePinnedToCore(deck_task, "deck", 6144, NULL, 4, &s_task, 1) != pdPASS) {
        s_task = NULL;
        fh1_ui_pause(false, true);
        heap_caps_free(s_bb);
        heap_caps_free(s_d);
        heap_caps_free(s_wf);
        heap_caps_free(s_glow);
        s_bb = NULL;
        s_d = NULL;
        s_wf = NULL;
        s_glow = NULL;
        fh1_ui_message(T("Music"), T("Not enough memory for the deck."));
    }
}
