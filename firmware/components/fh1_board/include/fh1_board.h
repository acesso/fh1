// The board layer: brings up the display and declares the devices the board may have.
// Everything above it uses the device manager and the 320 x 240 logical screen below.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    const char *id, *name;     // "native", "Native 480x480"
    int w, h;                  // the UI's resolution in this mode
    int ui_scale;              // 1: 8-pixel font and spacing; 2: everything doubled
    int num, den;              // shown at num/den size on the panel, centred
} fh1_display_mode_t;

const char *fh1_board_name(void);
esp_err_t fh1_board_init(void);                 // display, backlight, buses; registers candidate devices

void fh1_board_backlight(uint8_t pct);           // 0..100, saved
void fh1_board_backlight_preview(uint8_t pct);   // 0..100, applied but not saved (a slider being moved)
uint8_t fh1_board_backlight_level(void);

// Screen modes: [0] is the panel's native resolution. The mode is read from settings at init
// ("display"/"mode"); changing it takes a restart.
int fh1_board_display_modes(const fh1_display_mode_t **modes);
const fh1_display_mode_t *fh1_board_display_mode(void);
void fh1_board_display_set_mode(const char *id);

// The screen as the UI sees it (RGB565, the mode's logical size). Full-screen programs (the emulator)
// may draw into it directly; the screenshot reads it.
uint16_t *fh1_board_framebuffer(int *w, int *h);
// Whether a full-screen program draws into the framebuffer itself (true) or every change comes through
// fh1_board_flush or fh1_board_changed (false, the start: the UI, the POST screen). A panel fed over SPI then sends
// only what changed; one that scans the framebuffer by itself ignores both. fh1_ui_pause switches it.
void fh1_board_direct(bool on);
// Rows y1..y2 of the framebuffer were drawn directly (the POST screen).
void fh1_board_changed(int y1, int y2);

// How well the panel is being fed (for the dev console and the stress test).
typedef struct {
    uint32_t frames;         // panel frames sent
    uint32_t fills;          // line bands prepared
    uint32_t late_fills;     // bands that took longer than the DMA's sending time: likely visible
    uint32_t odd_frames;     // frames that did not get exactly their bands
    uint32_t max_fill_us;
    uint32_t late_starts;    // refills that started late (interrupt held up)
    uint32_t max_gap_us;     // longest time between two refills in a frame
    uint32_t vsync_min_us, vsync_max_us;   // frame period range
} fh1_display_stats_t;
void fh1_board_display_stats(fh1_display_stats_t *out, bool reset);

// Copy a rectangle of the UI's screen (RGB565, row-major, inclusive bounds) to the panel.
void fh1_board_flush(int x1, int y1, int x2, int y2, const uint16_t *px);
