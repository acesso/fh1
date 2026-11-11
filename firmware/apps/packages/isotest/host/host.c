// Táticas da Ilha on the PC, for checking its looks without an FH1: the SDK stubbed (the FH1's own 8 x 8 font
// for text), the game driven by a script, screenshots saved as BMP files.
//
//   make -C host && host/run game.script out/      (from the package's folder; see host/Makefile)
//
// Script lines: key A|B|X|Y|UP|DOWN|LEFT|RIGHT|START|SELECT|MENU, hold KEY FRAMES, wait FRAMES, shot NAME,
// and in a battle: place UNIT C R DIR, act UNIT KIND WHAT TARGET (kind 0 attack, 1 spell, 2 item), counter UNIT,
// target UNIT KIND WHAT C R, gear UNIT WEAPON ARMOUR, ranged UNIT RANGE, cam X Y (holds the camera), xp SLOT N (XP the
// battle will give), win, lose, log. A frame is 33 ms. Lines
// starting with # are comments. The frame buffer has guard zones around it: a frame that writes outside it stops
// the run (on the FH1 that write would corrupt memory and crash later, somewhere else).
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game.h"

#include "font.inc"                      // glyph_bitmap[], from firmware/components/fh1_ui/fh1_font8.c
static const uint8_t *glyph(uint32_t cp)
{
    if (cp >= 0x20 && cp <= 0x7E) return &glyph_bitmap[(cp - 0x20) * 8];
    if (cp >= 0xA0 && cp <= 0xFF) return &glyph_bitmap[(95 + cp - 0xA0) * 8];
    return &glyph_bitmap[0];
}

static uint32_t s_ms, s_keys;
static fh1_frame_fn s_frame;
static void *s_user;
static const char *s_dir = ".";

uint32_t fh1_millis(void) { return s_ms; }
bool fh1_input_is_down(int k) { return s_keys & (1u << k); }
int fh1_px_text(uint16_t *px, int w, int h, int x, int y, const char *text, uint32_t rgb, int scale)
{
    const uint16_t c = (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
    if (scale < 1) scale = 1;
    for (const unsigned char *p = (const unsigned char *)text; *p && x < w;) {
        uint32_t cp = *p++;
        if ((cp & 0xE0) == 0xC0 && *p) cp = ((cp & 0x1F) << 6) | (*p++ & 0x3F);
        const uint8_t *g = glyph(cp);
        for (int gy = 0; gy < 8 * scale; gy++)
            for (int gx = 0; gx < 8 * scale; gx++)
                if (g[gy / scale] & (0x80 >> (gx / scale)) && x + gx >= 0 && x + gx < w && y + gy >= 0 && y + gy < h)
                    px[(y + gy) * w + x + gx] = c;
        x += 8 * scale;
    }
    return x;
}
// Player 1's user (host command "user NAME") and a save per user, as the FH1 keeps them (SDK 0.8).
static char s_player[24] = "surya";
static struct { char user[24]; char data[16384]; int len; } s_saves[4];
static int save_slot(void)
{
    for (int i = 0; i < 4; i++)
        if (!strcmp(s_saves[i].user, s_player)) return i;
    for (int i = 0; i < 4; i++)
        if (!s_saves[i].user[0]) {
            strcpy(s_saves[i].user, s_player);
            s_saves[i].len = -1;
            return i;
        }
    return 0;
}
bool fh1_user(int player, char *name, size_t n) { if (player) return false; snprintf(name, n, "%s", s_player); return true; }
int fh1_state_save(const char *n, const void *d, size_t l)
{
    const int k = save_slot();
    if (l > sizeof(s_saves[k].data)) return -1;
    memcpy(s_saves[k].data, d, l);
    s_saves[k].len = (int)l;
    return 0;
}
int fh1_state_load(const char *n, void *d, size_t m)
{
    const int k = save_slot();
    if (s_saves[k].len < 0 || (size_t)s_saves[k].len > m) return -1;
    memcpy(d, s_saves[k].data, s_saves[k].len);
    return s_saves[k].len;
}
bool fh1_pad(int player, fh1_pad_t *out) { (void)player; (void)out; return false; }   // no controllers on the PC
// The controller's style (host command "pad fh1|xbox|nintendo|ps") and its key icons, drawn simply.
static int s_style;
int fh1_pad_style(void) { return s_style; }
int fh1_px_key(uint16_t *px, int w, int h, int x, int y, int key)
{
    static const uint32_t caps[4] = { 0xe5302c, 0xf4c21f, 0x2f6fe8, 0x2fb36a };
    static const char *const names[4][4] = { { "", "", "", "" }, { "B", "A", "Y", "X" }, { "A", "B", "X", "Y" }, { "O", "x", "^", "#" } };
    static const uint32_t ink[4][4] = { { 0 }, { 0xe5302c, 0x5cc23a, 0xf4c21f, 0x3a78e0 }, { 0xffffff, 0xffffff, 0xffffff, 0xffffff },
                                        { 0xff5a5a, 0x5a8cff, 0x3cd4a0, 0xf07cc0 } };
    const bool face = key >= FH1_KEY_A && key <= FH1_KEY_Y;
    const int cw = face ? 11 : key >= 100 ? 11 : 13;
    const uint32_t bg = face ? (s_style ? 0x26282c : caps[key - FH1_KEY_A]) : key >= 100 ? 0x6a6f78 : key == FH1_KEY_MENU ? 0x26282c : 0xf2f2f2;
    const uint16_t f = (uint16_t)(((bg >> 8) & 0xF800) | ((bg >> 5) & 0x07E0) | ((bg >> 3) & 0x001F));
    for (int yy = 0; yy < 11; yy++)
        for (int xx = 0; xx < cw; xx++) {
            const int cx = xx < 5 ? 5 : xx > cw - 6 ? cw - 6 : xx, dx = 2 * (xx - cx), dy = 2 * yy - 10;
            if (dx * dx + dy * dy <= 121 && x + xx >= 0 && x + xx < w && y + yy >= 0 && y + yy < h)
                px[(y + yy) * w + x + xx] = dx * dx + dy * dy > 81 ? 0x0841 : f;
        }
    if (face && s_style) fh1_px_text(px, w, h, x + 2, y + 2, names[s_style][key - FH1_KEY_A], ink[s_style][key - FH1_KEY_A], 1);
    else if (!face) fh1_px_text(px, w, h, x + 3, y + 2, key == FH1_KEY_START ? ">" : key == FH1_KEY_SELECT ? "-" : key == 100 ? "<" : key == 101 ? "^" : "=", 0x101010, 1);
    return x + cw;
}
const char *fh1_app_dir(void) { return "."; }
const char *fh1_asset_dir(const char *id)
{
    static char p[256];
    snprintf(p, sizeof(p), "../../../../assets/pools/%s", id);         // built there by tools/fh1sfx.py
    return p;
}

// The mixer, as the FH1's (8 voices, 44.1 kHz), writing what it plays to OUTDIR/audio.wav.
#define VOICES 8
static struct { const int16_t *pcm; uint32_t frames, pos; uint16_t vol; bool on; } s_voice[VOICES];
static int16_t *s_audio;
static size_t s_audio_n, s_audio_cap;
int fh1_sfx_play(const int16_t *pcm, uint32_t frames, uint32_t rate, uint8_t volume)
{
    int k = -1;
    for (int i = 0; i < VOICES && k < 0; i++)
        if (!s_voice[i].on) k = i;
    if (k < 0) k = 0;
    s_voice[k].pcm = pcm;
    s_voice[k].frames = frames;
    s_voice[k].pos = 0;
    s_voice[k].vol = volume + (volume >> 7);
    s_voice[k].on = true;
    return k;
}
void fh1_sfx_stop(int v) { if (v >= 0 && v < VOICES) s_voice[v].on = false; }
void fh1_sfx_stop_all(void) { for (int v = 0; v < VOICES; v++) s_voice[v].on = false; }
bool fh1_sfx_playing(int v) { return v >= 0 && v < VOICES && s_voice[v].on; }
static void mix(int frames)
{
    if (s_audio_n + frames > s_audio_cap) {
        s_audio_cap = (s_audio_n + frames) * 2;
        s_audio = realloc(s_audio, s_audio_cap * 2);
    }
    for (int i = 0; i < frames; i++) {
        int32_t acc = 0;
        for (int k = 0; k < VOICES; k++) {
            if (!s_voice[k].on) continue;
            if (s_voice[k].pos >= s_voice[k].frames) {
                s_voice[k].on = false;
                continue;
            }
            acc += (s_voice[k].pcm[s_voice[k].pos++] * (int32_t)s_voice[k].vol) >> 8;
        }
        if (acc > 24000) acc = 24000 + (acc - 24000) / 4;             // the FH1 mixer's soft limit
        else if (acc < -24000) acc = -24000 + (acc + 24000) / 4;
        s_audio[s_audio_n++] = (int16_t)(acc > 32767 ? 32767 : acc < -32768 ? -32768 : acc);
    }
}
static void write_audio(void)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/audio.wav", s_dir);
    FILE *f = fopen(path, "wb");
    if (!f || !s_audio_n) return;
    const uint32_t bytes = (uint32_t)s_audio_n * 2, rate = 44100, brate = rate * 2;
    fwrite("RIFF", 1, 4, f);
    const uint32_t riff = 36 + bytes;
    fwrite(&riff, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    const uint32_t fmt_len = 16;
    const uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
    fwrite(&fmt_len, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
    fwrite(&brate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&bytes, 4, 1, f);
    fwrite(s_audio, 2, s_audio_n, f);
    fclose(f);
    printf("audio: %s (%.1f s)\n", path, s_audio_n / 44100.0);
}
void fh1_ui_message(const char *a, const char *b) { printf("message: %s: %s\n", a, b); }
void fh1_log(const char *f, ...) { va_list ap; va_start(ap, f); vprintf(f, ap); va_end(ap); printf("\n"); }
void fh1_app_exit(void) {}
bool fh1_screen_open(uint32_t p, fh1_frame_fn f, void (*c)(void *), void *u) { s_frame = f; s_user = u; return true; }
int app_main(int argc, char *argv[]);
void battle_debug(void);
bool dev_command(const char *line);
bool fh1_dev_command(char *out, size_t n) { return false; }
void battle_host_place(int i, int c, int r, int dir);
void battle_host_act(int a, int act, int what, int b);
void battle_host_counter(int i);
void battle_host_target(int a, int act, int what, int c, int r);
void battle_host_gear(int i, int weapon, int armour);
void battle_host_end(bool won);
void battle_host_ranged(int i, int range);
void battle_host_cam(int x, int y);
void battle_host_xp(int slot, int xp);

#define W 320
#define H 240
#define GUARD 8192
static uint16_t s_mem[GUARD + W * H + GUARD];
static uint16_t *const s_px = s_mem + GUARD;

static void guard_check(const char *when)
{
    for (int i = 0; i < GUARD; i++)
        if (s_mem[i] != 0xA5A5 || s_mem[GUARD + W * H + i] != 0xA5A5) {
            fprintf(stderr, "WRITE OUTSIDE THE FRAME BUFFER (%s, %s, offset %d)\n", when, i < GUARD && s_mem[i] != 0xA5A5 ? "before" : "after", i);
            exit(2);
        }
}

static void frames(int n)
{
    for (int i = 0; i < n; i++) {
        s_ms += 33;
        mix(1455);                                       // 33 ms at 44.1 kHz
        if (!s_frame(s_px, W, H, 33, s_user)) {
            printf("the game ended\n");
            exit(0);
        }
        guard_check("frame");
    }
}

static void shot(const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.bmp", s_dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    const int row = W * 3, size = 54 + row * H;
    uint8_t hd[54] = { 'B', 'M', (uint8_t)size, (uint8_t)(size >> 8), (uint8_t)(size >> 16), 0, 0, 0, 0, 0, 54, 0, 0, 0,
                       40, 0, 0, 0, W & 255, W >> 8, 0, 0, H & 255, H >> 8, 0, 0, 1, 0, 24 };
    fwrite(hd, 1, 54, f);
    for (int y = H - 1; y >= 0; y--)
        for (int x = 0; x < W; x++) {
            const uint16_t v = s_px[y * W + x];
            const uint8_t bgr[3] = { (uint8_t)((v & 31) << 3), (uint8_t)(((v >> 5) & 63) << 2), (uint8_t)((v >> 11) << 3) };
            fwrite(bgr, 1, 3, f);
        }
    fclose(f);
}

static int key_of(const char *k)
{
    static const char *names[] = { "UP", "DOWN", "LEFT", "RIGHT", "A", "B", "X", "Y", "START", "SELECT", "MENU" };
    for (int i = 0; i < 11; i++)
        if (!strcmp(k, names[i])) return i;
    return -1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "host SCRIPT [OUTDIR]\n");
        return 1;
    }
    if (argc > 2) s_dir = argv[2];
    FILE *sc = fopen(argv[1], "r");
    if (!sc) return 1;
    srand(1);
    for (int i = 0; i < GUARD; i++) s_mem[i] = s_mem[GUARD + W * H + i] = 0xA5A5;
    app_main(0, NULL);
    if (!s_frame) return 1;
    frames(3);
    char line[256];
    while (fgets(line, sizeof(line), sc)) {
        char cmd[32] = "", a[64] = "";
        int n[5] = { 0 };
        if (line[0] == '#' || sscanf(line, "%31s", cmd) != 1) continue;
        if (!strcmp(cmd, "key") && sscanf(line, "%*s %63s", a) == 1) {
            s_keys = 1u << key_of(a);
            frames(2);
            s_keys = 0;
            frames(6);
        } else if (!strcmp(cmd, "hold") && sscanf(line, "%*s %63s %d", a, &n[0]) == 2) {
            s_keys = 1u << key_of(a);
            frames(n[0]);
            s_keys = 0;
        } else if (!strcmp(cmd, "wait") && sscanf(line, "%*s %d", &n[0]) == 1) frames(n[0]);
        else if (!strcmp(cmd, "shot") && sscanf(line, "%*s %63s", a) == 1) shot(a);
        else if (!strcmp(cmd, "log")) battle_debug();
        else if (!strcmp(cmd, "user") && sscanf(line, "%*s %23s", s_player) == 1) {
        } else if (!strcmp(cmd, "pad") && sscanf(line, "%*s %63s", a) == 1)
            s_style = !strcmp(a, "xbox") ? 1 : !strcmp(a, "nintendo") ? 2 : !strcmp(a, "ps") ? 3 : 0;
        else if (dev_command(line)) {
        }
        else fprintf(stderr, "? %s", line);
    }
    write_audio();
    return 0;
}
