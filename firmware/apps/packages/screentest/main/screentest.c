// Teste de tela: the screen's picture and speed, on any FH1 board (tools/fh1bench.py runs it from a computer).
//
// Patterns (A: the next one, B: the one before, Start or Menu: leave):
//   card    the test card, still: colour bars, four ramps (red, green, blue, grey: banding or missing bits show),
//           1-pixel lines and a checker (a shifted or dropped bit shows there), a border on the screen's edge
//   motion  every pixel changes every frame (diagonal stripes sliding): the worst case for every link
//   sprites 16 boxes bouncing over the card: small changed areas, the common case in games
//   half    the top half scrolling, the bottom still: a scrolling playfield under a still panel
//   tear    white bars sweeping across a dark screen, a screen's width in about half a second: where a frame reached
//           the panel while it was redrawing itself, a bar shows a step (tearing; film it at 240 fps to see)
// A line at the bottom shows the pattern, the frames a second the app reaches and its drawing time.
//
// The app asks for frames every 10 ms (the firmware's shortest period), so the rate it reaches is the board's own
// ceiling for that pattern: drawing, plus the copy of the changed rows to the screen. What the panel shows depends
// on its link too (a panel with its own memory gets the changes at most 30 times a second; one without is refreshed
// at its fixed rate): the dev console's "disp" numbers tell that side.
//
// Development commands: "mode NAME" shows a pattern; "bench" runs motion, sprites, half and card for 10 s each and
// logs a line for each ("screentest: motion: 58.3 fps, draw 6.2 ms (max 9), longest gap 31 ms, 320x240"), then
// "screentest: bench done".
#include "fh1_sdk.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define RGB(r, g, b) (uint16_t)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | ((b) >> 3))

enum { M_CARD, M_MOTION, M_SPRITES, M_HALF, M_TEAR, MODES };
static const char *const s_names[MODES] = { "card", "motion", "sprites", "half", "tear" };

static int s_mode = M_CARD;
static uint32_t s_t;                 // ms since the pattern started
static uint32_t s_keys_prev;
static uint16_t *s_card;             // the card, drawn once (the sprites run over it)
static int s_cw, s_ch;

// Numbers for the line at the bottom and for the bench (over a window).
static uint32_t s_win_t, s_win_frames, s_win_draw, s_win_draw_max, s_win_gap;
static char s_line[64];

// The bench: each pattern 10 s (the first second not counted: it settles).
static const int s_bench_order[] = { M_MOTION, M_SPRITES, M_HALF, M_CARD };
static int s_bench = -1;             // the step under way, -1: none
static uint32_t s_bench_t, s_b_frames, s_b_draw, s_b_draw_max, s_b_gap, s_b_ms;

static void rect(uint16_t *px, int w, int h, int x, int y, int rw, int rh, uint16_t c)
{
    for (int j = y < 0 ? 0 : y; j < y + rh && j < h; j++)
        for (int i = x < 0 ? 0 : x; i < x + rw && i < w; i++) px[j * w + i] = c;
}

// The test card, sized to the screen: bars on the top quarter, four ramps, then lines and a checker, a border.
static void draw_card(uint16_t *px, int w, int h)
{
    static const uint16_t bars[8] = { RGB(255, 255, 255), RGB(255, 255, 0), RGB(0, 255, 255), RGB(0, 255, 0),
                                      RGB(255, 0, 255), RGB(255, 0, 0), RGB(0, 0, 255), RGB(0, 0, 0) };
    const int bh = h / 4;
    for (int i = 0; i < 8; i++) rect(px, w, h, i * w / 8, 0, (i + 1) * w / 8 - i * w / 8, bh, bars[i]);
    const int rh = h / 12, y0 = bh;
    for (int x = 0; x < w; x++) {
        const int v = x * 255 / (w - 1);
        const uint16_t c[4] = { RGB(v, 0, 0), RGB(0, v, 0), RGB(0, 0, v), RGB(v, v, v) };
        for (int k = 0; k < 4; k++)
            for (int y = y0 + k * rh; y < y0 + (k + 1) * rh; y++) px[y * w + x] = c[k];
    }
    const int y1 = y0 + 4 * rh;
    rect(px, w, h, 0, y1, w, h - y1, RGB(24, 24, 32));
    // 1-pixel lines, alternating, on the left half; a checker of 1-pixel squares on the right
    for (int y = y1 + 8; y < h - 30; y++)
        for (int x = 8; x < w - 8; x++) {
            const bool on = x < w / 2 ? ((y - y1) & 1) == 0 : ((x + y) & 1) == 0;
            px[y * w + x] = on ? RGB(255, 255, 255) : RGB(0, 0, 0);
        }
    for (int x = 0; x < w; x++) px[x] = px[(h - 1) * w + x] = RGB(255, 0, 0);   // the border, on the edge
    for (int y = 0; y < h; y++) px[y * w] = px[y * w + w - 1] = RGB(255, 0, 0);
    char l[40];
    snprintf(l, sizeof(l), "%dx%d", w, h);
    fh1_px_text(px, w, h, 10, h - 24, l, 0xffffff, 1);
}

// Every pixel changes every frame: diagonal stripes in four colours sliding by 3 pixels a frame.
static void draw_motion(uint16_t *px, int w, int h, uint32_t f)
{
    static const uint16_t c[4] = { RGB(230, 60, 60), RGB(60, 200, 90), RGB(70, 110, 240), RGB(240, 210, 60) };
    const int off = (int)(f * 3);
    for (int y = 0; y < h; y++) {
        uint16_t *row = px + y * w;
        int k = y + off;
        for (int x = 0; x < w; x++, k++) row[x] = c[(k >> 4) & 3];
    }
}

// 16 boxes (24 x 24) bouncing over the card: only their old and new places change.
static void draw_sprites(uint16_t *px, int w, int h, uint32_t t)
{
    memcpy(px, s_card, (size_t)w * h * 2);
    for (int i = 0; i < 16; i++) {
        const int sx = 37 + i * 13, sy = 23 + i * 17;               // pixels a second
        const int rw = w - 24, rh = h - 24;
        int x = (int)((t * (uint32_t)sx / 1000 + i * 41) % (uint32_t)(2 * rw)), y = (int)((t * (uint32_t)sy / 1000 + i * 29) % (uint32_t)(2 * rh));
        if (x >= rw) x = 2 * rw - x;
        if (y >= rh) y = 2 * rh - y;
        rect(px, w, h, x, y, 24, 24, RGB(40 + i * 13, 255 - i * 12, 120 + i * 8));
    }
}

// The top half scrolls (stripes moving down), the bottom half is the card's.
// Tearing: three white bars, 6 pixels wide, sweeping right over dark grey with thin horizontal guide lines (a step in
// a bar between two guides is easy to see), by time: the same speed at any frame rate.
static void draw_tear(uint16_t *px, int w, int h, uint32_t t)
{
    for (int y = 0; y < h; y++) {
        const uint16_t bg = (y % 24) == 0 ? RGB(70, 70, 90) : RGB(20, 20, 28);
        uint16_t *row = px + y * w;
        for (int x = 0; x < w; x++) row[x] = bg;
    }
    const int span = w + 40, x0 = (int)(t * (uint32_t)w * 2 / 1000 % (uint32_t)span);
    for (int b = 0; b < 3; b++) {
        const int x = (x0 + b * span / 3) % span - 20;
        rect(px, w, h, x, 0, 6, h, RGB(255, 255, 255));
    }
}

static void draw_half(uint16_t *px, int w, int h, uint32_t f)
{
    memcpy(px + (size_t)(h / 2) * w, s_card + (size_t)(h / 2) * w, (size_t)w * (h - h / 2) * 2);
    for (int y = 0; y < h / 2; y++) {
        const uint16_t c = ((y + (int)f * 2) >> 3) & 1 ? RGB(40, 40, 70) : RGB(90, 150, 220);
        uint16_t *row = px + y * w;
        for (int x = 0; x < w; x++) row[x] = c;
    }
}

static void set_mode(int m)
{
    s_mode = (m % MODES + MODES) % MODES;
    s_t = 0;
    s_win_t = s_win_frames = s_win_draw = s_win_draw_max = s_win_gap = 0;
}

static void bench_step(int i)
{
    s_bench = i;
    s_bench_t = 0;
    s_b_frames = s_b_draw = s_b_draw_max = s_b_gap = s_b_ms = 0;
    set_mode(s_bench_order[i]);
}

static bool command(const char *line, int w, int h)
{
    char name[16];
    if (sscanf(line, "mode %15s", name) == 1) {
        for (int i = 0; i < MODES; i++)
            if (!strcmp(name, s_names[i])) set_mode(i);
        s_bench = -1;
        return true;
    }
    if (!strncmp(line, "bench", 5)) {
        fh1_log("screentest: bench, %dx%d, 10 s a pattern", w, h);
        bench_step(0);
        return true;
    }
    return false;
}

static int frame(uint16_t *px, int w, int h, uint32_t dt, void *user)
{
    (void)user;
    if (!s_card || s_cw != w || s_ch != h) {         // the card, once the screen's size is known (PSRAM: >= 4160 B)
        free(s_card);
        s_card = malloc((size_t)w * h * 2);
        if (!s_card) return FH1_FRAME_END;
        s_cw = w, s_ch = h;
        draw_card(s_card, w, h);
    }
    char cmd[48];
    while (fh1_dev_command(cmd, sizeof(cmd)))
        if (!command(cmd, w, h)) fh1_log("screentest: not understood: %s", cmd);
    uint32_t keys = 0;
    for (int k = 0; k <= FH1_KEY_MENU; k++)
        if (fh1_input_is_down(k)) keys |= 1u << k;
    const uint32_t edge = keys & ~s_keys_prev;
    s_keys_prev = keys;
    if (edge & ((1u << FH1_KEY_START) | (1u << FH1_KEY_MENU))) return FH1_FRAME_END;
    if (edge & (1u << FH1_KEY_A)) set_mode(s_mode + 1), s_bench = -1;
    if (edge & (1u << FH1_KEY_B)) set_mode(s_mode - 1), s_bench = -1;

    const uint32_t t0 = fh1_millis();
    const uint32_t f = s_t / 16;     // motion by time (the same speed at any frame rate)
    switch (s_mode) {
    case M_CARD: memcpy(px, s_card, (size_t)w * h * 2); break;
    case M_MOTION: draw_motion(px, w, h, f); break;
    case M_SPRITES: draw_sprites(px, w, h, s_t); break;
    case M_HALF: draw_half(px, w, h, f); break;
    case M_TEAR: draw_tear(px, w, h, s_t); break;
    }
    const uint32_t draw = fh1_millis() - t0;
    s_t += dt;

    // The window for the line at the bottom: 1 s.
    s_win_frames++;
    s_win_draw += draw;
    if (draw > s_win_draw_max) s_win_draw_max = draw;
    if (dt > s_win_gap) s_win_gap = dt;
    if ((s_win_t += dt) >= 1000) {
        const unsigned fps10 = s_win_frames * 10000u / s_win_t, draw10 = s_win_draw * 10u / s_win_frames;
        snprintf(s_line, sizeof(s_line), "%s  %u.%u fps  draw %u.%u ms (max %u)  gap %u ms", s_names[s_mode], fps10 / 10,
                 fps10 % 10, draw10 / 10, draw10 % 10, (unsigned)s_win_draw_max, (unsigned)s_win_gap);
        s_win_t = s_win_frames = s_win_draw = s_win_draw_max = s_win_gap = 0;
    }
    if (s_line[0]) {
        rect(px, w, h, 0, h - 12, w, 12, RGB(0, 0, 0));
        fh1_px_text(px, w, h, 4, h - 10, s_line, 0xffffff, 1);
    }

    // The bench: 1 s to settle, then 10 s counted.
    if (s_bench >= 0) {
        s_bench_t += dt;
        if (s_bench_t > 1000) {
            s_b_frames++;
            s_b_draw += draw;
            s_b_ms += dt;
            if (draw > s_b_draw_max) s_b_draw_max = draw;
            if (dt > s_b_gap) s_b_gap = dt;
        }
        if (s_bench_t >= 11000) {
            // (whole numbers in tenths: a float through printf needs double helpers the firmware does not export)
            const unsigned fps10 = s_b_frames * 10000u / s_b_ms, draw10 = s_b_draw * 10u / s_b_frames;
            fh1_log("screentest: %s: %u.%u fps, draw %u.%u ms (max %u), longest gap %u ms, %dx%d", s_names[s_mode],
                    fps10 / 10, fps10 % 10, draw10 / 10, draw10 % 10, (unsigned)s_b_draw_max, (unsigned)s_b_gap, w, h);
            if (s_bench + 1 < (int)(sizeof(s_bench_order) / sizeof(s_bench_order[0]))) bench_step(s_bench + 1);
            else {
                s_bench = -1;
                fh1_log("screentest: bench done");
            }
        }
    }
    return FH1_FRAME_SHOW;
}

static void closed(void *user)
{
    (void)user;
    free(s_card);
    s_card = NULL;
    fh1_app_exit();
}

int app_main(int argc, char *argv[])
{
    (void)argc, (void)argv;
    set_mode(M_CARD);
    for (int k = 0; k <= FH1_KEY_MENU; k++)
        if (fh1_input_is_down(k)) s_keys_prev |= 1u << k;
    if (!fh1_screen_open(10, frame, closed, NULL)) {
        fh1_ui_message("Teste de tela", "Não foi possível usar a tela.");
    }
    return 0;
}
