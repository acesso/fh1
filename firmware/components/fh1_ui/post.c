// The boot screen (fh1_post.h). 8 x 8 pixel font straight into the logical screen; costs nothing after boot
// (its line buffer is in PSRAM and freed at the end).
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "fh1_post.h"
#include "fh1_board.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

const uint8_t *fh1_font8_glyph(uint32_t cp);

#define MAX_LINES 40
#define LINE_LEN 48
#define C_BG 0x0000
#define C_LABEL 0x8C71               // grey
#define C_TEXT 0xE73C                // light grey
#define C_OK 0x47E8                  // green
#define C_WARN 0xFEA0                // amber
#define C_FAIL 0xF8A2                // red
#define C_HEAD 0x5DDF                // light blue

typedef struct {
    fh1_post_status_t st;
    char label[12];
    char text[LINE_LEN];
} line_t;

static line_t *s_lines;
static int s_n, s_steps, s_done;
static uint16_t *s_fb;
static int s_w, s_h, s_cols, s_rows;

static void put_char(int col, int row, uint32_t cp, uint16_t fg)
{
    const uint8_t *g = fh1_font8_glyph(cp);
    for (int y = 0; y < 8; y++) {
        uint16_t *p = s_fb + (row * 8 + y) * s_w + col * 8;
        for (int x = 0; x < 8; x++) p[x] = (g[y] & (0x80 >> x)) ? fg : C_BG;
    }
}

// UTF-8 text (Latin-1 range), clipped to the row's end. Returns the next column.
static int put_text(int col, int row, const char *s, uint16_t fg)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p && col < s_cols) {
        uint32_t cp = *p++;
        if ((cp & 0xE0) == 0xC0 && *p) cp = ((cp & 0x1F) << 6) | (*p++ & 0x3F);
        put_char(col++, row, cp, fg);
    }
    return col;
}

static const char *tag(fh1_post_status_t st, uint16_t *c)
{
    switch (st) {
    case FH1_POST_OK: *c = C_OK; return "OK";
    case FH1_POST_WARN: *c = C_WARN; return "--";
    case FH1_POST_OFF: *c = C_LABEL; return "OFF";
    case FH1_POST_FAIL: *c = C_FAIL; return "FAIL";
    default: *c = C_TEXT; return "";
    }
}

static void draw_line(int i)
{
    const int text_rows = s_rows - 2;                          // the bar below
    const int first = s_n > text_rows ? s_n - text_rows : 0;   // the screen scrolls when full
    if (i < first) return;
    int row = i - first;
    for (int c = 0; c < s_cols; c++) put_char(c, row, ' ', C_TEXT);
    const line_t *l = &s_lines[i];
    uint16_t tc;
    const char *t = tag(l->st, &tc);
    int col = 0;
    if (l->label[0]) {
        col = put_text(0, row, l->label, l->st == FH1_POST_INFO && !l->text[0] ? C_HEAD : C_LABEL);
        put_char(col++, row, ' ', C_TEXT);           // at least one space after a long name
        while (col < 11) put_char(col++, row, ' ', C_TEXT);
    }
    int room = s_cols - col - (t[0] ? (int)strlen(t) + 1 : 0);
    char clipped[LINE_LEN];
    snprintf(clipped, sizeof(clipped), "%.*s", room > 0 ? room : 0, l->text);
    put_text(col, row, clipped, l->st == FH1_POST_INFO ? C_HEAD : C_TEXT);
    if (t[0]) put_text(s_cols - (int)strlen(t), row, t, tc);
}

static void redraw_all(void)
{
    for (int i = 0; i < s_w * s_h; i++) s_fb[i] = C_BG;
    for (int i = 0; i < s_n; i++) draw_line(i);
}

static void draw_bar(void)
{
    if (!s_fb || !s_steps) return;
    const int y0 = (s_rows - 1) * 8 + 2, x0 = 8, w = s_w - 16;
    const int fill = w * (s_done > s_steps ? s_steps : s_done) / s_steps;
    for (int y = 0; y < 5; y++) {
        uint16_t *p = s_fb + (y0 + y) * s_w + x0;
        for (int x = 0; x < w; x++) {
            bool edge = y == 0 || y == 4 || x == 0 || x == w - 1;
            p[x] = edge ? C_LABEL : (x < fill ? C_OK : C_BG);
        }
    }
}

void fh1_post_begin(int steps)
{
    s_steps = steps;
    if (!s_lines) s_lines = heap_caps_calloc(MAX_LINES, sizeof(line_t), MALLOC_CAP_SPIRAM);
}

void fh1_post(fh1_post_status_t st, const char *label, const char *fmt, ...)
{
    if (!s_lines) return;
    if (s_n == MAX_LINES) {                    // keep the latest
        memmove(s_lines, s_lines + 1, (MAX_LINES - 1) * sizeof(line_t));
        s_n--;
    }
    line_t *l = &s_lines[s_n++];
    l->st = st;
    snprintf(l->label, sizeof(l->label), "%s", label ? label : "");
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(l->text, sizeof(l->text), fmt, ap);
    va_end(ap);
    if (!s_fb) return;
    if (s_n > s_rows - 2) redraw_all();        // scrolled
    else draw_line(s_n - 1);
    draw_bar();
}

void fh1_post_step(void)
{
    s_done++;
    draw_bar();
}

void fh1_post_display_ready(void)
{
    s_fb = fh1_board_framebuffer(&s_w, &s_h);
    if (!s_fb) return;
    s_cols = s_w / 8;
    s_rows = s_h / 8;
    redraw_all();
    draw_bar();
}

void fh1_post_end(int hold_ms)
{
    s_done = s_steps;
    draw_bar();
    if (s_fb && hold_ms > 0) vTaskDelay(pdMS_TO_TICKS(hold_ms));
    heap_caps_free(s_lines);
    s_lines = NULL;
    s_fb = NULL;
}
