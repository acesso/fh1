#include "ui.h"
#include <string.h>
#include "fh1_sdk.h"

#define BG 0x10a5                    // the boxes' navy, mixed 3/4 over the scene
#define EDGE 0xce79
#define SHADE 0x4a69

static inline uint16_t mix34(uint16_t under, uint16_t c)
{
    return ((under >> 2) & 0x39E7) + ((c >> 1) & 0x7BEF) + ((c >> 2) & 0x39E7);
}

void ui_box(const ui_t *u, int x, int y, int bw, int bh)
{
    for (int j = 0; j < bh; j++) {
        const int yy = y + j;
        if (yy < 0 || yy >= u->h) continue;
        uint16_t *line = u->px + yy * u->w;
        for (int i = 0; i < bw; i++) {
            const int xx = x + i;
            if (xx < 0 || xx >= u->w) continue;
            const bool corner = (i == 0 || i == bw - 1) && (j == 0 || j == bh - 1);
            if (corner) continue;
            const bool edge = i == 0 || j == 0 || i == bw - 1 || j == bh - 1;
            const bool inner = i == 1 || j == 1 || i == bw - 2 || j == bh - 2;
            line[xx] = edge ? EDGE : inner ? SHADE : mix34(line[xx], BG);
        }
    }
}

void ui_text(const ui_t *u, int x, int y, const char *s, uint32_t rgb)
{
    fh1_px_text(u->px, u->w, u->h, x + 1, y + 1, s, 0x000000, 1);      // a drop shadow
    fh1_px_text(u->px, u->w, u->h, x, y, s, rgb, 1);
}

// The wave: each letter rises up to 2 pixels as it passes, 70 ms after the one before, once a second.
void ui_text_wave(const ui_t *u, int x, int y, const char *s, uint32_t rgb, uint32_t now_ms)
{
    const int t = (int)(now_ms % 1000);
    const unsigned char *p = (const unsigned char *)s;
    for (int i = 0; *p; i++) {
        char one[5] = { 0 };
        int n = (*p & 0xE0) == 0xC0 ? 2 : (*p & 0xF0) == 0xE0 ? 3 : 1;
        for (int k = 0; k < n && p[k]; k++) one[k] = (char)p[k];
        p += n;
        const int d = t - i * 70, lift = d > -110 && d < 110 ? (110 - (d < 0 ? -d : d)) * 2 / 110 : 0;
        ui_text(u, x + i * 8, y - lift, one, rgb);
    }
}

static int key_token(const char *s, int *len)
{
    static const struct { const char *t; int key; } tok[] = {
        { "{A}", FH1_KEY_A }, { "{B}", FH1_KEY_B }, { "{X}", FH1_KEY_X }, { "{Y}", FH1_KEY_Y }, { "{S}", FH1_KEY_START },
        { "{s}", FH1_KEY_SELECT }, { "{M}", FH1_KEY_MENU }, { "{LR}", FH1_HINT_LEFTRIGHT }, { "{UD}", FH1_HINT_UPDOWN },
    };
    for (size_t i = 0; i < sizeof(tok) / sizeof(tok[0]); i++) {
        const size_t n = strlen(tok[i].t);
        if (!strncmp(s, tok[i].t, n)) {
            *len = (int)n;
            return tok[i].key;
        }
    }
    return -1;
}

static int hints(const ui_t *u, int x, int y, const char *s, uint32_t rgb)
{
    static uint16_t scratch[24 * 12];                   // measuring: icons drawn off to the side
    char word[64];
    int wn = 0;
    while (1) {
        int len = 0, key = *s == '{' ? key_token(s, &len) : -1;
        if (key >= 0 || !*s) {
            word[wn] = 0;
            if (wn) {
                if (u) ui_text(u, x, y, word, rgb);
                for (const unsigned char *p = (const unsigned char *)word; *p; p++)
                    if ((*p & 0xC0) != 0x80) x += 8;
                wn = 0;
            }
            if (!*s) return x;
            x = u ? fh1_px_key(u->px, u->w, u->h, x, y - 2, key) + 2 : x + fh1_px_key(scratch, 24, 12, 0, 0, key) + 2;
            s += len;
            continue;
        }
        if (wn < (int)sizeof(word) - 1) word[wn++] = *s;
        s++;
    }
}
int ui_hints(const ui_t *u, int x, int y, const char *s, uint32_t rgb) { return hints(u, x, y, s, rgb); }
int ui_hints_width(const char *s) { return hints(NULL, 0, 0, s, 0); }

void ui_bar(const ui_t *u, int x, int y, int bw, int value, int max, uint32_t rgb)
{
    const uint16_t c = (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
    const int fill = max > 0 ? (value < 0 ? 0 : value > max ? bw : value * bw / max) : 0;
    for (int j = 0; j < 4; j++) {
        const int yy = y + j;
        if (yy < 0 || yy >= u->h) continue;
        for (int i = 0; i < bw; i++) {
            const int xx = x + i;
            if (xx >= 0 && xx < u->w) u->px[yy * u->w + xx] = j == 0 || j == 3 ? SHADE : i < fill ? c : 0x2104;
        }
    }
}

static int len8(const char *s)
{
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if ((*p & 0xC0) != 0x80) n++;               // UTF-8: count letters, not bytes
    return n * 8;
}

void ui_menu_size(const ui_menu_t *m, int *bw, int *bh)
{
    int w = m->title ? len8(m->title) : 0;
    for (int i = 0; i < m->n; i++) {
        const int iw = len8(m->items[i]) + (m->notes[i] ? len8(m->notes[i]) + 12 : 0);
        if (iw > w) w = iw;
    }
    *bw = w + 24;
    *bh = m->n * 11 + (m->title ? 13 : 0) + 9;
}

void ui_menu_draw(const ui_t *u, const ui_menu_t *m, int x, int y, uint32_t now_ms)
{
    int bw, bh;
    ui_menu_size(m, &bw, &bh);
    ui_box(u, x, y, bw, bh);
    int ty = y + 5;
    if (m->title) {
        ui_text(u, x + 6, ty, m->title, 0xf8d870);
        ty += 13;
    }
    for (int i = 0; i < m->n; i++, ty += 11) {
        const bool off = m->off & (1u << i);
        const uint32_t col = off ? 0x707890 : m->colors[i] ? m->colors[i] : 0xf0f0f0;
        if (i == m->sel) {
            ui_text(u, x + 5 + (int)((now_ms / 200) % 2), ty, ">", 0xffffff);
            ui_text_wave(u, x + 15, ty, m->items[i], col, now_ms);   // the chosen line waves; its note stays
        } else ui_text(u, x + 15, ty, m->items[i], col);
        if (m->notes[i])
            ui_text(u, x + bw - 6 - len8(m->notes[i]), ty, m->notes[i], off ? 0x707890 : m->note_colors[i] ? m->note_colors[i] : 0x9fd8ff);
    }
}

void (*ui_sound)(int what);
static void sound(int what) { if (ui_sound) ui_sound(what); }

int ui_menu_key(ui_menu_t *m, int key)
{
    if (!m->n) {
        if (key == FH1_KEY_B) sound(UI_SND_BACK);
        return key == FH1_KEY_B ? UI_BACK : UI_NONE;
    }
    switch (key) {
    case FH1_KEY_UP: m->sel = (m->sel + m->n - 1) % m->n; sound(UI_SND_MOVE); return UI_NONE;
    case FH1_KEY_DOWN: m->sel = (m->sel + 1) % m->n; sound(UI_SND_MOVE); return UI_NONE;
    case FH1_KEY_A:
        sound((m->off & (1u << m->sel)) ? UI_SND_NO : UI_SND_OK);
        return (m->off & (1u << m->sel)) ? UI_NONE : m->sel;
    case FH1_KEY_B: sound(UI_SND_BACK); return UI_BACK;
    default: return UI_NONE;
    }
}

void ui_banner(const ui_t *u, const char *s, uint32_t rgb)
{
    const int bw = len8(s) * 2 + 24, bh = 30;
    const int x = (u->w - bw) / 2, y = u->h / 2 - 40;
    ui_box(u, x, y, bw, bh);
    fh1_px_text(u->px, u->w, u->h, x + 13, y + 8, s, 0x000000, 2);
    fh1_px_text(u->px, u->w, u->h, x + 12, y + 7, s, rgb, 2);
}
