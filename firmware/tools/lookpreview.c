// Renders a look (components/fh1_ui/ui_look.c) on a computer, without the firmware: a mock 320 x 240 screen with the
// desktop, a window out of focus, one in focus with its mote's path sampled, and an alert, at 3x; and the opening
// as a strip of six frames. Text is drawn as bars. PPM files (convert them with any image tool).
//
//   cc -O2 -Wall -I components/fh1_ui tools/lookpreview.c components/fh1_ui/ui_look.c -o lookpreview
//   ./lookpreview 2 [phase]      (2 Garden, 3 Command: the LOOK_* number)
//
// The palettes below copy the themes' colours in ui.c (s_themes); keep them in step.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui_look.h"
#define SW 320
#define SH 240
static uint32_t scr[SH * SW];
static void blend(int x, int y, uint32_t argb)
{
    if (x < 0 || y < 0 || x >= SW || y >= SH || !(argb >> 24)) return;
    const unsigned a = argb >> 24, d = scr[y * SW + x];
    uint32_t o = 0;
    for (int s = 0; s < 24; s += 8) o |= ((((argb >> s) & 255) * a + ((d >> s) & 255) * (255 - a)) / 255) << s;
    scr[y * SW + x] = o;
}
static void blit(const uint32_t *px, int w, int h, int x0, int y0) { for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) blend(x0 + x, y0 + y, px[y * w + x]); }
static void rect(int x0, int y0, int w, int h, uint32_t c) { for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) blend(x0 + x, y0 + y, 0xff000000 | c); }
static void save(const char *f, int W, int H, const uint32_t *b, int k)
{
    FILE *o = fopen(f, "wb"); fprintf(o, "P6 %d %d 255\n", W * k, H * k);
    for (int y = 0; y < H * k; y++) for (int x = 0; x < W * k; x++) { uint32_t c = b[(y / k) * W + x / k]; fputc(c >> 16, o); fputc((c >> 8) & 255, o); fputc(c & 255, o); }
    fclose(o);
}
static look_pal_t P;
static int LOOK;
static void window(int x, int y, int w, int h, int state, int grow, int phase, int title_w, int items)
{
    const int u = P.u, O = look_strip_out(LOOK) * u, T = look_strip_thick(LOOK) * u + O, cw = w - 4 * u;
    rect(x, y, w, h, P.paper);
    static uint32_t buf[400 * 400];
    // strips: top, bottom (w x T), left, right (T x h)
    look_strip(LOOK, &P, 0, w, w, h, state, grow, phase, buf); blit(buf, w, T, x, y - O);
    look_strip(LOOK, &P, 1, w, w, h, state, grow, phase, buf);
    // the bottom strip is drawn with its outer edge at row 0: flip it
    { static uint32_t f[400 * 16]; for (int r = 0; r < T; r++) memcpy(f + (T - 1 - r) * w, buf + r * w, w * 4); blit(f, w, T, x, y + h - T + O); }
    look_strip(LOOK, &P, 2, h, w, h, state, grow, phase, buf); blit(buf, T, h, x - O, y);
    look_strip(LOOK, &P, 3, h, w, h, state, grow, phase, buf);
    { static uint32_t f[16 * 400]; for (int a = 0; a < h; a++) for (int r = 0; r < T; r++) f[a * T + (T - 1 - r)] = buf[a * T + r]; blit(f, T, h, x + w - T + O, y); }
    // title
    int tw = title_w, tx, ty, pw, ph, px_, py_;
    look_title_box(LOOK, u, cw, tw, 8 * u, &pw, &ph, &px_, &py_, &tx, &ty);
    look_title(LOOK, &P, pw, ph, tw, state, phase, buf);
    blit(buf, pw, ph, x + 2 * u + px_, y + 2 * u + py_);
    rect(x + 2 * u + tx, y + 2 * u + ty + 2 * u, tw, 5 * u, look_title_ink(LOOK, &P, state));
    // corners
    const int S = look_corner_size(LOOK) * u, o = look_corner_out(LOOK) * u;
    for (int i = 0; i < 4; i++) {
        look_corner(LOOK, &P, i, state, grow, phase, buf);
        blit(buf, S, S, (i & 1) ? x + w - S + o : x - o, (i & 2) ? y + h - S + o : y - o);
    }
    // items: a cursor and bars of text
    int cw_, ch_;
    if (look_cursor(LOOK, &P, state != 1, buf, &cw_, &ch_)) blit(buf, cw_, ch_, x + 3 * u + u, y + 11 * u);
    if (state == 1) {
        static uint32_t m[16 * 16]; int mx, my, mf, mw, mh;
        for (uint32_t t = 0; t < 30000; t += 2700)
            if (look_mote_at(LOOK, u, w, h, t, &mx, &my, &mf) && look_mote(LOOK, &P, mf, m, &mw, &mh)) blit(m, mw, mh, x + mx, y + my);
    }
    for (int k = 0; k < items; k++) rect(x + 13 * u, y + 12 * u + k * 11 * u, 40 + (k * 37) % 60, 5 * u, k == 0 && state == 1 ? look_focus_text(LOOK, &P) : P.text);
}
int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <look: 2 garden, 3 command> [phase]\n", argv[0]);
        return 1;
    }
    LOOK = atoi(argv[1]);
    if (LOOK == LOOK_GARDEN) P = (look_pal_t){ 1, 0xf3ead2, 0x3b2a1a, 0x9c8a6e, 0xc0392b, 0x3b2a1a, 0xc9b48c, 0x4e8a3a, 0x3f7530 };
    else P = (look_pal_t){ 1, 0x07141c, 0x3fd8ff, 0x1d5a6b, 0xff4a3a, 0xd6f4ff, 0x0e3442, 0x050c12, 0x0d2430 };
    const int phase = argc > 2 ? atoi(argv[2]) : 0;
    uint16_t tile[96 * 96]; int tw, th;
    look_desk(LOOK, &P, tile, &tw, &th);
    for (int y = 0; y < SH; y++) for (int x = 0; x < SW; x++) { uint16_t c = tile[(y % th) * tw + x % tw]; scr[y * SW + x] = ((c >> 11) * 255 / 31) << 16 | (((c >> 5) & 63) * 255 / 63) << 8 | (c & 31) * 255 / 31; }
    window(14, 26, 210, 150, 0, 256, phase, 36, 4);
    window(44, 50, 220, 160, 1, 256, phase, 52, 5);
    window(150, 120, 150, 80, 2, 256, phase, 44, 2);
    char f[64]; snprintf(f, sizeof f, "look%d-p%d.ppm", LOOK, phase); save(f, SW, SH, scr, 3);
    // the opening, 6 frames
    static uint32_t strip[SH * SW * 6];
    for (int k = 0; k < 6; k++) {
        for (int y = 0; y < SH; y++) for (int x = 0; x < SW; x++) { uint16_t c = tile[(y % th) * tw + x % tw]; scr[y * SW + x] = ((c >> 11) * 255 / 31) << 16 | (((c >> 5) & 63) * 255 / 63) << 8 | (c & 31) * 255 / 31; }
        window(40, 40, 200, 140, 1, k * 256 / 5, 0, 52, 4);
        for (int y = 0; y < SH; y++) memcpy(strip + y * SW * 6 + k * SW, scr + y * SW, SW * 4);
    }
    snprintf(f, sizeof f, "look%d-open.ppm", LOOK); save(f, SW * 6, SH, strip, 1);
    return 0;
}
