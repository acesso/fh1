// The UI task, the LVGL display port, the desktop and the window manager.
// Look: text boxes in the style of handheld RPGs of the late 1990s (a light box, a double-line border
// with rounded corners, the title set into the top border, plain text items with a ▶ cursor), drawn
// from scratch; palettes are themes.
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <time.h>
#include "fh1_ui.h"
#include "src/core/lv_refr_private.h"   // lv_inv_area(): drop pending redraws when lending the screen ends
#include "fh1_board.h"
#include "fh1_devices.h"
#include "fh1_input.h"
#include "fh1_gamepad.h"
#include "src/misc/cache/instance/lv_image_cache.h"   // lv_image_cache_drop: tray icons are redrawn in place
#include "fh1_settings.h"
#include "fh1_i18n.h"
#include "fh1_events.h"
#include "fh1_mem.h"
#include "fh1_app.h"
#include "esp_random.h"

LV_FONT_DECLARE(fh1_font_8);    // unscii-8 with Latin-1 accents (fh1_font8.c)
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "ui_priv.h"
#include "ui_look.h"

static const char *TAG = "ui";

#define MAX_WINS 12

// ── Themes ──

// Four looks (ui_look.h): Gen (rounded boxes, a ball on each corner, the title in a pill the border runs into, a
// dotted or latticed desktop), Hex (square boxes cut at the corners, the title on a band of warning stripes, a
// honeycomb desktop; the window in focus bright, the others dim, alerts red), and the framed looks, whose windows
// wear a frame drawn by ui_look.c: Garden (wooden planks, a vine that grows round, flowers, a butterfly) and
// Command (metal plates that close round the window, brackets, a lit tab, a spark running along the light).

typedef struct {
    const char *id, *name;
    uint32_t paper, ink, dim, desk_a, desk_b;
    uint32_t ball;        // the top half of the corner balls (Gen); the alert colour (Hex)
    bool lattice;         // desktop pattern: diamond lattice (else dots; Hex: honeycomb)
    int look;             // LOOK_*
    uint32_t text, inset; // body text and the inner line (0: the ink)
    int voice;            // the system sounds' voice (UI_VOICE_*)
} theme_t;

static const theme_t s_themes[] = {
    { "gen1", "Gen 1 (green)", 0xe0f8d0, 0x081820, 0x6a9a6a, 0x88c070, 0x78b060, 0x346856, false, LOOK_GEN, 0, 0, UI_VOICE_SOFT },
    { "gen2", "Gen 2 (colour)", 0xf8f8f8, 0x202020, 0x8a8f9a, 0xa8d0f0, 0x98c0e8, 0xd83838, true, LOOK_GEN, 0, 0, UI_VOICE_SOFT },
    // An early-warning console: amber on black, alarm red, warm white text, dim amber for what is not in focus.
    { "hex", "Hex (alert)", 0x000000, 0xffaa00, 0x7a5200, 0x000000, 0x2e1f00, 0xe60908, false, LOOK_HEX, 0xf2eee6, 0x5c3d00,
      UI_VOICE_ALARM },
    // A garden shed's sign: cream paper, brown ink, a lawn; the frame is wood with a vine on it.
    { "garden", "Garden", 0xf3ead2, 0x3b2a1a, 0x9c8a6e, 0x4e8a3a, 0x3f7530, 0xc0392b, false, LOOK_GARDEN, 0x3b2a1a, 0xc9b48c,
      UI_VOICE_GARDEN },
    // A command console: cyan light on near-black glass, steel plates, red alerts.
    { "scifi", "Command", 0x07141c, 0x3fd8ff, 0x1d5a6b, 0x050c12, 0x0d2430, 0xff4a3a, false, LOOK_SCIFI, 0xd6f4ff, 0x0e3442,
      UI_VOICE_SCIFI },
};
#define NTHEMES (sizeof(s_themes) / sizeof(s_themes[0]))
static const theme_t *s_theme = &s_themes[0];
#define HEX (s_theme->look == LOOK_HEX)
#define FRAMED (s_theme->look >= LOOK_GARDEN)     // the frame is drawn by ui_look.c
static uint32_t text_rgb(void) { return s_theme->text ? s_theme->text : s_theme->ink; }
static uint32_t inset_rgb(void) { return s_theme->inset ? s_theme->inset : s_theme->ink; }
#define HEX_ALERT_INSET 0x5c0a08            // Hex: an alert's inner line, the red's dim (as the inset is the amber's)

struct fh1_win {
    lv_obj_t *box, *title, *body, *footer;
    lv_obj_t *inset;                // the inner line
    int cw, ch, body_y;             // content size and where the body starts
    lv_obj_t *frame;                // the title frame image (title_frame())
    uint32_t *frame_px;
    lv_image_dsc_t frame_dsc;
    lv_obj_t *ghost;                // the opening animation's outline, while it runs
    lv_obj_t *balls[4];
    struct ui_list *list;           // a paged list (fh1_ui_list), if the window has one
    struct ui_views *views;         // sideways pages (fh1_win_add_view), if the window has them
    lv_group_t *group;
    fh1_win_key_cb key_cb;
    void *key_user;
    fh1_win_close_cb close_cb;
    void *close_user;
    void *owner;
    bool alert;                     // a message or a question (Hex: red)
    signed char band;               // Hex: how its band was last drawn (-1 never, 0 dim, 1 focused, 2 alert)
    unsigned char phase;            // Hex: where the stripes are (they run, look_tick)
    short band_w;                   // Hex: the band image's width, as last set
    // Framed looks: the four sides of the frame, its corners and the mote, as pictures of their own (PSRAM).
    lv_obj_t *strip[4];
    uint32_t *strip_px[4], *corner_px, *mote_px;
    lv_image_dsc_t strip_dsc[4], corner_dsc[4];
    lv_obj_t *mote, *scan;
    short grow;                     // how far the opening has got (0..256), as the frame was drawn
    signed char fstate;             // the state the frame was drawn in (-1 never)
    bool building;                  // opening or closing (frame_build)
    unsigned char copa;             // the content's opacity during the build
};

typedef struct {
    fh1_ui_fn fn;
    void *arg;
} ui_call_t;

static QueueHandle_t s_calls;
static fh1_win_t *s_wins[MAX_WINS];    // bottom .. top; the top one has focus
static int s_nwins;
static int s_cascade;
static lv_obj_t *s_tabs, *s_sbar, *s_clock, *s_clk_h, *s_clk_colon, *s_clk_m, *s_wifi[4], *s_sd, *s_batt, *s_batt_fill;
static fh1_ui_status_fn s_status_fn;
static int s_bar_h;                    // height of each bar
static volatile bool s_paused;         // a full-screen program owns the display
static fh1_ui_fn s_menu_fn;
static void *s_menu_arg;
static struct { fh1_ui_fn fn; void *arg; } s_watchers[8];
static int s_nwatchers;
static int s_w, s_h, s_u;              // screen size, and the unit (1 or 2 pixels)
static void look_palette(look_pal_t *p)
{
    *p = (look_pal_t){ s_u, s_theme->paper, s_theme->ink, s_theme->dim, s_theme->ball, text_rgb(), inset_rgb(), s_theme->desk_a,
                       s_theme->desk_b };
}
static const lv_font_t *s_font;
static int s_line;                     // font line height
static lv_style_t st_box, st_inset, st_title, st_text, st_item, st_dim, st_cursor, st_bar, st_tab_on, st_icon, st_icon_off;

#define U(x) ((x) * s_u)

// Items: the text starts 9 units in, the cursor 8 to its left (Gen's 5-unit triangle); Hex's 7-unit hexagon takes
// 10 and 9, so two units part it from the text.
static int item_pad(void) { return FRAMED ? U(11) : HEX ? U(10) : U(9); }
static int cursor_ofs(void) { return FRAMED ? -U(9) : HEX ? -U(9) : -U(8); }

// ── Cursor bitmaps (A8, recoloured with the ink colour): a solid and a hollow ▶ ──

static uint8_t s_cur_px[2][16 * 16];
static lv_image_dsc_t s_cur[2];

// Hex: a pointy-top hexagon, solid in the window in focus and hollow in the others.
static const char *const s_hexcur[2][8] = {
    { "...#...", ".#####.", "#######", "#######", "#######", "#######", ".#####.", "...#..." },
    { "...#...", ".##.##.", "#.....#", "#.....#", "#.....#", "#.....#", ".##.##.", "...#..." },
};

static uint32_t *s_cur_argb[2];        // framed looks: cursors in colour (PSRAM)

static void make_cursors(void)
{
    if (FRAMED) {
        look_pal_t p;
        look_palette(&p);
        for (int k = 0; k < 2; k++) {
            if (!s_cur_argb[k]) s_cur_argb[k] = heap_caps_malloc(16 * 16 * 4, MALLOC_CAP_SPIRAM);
            int w = 0, h = 0;
            if (!s_cur_argb[k] || !look_cursor(s_theme->look, &p, k, s_cur_argb[k], &w, &h)) continue;
            s_cur[k].header.magic = LV_IMAGE_HEADER_MAGIC;
            s_cur[k].header.cf = LV_COLOR_FORMAT_ARGB8888;
            s_cur[k].header.w = w;
            s_cur[k].header.h = h;
            s_cur[k].header.stride = w * 4;
            s_cur[k].data_size = w * h * 4;
            s_cur[k].data = (const uint8_t *)s_cur_argb[k];
        }
        return;
    }
    const int w = HEX ? U(7) : U(5), h = HEX ? U(8) : U(7);
    for (int k = 0; k < 2; k++) {
        memset(s_cur_px[k], 0, sizeof(s_cur_px[k]));
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                bool on;
                if (HEX) {
                    on = s_hexcur[k][y / s_u][x / s_u] == '#';
                } else {
                    // triangle: column x is filled while |y - centre| <= (w - x) * h / (2w)
                    int c = h / 2, half = (w - x) * h / (2 * w);
                    bool in = y >= c - half && y <= c + half;
                    bool edge = in && (x == 0 || y == c - half || y == c + half);
                    on = k == 0 ? in : edge;
                }
                if (on) s_cur_px[k][y * w + x] = 0xff;
            }
        }
        s_cur[k].header.magic = LV_IMAGE_HEADER_MAGIC;
        s_cur[k].header.cf = LV_COLOR_FORMAT_A8;
        s_cur[k].header.w = w;
        s_cur[k].header.h = h;
        s_cur[k].header.stride = w;
        s_cur[k].data_size = w * h;
        s_cur[k].data = s_cur_px[k];
    }
}

// ── Pixel sprites: rows of characters, one per pixel, coloured from a palette (some colours come from
// the theme, so sprites are redrawn when it changes) ──

static uint32_t argb(uint32_t rgb) { return 0xff000000u | rgb; }

static uint32_t sprite_colour(char c)
{
    switch (c) {
    case 'K': return argb(s_theme->ink);
    case 'P': return argb(s_theme->paper);
    case 'R': return argb(s_theme->ball);
    case 'W': return argb(0xffffff);
    case 'b': return argb(0x3a78e0);     // SD card blue
    case 'g': return argb(0xe8b84a);     // gold contacts
    case 'L': return argb(0xc8ccd2);     // chip legs
    case 'D': return argb(0x3a3e44);     // chip body
    case 'r': return argb(0xe5302c);     // alert red
    case 'Y': return argb(0xf4c21f);     // warning yellow
    case 'G': return argb(0x2fb36a);     // good green
    case 'V': return argb(0x7b2fbe);     // GameBoy Color purple
    case 'B': return argb(0xc08a4a);     // parcel brown
    case 'S': return argb(0x9bbc0f);     // handheld screen
    case 'o': return argb(0xf08a24);     // bridge orange
    case 'c': return argb(0x26a7c7);     // water, radio waves
    case 'p': return argb(0xf07cc0);     // PlayStation square pink
    case 'x': return argb(0x5cc23a);     // Xbox A green
    case 'q': return argb(0x5a8cff);     // PlayStation cross blue
    case 'e': return argb(0xff5a5a);     // PlayStation circle red
    case 't': return argb(0x3cd4a0);     // PlayStation triangle green
    default: return 0;                   // '.' transparent
    }
}

typedef struct {
    const char *const *rows;
    int h;
    lv_image_dsc_t dsc, dsc2;       // at the unit's size, and twice that
    uint32_t *px, *px2;
} sprite_t;

static void sprite_draw_at(sprite_t *sp, int z, uint32_t **pxp, lv_image_dsc_t *dsc)
{
    const int w = strlen(sp->rows[0]), W = w * z, H = sp->h * z;
    if (!*pxp) *pxp = heap_caps_malloc(W * H * 4, MALLOC_CAP_SPIRAM);    // internal RAM is for Wi-Fi
    uint32_t *px = *pxp;
    if (!px) return;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) px[y * W + x] = sprite_colour(sp->rows[y / z][x / z]);
    }
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_ARGB8888;
    dsc->header.w = W;
    dsc->header.h = H;
    dsc->header.stride = W * 4;
    dsc->data_size = W * H * 4;
    dsc->data = (const uint8_t *)px;
}

static void sprite_draw(sprite_t *sp)
{
    sprite_draw_at(sp, s_u, &sp->px, &sp->dsc);
    sprite_draw_at(sp, 2 * s_u, &sp->px2, &sp->dsc2);
}

#define SPRITE(name, ...) static const char *const name##_rows[] = { __VA_ARGS__ }; \
    static sprite_t name = { .rows = name##_rows, .h = sizeof(name##_rows) / sizeof(name##_rows[0]) };

// A ball: red top, white bottom, a black band, a white button in the middle.
SPRITE(sp_ball,
    "..KKKKK..",
    ".KRRRRRK.",
    "KRRRRRRRK",
    "KRRKKKRRK",
    "KKKKWKKKK",
    "KWWKKKWWK",
    "KWWWWWWWK",
    ".KWWWWWK.",
    "..KKKKK..")
// An SD card: gold contacts on a blue card with a white label, top-left corner cut.
SPRITE(sp_sd,
    "..KKKKKK",
    ".KgKgKgK",
    "KbgbgbgK",
    "KbbbbbbK",
    "KbWWWWbK",
    "KbWWWWbK",
    "KbWWWWbK",
    "KbbbbbbK",
    "KKKKKKKK")
// A memory chip: dark body, pin-1 dot, silver legs.
SPRITE(sp_chip,
    ".L.L.L.L.",
    "KKKKKKKKK",
    "KDDDDDDDK",
    "KDWDDDDDK",
    "KDDDDDDDK",
    "KKKKKKKKK",
    ".L.L.L.L.")
SPRITE(sp_menu,
    "WWWWWWW",
    ".......",
    "WWWWWWW",
    ".......",
    "WWWWWWW")
SPRITE(sp_start,
    "K...",
    "KK..",
    "KKK.",
    "KK..",
    "K...")
SPRITE(sp_select,
    ".KKK.",
    "KKKKK",
    ".KKK.")
// The D-pad, outlined, its left and right arms lit: "press left or right". Drawn bare, not on a pill.
SPRITE(sp_lr,
    "...KKKKK...",
    "...KDDDK...",
    "...KDDDK...",
    "KKKKDDDKKKK",
    "KWWWDDDWWWK",
    "KWWWDDDWWWK",
    "KWWWDDDWWWK",
    "KKKKDDDKKKK",
    "...KDDDK...",
    "...KDDDK...",
    "...KKKKK...")

// Button letters and shapes for the controller styles (fh1_gamepad_style): Xbox letters in their colours,
// Nintendo letters in white, PlayStation shapes.
SPRITE(sx_a,
    ".xxx.",
    "x...x",
    "x...x",
    "xxxxx",
    "x...x",
    "x...x",
    "x...x")
SPRITE(sx_b,
    "rrrr.",
    "r...r",
    "r...r",
    "rrrr.",
    "r...r",
    "r...r",
    "rrrr.")
SPRITE(sx_x,
    "b...b",
    "b...b",
    ".b.b.",
    "..b..",
    ".b.b.",
    "b...b",
    "b...b")
SPRITE(sx_y,
    "Y...Y",
    "Y...Y",
    ".Y.Y.",
    "..Y..",
    "..Y..",
    "..Y..",
    "..Y..")
SPRITE(sn_a,
    ".WWW.",
    "W...W",
    "W...W",
    "WWWWW",
    "W...W",
    "W...W",
    "W...W")
SPRITE(sn_b,
    "WWWW.",
    "W...W",
    "W...W",
    "WWWW.",
    "W...W",
    "W...W",
    "WWWW.")
SPRITE(sn_x,
    "W...W",
    "W...W",
    ".W.W.",
    "..W..",
    ".W.W.",
    "W...W",
    "W...W")
SPRITE(sn_y,
    "W...W",
    "W...W",
    ".W.W.",
    "..W..",
    "..W..",
    "..W..",
    "..W..")
SPRITE(sp_ps_o,
    "..eee..",
    ".e...e.",
    "e.....e",
    "e.....e",
    "e.....e",
    ".e...e.",
    "..eee..")
SPRITE(sp_ps_x,
    "q.....q",
    ".q...q.",
    "..q.q..",
    "...q...",
    "..q.q..",
    ".q...q.",
    "q.....q")
SPRITE(sp_ps_t,
    "...t...",
    "...t...",
    "..t.t..",
    "..t.t..",
    ".t...t.",
    ".t...t.",
    "ttttttt")
SPRITE(sp_ps_s,
    "ppppppp",
    "p.....p",
    "p.....p",
    "p.....p",
    "p.....p",
    "p.....p",
    "ppppppp")
// The D-pad with its up and down arms lit.
SPRITE(sp_ud,
    "...KKKKK...",
    "...KWWWK...",
    "...KWWWK...",
    "KKKKWWWKKKK",
    "KDDDDDDDDDK",
    "KDDDDDDDDDK",
    "KDDDDDDDDDK",
    "KKKKWWWKKKK",
    "...KWWWK...",
    "...KWWWK...",
    "...KKKKK...")

// ── The common icons (fh1_ui_icon): a still picture, and a part that moves when animated. Parts are drawn
// behind the picture, so an arrow can disappear into a chip. ──
// A tray, and a blue arrow dropping into it (something arriving) or rising out of it (going).
SPRITE(ic_tray,
    "............",
    "............",
    "............",
    "............",
    "............",
    "............",
    "............",
    "K..........K",
    "K..........K",
    "K..........K",
    "KKKKKKKKKKKK")
SPRITE(ic_arrow_down,
    "..bbbb..",
    "..bbbb..",
    "..bbbb..",
    "bbbbbbbb",
    ".bbbbbb.",
    "..bbbb..",
    "...bb...")
SPRITE(ic_arrow_up,
    "...bb...",
    "..bbbb..",
    ".bbbbbb.",
    "bbbbbbbb",
    "..bbbb..",
    "..bbbb..",
    "..bbbb..")
// Firmware: a chip, a gold arrow sinking into it.
SPRITE(ic_fw_chip,
    "............",
    "............",
    "............",
    "............",
    "............",
    ".KKKKKKKKKK.",
    "LKDDDDDDDDKL",
    "LKDWDDDDDDKL",
    ".KDDDDDDDDK.",
    "LKDDDDDDDDKL",
    "LKDDDDDDDDKL",
    ".KKKKKKKKKK.")
SPRITE(ic_fw_arrow,
    "..gggg..",
    "..gggg..",
    "..gggg..",
    "gggggggg",
    ".gggggg.",
    "..gggg..",
    "...gg...")
// A letter; it bobs.
SPRITE(ic_message,
    "............",
    "............",
    "KKKKKKKKKKKK",
    "KKWWWWWWWWKK",
    "KWKWWWWWWKWK",
    "KWWKWWWWKWWK",
    "KWWWKKKKWWWK",
    "KWWWWWWWWWWK",
    "KWWWWWWWWWWK",
    "KKKKKKKKKKKK")
// A battery with one red bar that blinks.
SPRITE(ic_battery,
    "............",
    "............",
    "............",
    "KKKKKKKKKK..",
    "KWWWWWWWWK..",
    "KWWWWWWWWKK.",
    "KWWWWWWWWKK.",
    "KWWWWWWWWK..",
    "KKKKKKKKKK..")
SPRITE(ic_battery_bar,
    "rr",
    "rr",
    "rr",
    "rr")
// A warning sign with a blinking mark.
SPRITE(ic_warning,
    ".....KK.....",
    "....KYYK....",
    "....KYYK....",
    "...KYYYYK...",
    "...KYYYYK...",
    "..KYYYYYYK..",
    "..KYYYYYYK..",
    ".KYYYYYYYYK.",
    ".KYYYYYYYYK.",
    "KYYYYYYYYYYK",
    "KKKKKKKKKKKK")
SPRITE(ic_warning_mark,
    "KK",
    "KK",
    "KK",
    "..",
    "KK")

// A package: a parcel, a blue arrow dropping into it.
SPRITE(ic_parcel,
    "............",
    "............",
    "............",
    "............",
    "............",
    "KKKKKKKKKKKK",
    "KBBBBYYBBBBK",
    "KBBBBYYBBBBK",
    "KBBBBYYBBBBK",
    "KBBBBBBBBBBK",
    "KKKKKKKKKKKK")
// The GameBoy Color app: a purple handheld, a cartridge sliding into it.
SPRITE(ic_gbc,
    "............",
    "............",
    "............",
    "..KKKKKKKK..",
    ".KVVVVVVVVK.",
    ".KVKKKKKKVK.",
    ".KVKSSSSKVK.",
    ".KVKSSSSKVK.",
    ".KVKKKKKKVK.",
    ".KVVVVVVVVK.",
    ".KVKVVVVrVK.",
    ".KKKKVVrVVK.",
    ".KVKVVVVVVK.",
    "..KKKKKKKK..")
SPRITE(ic_cartridge,
    "LLLLLL",
    "LKKKKL",
    "LWWWWL",
    "LLLLLL",
    "LLLLLL")

// The NES app: a grey console, a cartridge sliding into its slot.
SPRITE(ic_nes,
    "............",
    "............",
    "............",
    "............",
    "............",
    "KKKKKKKKKKKK",
    "KWLLLLLLLLLK",
    "KLKKKKKKKKLK",
    "KLLLLLLLLLLK",
    "KDDDDDDDDDDK",
    "KDrDDDDDDDDK",
    "KKKKKKKKKKKK")
// The Master System app: a black console with its red stripe.
SPRITE(ic_sms,
    "............",
    "............",
    "............",
    "............",
    "............",
    "KKKKKKKKKKKK",
    "KDDDDDDDDDDK",
    "KDKKKKKKKKDK",
    "KrrrrrrrrrrK",
    "KDDDDDDDDDDK",
    "KDDDDDDDLLDK",
    "KKKKKKKKKKKK")
// The Music app: a cassette tape; its reels turn (the hubs blink).
SPRITE(ic_cassette,
    "............",
    "............",
    "KKKKKKKKKKKK",
    "KDDDDDDDDDDK",
    "KDrrrrrrrrDK",
    "KDWWWWWWWWDK",
    "KDWLWWWWLWDK",
    "KDWWWWWWWWDK",
    "KDDDDDDDDDDK",
    "KDDDLLLLDDDK",
    "KKKKKKKKKKKK")
SPRITE(ic_cassette_hubs,
    "K....K")
// Small menu icons (10 x 9, still): one per System entry and per bridge menu entry.
SPRITE(mi_install,
    "....GG....",
    "....GG....",
    "..GGGGGG..",
    "...GGGG...",
    "....GG....",
    "KKKKKKKKKK",
    "KBBBBBBBBK",
    "KBBBBBBBBK",
    "KKKKKKKKKK")
SPRITE(mi_storage,
    "..KKKKKK..",
    ".KbbbbbbK.",
    "KbgbgbgbK.",
    "KbbbbbbbK.",
    "KbbWWWWbK.",
    "KbbWWWWbK.",
    "KbbbbbbbK.",
    "KKKKKKKKK.",
    "..........")
SPRITE(mi_devices,
    ".L.L.L.L..",
    "KKKKKKKKK.",
    "KDDDDDDDK.",
    "KDGDDDDDK.",
    "KDDDDDDDK.",
    "KDDDDDDDK.",
    "KKKKKKKKK.",
    ".L.L.L.L..",
    "..........")
SPRITE(mi_io,
    "..........",
    ".KKKKKKKK.",
    "KLLLLLLLLK",
    "KLDLLLLrLK",
    "KDDDLLGLbK",
    "KLDLLLLYLK",
    "KLLKKKKLLK",
    "KKK....KKK",
    "..........")
SPRITE(mi_bridge,
    "..........",
    "...oooo...",
    ".oo....oo.",
    "oooooooooo",
    "o..o..o..o",
    "o..o..o..o",
    "cccccccccc",
    "c.cc.cc.cc",
    "..........")
SPRITE(mi_about,
    "...bbbb...",
    ".bbbWWbbb.",
    ".bbbbbbbb.",
    "bbbbWWbbbb",
    "bbbbWWbbbb",
    "bbbbWWbbbb",
    ".bbbWWbbb.",
    ".bbbbbbbb.",
    "...bbbb...")
SPRITE(mi_network,
    "..........",
    "...cccc...",
    ".cc....cc.",
    "c..cccc..c",
    "..c....c..",
    "....cc....",
    "....KK....",
    "....KK....",
    "...KKKK...")
SPRITE(mi_rename,
    "........K.",
    ".......KrK",
    "......KYK.",
    ".....KYK..",
    "....KYK...",
    "...KYK....",
    "..KYK.....",
    "..KK......",
    "..........")
SPRITE(mi_forget,
    "..........",
    ".rr....rr.",
    "..rr..rr..",
    "...rrrr...",
    "....rr....",
    "...rrrr...",
    "..rr..rr..",
    ".rr....rr.",
    "..........")
SPRITE(mi_users,
    "..bb......",
    ".bbbb..oo.",
    ".bbbb.oooo",
    "..bb..oooo",
    ".bbbb..oo.",
    "bbbbbboooo",
    "bbbbbboooo",
    "bbbbbboooo",
    "..........")
// The top-level menus' tabs: Apps (four tiles), System (a gear).
SPRITE(mi_apps,
    "..........",
    ".rrr..bbb.",
    ".rrr..bbb.",
    ".rrr..bbb.",
    "..........",
    ".GGG..YYY.",
    ".GGG..YYY.",
    ".GGG..YYY.",
    "..........")
SPRITE(mi_system,
    "....LL....",
    ".L.LLLL.L.",
    "..LLDDLL..",
    ".LLD..DLL.",
    "LLD....DLL",
    ".LLD..DLL.",
    "..LLDDLL..",
    ".L.LLLL.L.",
    "....LL....")
// The apps in the Apps menu: the handheld, the two consoles, a cassette, a parcel (an installed package).
SPRITE(mi_gbc,
    ".KKKKKKKK.",
    ".KSSSSSSK.",
    ".KSSSSSSK.",
    ".KVVVVVVK.",
    ".KVKVVVrK.",
    ".KKKKVrVK.",
    ".KVKVVVVK.",
    ".KVVVVVVK.",
    ".KKKKKKKK.")
SPRITE(mi_nes,
    "..........",
    "..........",
    "KKKKKKKKKK",
    "KWLLLLLLLK",
    "KLKKKKKKLK",
    "KLLLLLLLLK",
    "KDDDDDDDDK",
    "KDrDDDDDDK",
    "KKKKKKKKKK")
SPRITE(mi_sms,
    "..........",
    "..........",
    "KKKKKKKKKK",
    "KDDDDDDDDK",
    "KDKKKKKKDK",
    "KrrrrrrrrK",
    "KDDDDDDDDK",
    "KDDDDDDLLK",
    "KKKKKKKKKK")
SPRITE(mi_music,
    "..........",
    "KKKKKKKKKK",
    "KrrrrrrrrK",
    "KWWWWWWWWK",
    "KWKWWWWKWK",
    "KWWWWWWWWK",
    "KDDLLLLDDK",
    "KKKKKKKKKK",
    "..........")
// The Themes app: a painter's palette, its thumb hole and four paints.
SPRITE(mi_themes,
    "...KKKKK..",
    "..KBBBBBK.",
    ".KBrBBYBBK",
    "KBBBBBBBBK",
    "KBGBK..KBK",
    "KBBBK..KBK",
    "KBbBBKKBBK",
    ".KBBBBBBK.",
    "..KKKKKK..")
SPRITE(mi_package,
    "..........",
    "..........",
    "KKKKKKKKKK",
    "KBBBYYBBBK",
    "KBBBYYBBBK",
    "KBBBYYBBBK",
    "KBBBBBBBBK",
    "KKKKKKKKKK",
    "..........")

SPRITE(ic_nes_cart,
    "LLLLLL",
    "LDDDDL",
    "LLLLLL",
    "LLLLLL",
    "LLLLLL")

typedef enum { MOVE_NONE, MOVE_SLIDE, MOVE_BOB, MOVE_BLINK } icon_move_t;
typedef struct {
    sprite_t *still, *part;         // part may be NULL: then the still picture moves
    int8_t px, py;                  // where the part sits (units)
    int8_t from, to;                // SLIDE: the part's y runs from..to, again and again; BOB: by up to..
    icon_move_t move;
    uint16_t ms;
} icon_def_t;

static const icon_def_t s_icon_defs[FH1_ICON_COUNT] = {
    [FH1_ICON_SD] = { &sp_sd, NULL, 0, 0, 0, 0, MOVE_NONE, 0 },
    [FH1_ICON_INTERNAL] = { &sp_chip, NULL, 0, 0, 0, 0, MOVE_NONE, 0 },
    [FH1_ICON_DOWNLOAD] = { &ic_tray, &ic_arrow_down, 2, 0, -7, 4, MOVE_SLIDE, 900 },
    [FH1_ICON_UPLOAD] = { &ic_tray, &ic_arrow_up, 2, 0, 4, -7, MOVE_SLIDE, 900 },
    [FH1_ICON_FIRMWARE] = { &ic_fw_chip, &ic_fw_arrow, 2, 0, -7, 6, MOVE_SLIDE, 1100 },
    [FH1_ICON_MESSAGE] = { &ic_message, NULL, 0, 0, 0, 1, MOVE_BOB, 350 },
    [FH1_ICON_BATTERY_LOW] = { &ic_battery, &ic_battery_bar, 1, 4, 0, 0, MOVE_BLINK, 450 },
    [FH1_ICON_WARNING] = { &ic_warning, &ic_warning_mark, 5, 3, 0, 0, MOVE_BLINK, 450 },
    [FH1_ICON_PACKAGE] = { &ic_parcel, &ic_arrow_down, 2, 0, -7, 5, MOVE_SLIDE, 900 },
    [FH1_ICON_GAMEBOY] = { &ic_gbc, &ic_cartridge, 3, 0, -5, 4, MOVE_SLIDE, 1000 },
    [FH1_ICON_NES] = { &ic_nes, &ic_nes_cart, 3, 0, -5, 6, MOVE_SLIDE, 1000 },
    [FH1_ICON_SMS] = { &ic_sms, &ic_nes_cart, 3, 0, -5, 6, MOVE_SLIDE, 1000 },
    [FH1_ICON_MUSIC] = { &ic_cassette, &ic_cassette_hubs, 3, 6, 0, 0, MOVE_BLINK, 300 },
    [FH1_ICON_MENU_INSTALL] = { &mi_install, NULL, 0, 0, 0, 0, MOVE_NONE, 0 },
    [FH1_ICON_MENU_STORAGE] = { &mi_storage, NULL, 0, 0, 0, 0, MOVE_NONE, 0 },
    [FH1_ICON_MENU_DEVICES] = { &mi_devices, NULL, 0, 0, 0, 0, MOVE_NONE, 0 },
    [FH1_ICON_MENU_IO] = { &mi_io, NULL, 0, 0, 0, 0, MOVE_NONE, 0 },
    [FH1_ICON_MENU_BRIDGE] = { &mi_bridge, NULL, 0, 0, 0, 0, MOVE_NONE, 0 },
    [FH1_ICON_MENU_ABOUT] = { &mi_about, NULL, 0, 0, 0, 0, MOVE_NONE, 0 },
    [FH1_ICON_MENU_NETWORK] = { &mi_network, NULL, 0, 0, 0, 0, MOVE_NONE, 0 },
    [FH1_ICON_MENU_RENAME] = { &mi_rename, NULL, 0, 0, 0, 0, MOVE_NONE, 0 },
    [FH1_ICON_MENU_FORGET] = { &mi_forget, NULL, 0, 0, 0, 0, MOVE_NONE, 0 },
    [FH1_ICON_MENU_USERS] = { &mi_users, NULL, 0, 0, 0, 0, MOVE_NONE, 0 },
    [FH1_ICON_MENU_APPS] = { &mi_apps, NULL, 0, 0, 0, 0, MOVE_NONE, 0 },
    [FH1_ICON_MENU_SYSTEM] = { &mi_system, NULL, 0, 0, 0, 0, MOVE_NONE, 0 },
    [FH1_ICON_MENU_GAMEBOY] = { &mi_gbc, NULL, 0, 0, 0, 0, MOVE_NONE, 0 },
    [FH1_ICON_MENU_NES] = { &mi_nes, NULL, 0, 0, 0, 0, MOVE_NONE, 0 },
    [FH1_ICON_MENU_SMS] = { &mi_sms, NULL, 0, 0, 0, 0, MOVE_NONE, 0 },
    [FH1_ICON_MENU_MUSIC] = { &mi_music, NULL, 0, 0, 0, 0, MOVE_NONE, 0 },
    [FH1_ICON_MENU_PACKAGE] = { &mi_package, NULL, 0, 0, 0, 0, MOVE_NONE, 0 },
    [FH1_ICON_MENU_THEMES] = { &mi_themes, NULL, 0, 0, 0, 0, MOVE_NONE, 0 },
};

static void make_sprites(void)
{
    sprite_t *all[] = { &sp_ball, &sp_sd, &sp_chip, &sp_menu, &sp_start, &sp_select, &sp_lr, &sp_ud,
                        &sx_a, &sx_b, &sx_x, &sx_y, &sn_a, &sn_b, &sn_x, &sn_y,
                        &sp_ps_o, &sp_ps_x, &sp_ps_t, &sp_ps_s,
                        &ic_tray, &ic_arrow_down, &ic_arrow_up, &ic_fw_chip, &ic_fw_arrow, &ic_message,
                        &ic_battery, &ic_battery_bar, &ic_warning, &ic_warning_mark, &ic_parcel, &ic_gbc,
                        &ic_cartridge, &ic_nes, &ic_nes_cart, &ic_sms, &ic_cassette, &ic_cassette_hubs,
                        &mi_install, &mi_storage, &mi_devices, &mi_io, &mi_bridge, &mi_about, &mi_network,
                        &mi_rename, &mi_forget, &mi_users, &mi_apps, &mi_system, &mi_gbc, &mi_nes, &mi_sms,
                        &mi_music, &mi_package, &mi_themes };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) sprite_draw(all[i]);
}

// ── Public helpers ──

void fh1_ui_async(fh1_ui_fn fn, void *arg)
{
    ui_call_t c = { fn, arg };
    if (s_calls) xQueueSend(s_calls, &c, portMAX_DELAY);
}

void fh1_ui_on_devices_changed(fh1_ui_fn fn, void *arg)
{
    if (s_nwatchers < 8) {
        s_watchers[s_nwatchers].fn = fn;
        s_watchers[s_nwatchers].arg = arg;
        s_nwatchers++;
    }
}

void fh1_ui_set_menu_handler(fh1_ui_fn fn, void *arg)
{
    s_menu_fn = fn;
    s_menu_arg = arg;
}

int fh1_ui_scale(void) { return s_u; }
int fh1_ui_width(void) { return s_w / s_u; }
int fh1_ui_height(void) { return s_h / s_u; }

// ── Display port ──

static volatile uint32_t s_ui_frames;    // screens LVGL finished drawing, for the FPS overlay
static lv_obj_t *s_fps;

static void flush_cb(lv_display_t *disp, const lv_area_t *a, uint8_t *px)
{
    fh1_board_flush(a->x1, a->y1, a->x2, a->y2, (const uint16_t *)px);
    if (lv_display_flush_is_last(disp)) s_ui_frames++;
    lv_display_flush_ready(disp);
}

// The FPS overlay: screens drawn per second, top right, over everything.
static void fps_tick(lv_timer_t *t)
{
    static uint32_t last;
    uint32_t n = s_ui_frames;
    if (!lv_obj_is_hidden(s_fps)) lv_label_set_text_fmt(s_fps, "%lu fps", (unsigned long)(n - last));
    last = n;
}

void fh1_ui_set_show_fps(bool on)
{
    fh1_settings_set_u8("ui", "fps", on);
    if (s_fps) lv_obj_set_hidden(s_fps, !on);
}

bool fh1_ui_show_fps(void) { return fh1_settings_get_u8("ui", "fps", 0) != 0; }

static void make_fps_overlay(void)
{
    s_fps = lv_label_create(lv_layer_top());
    lv_obj_set_style_text_font(s_fps, s_font, 0);
    lv_obj_set_style_text_color(s_fps, lv_color_hex(0xFFE000), 0);
    lv_obj_set_style_bg_color(s_fps, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_fps, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(s_fps, 2, 0);
    lv_obj_align(s_fps, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_label_set_text(s_fps, "");
    lv_obj_set_hidden(s_fps, !fh1_ui_show_fps());
    lv_timer_create(fps_tick, 1000, NULL);
}

static uint32_t tick_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// ── Focus and cursors ──

static fh1_win_t *top(void) { return s_nwins ? s_wins[s_nwins - 1] : NULL; }

static void title_frame(fh1_win_t *win);
// The parts of a framed look's frame (frame_render)
#define FR_STRIPS 0x0f
#define FR_CORNERS 0x10
#define FR_TITLE 0x20
#define FR_ALL 0x3f
static void frame_render(fh1_win_t *w, int mask);
static void mote_set(fh1_win_t *w, bool on);

static void restyle(void)
{
    for (int i = 0; i < s_nwins; i++) {
        fh1_win_t *w = s_wins[i];
        bool on = (i == s_nwins - 1);
        if (FRAMED) {
            look_pal_t p;
            look_palette(&p);
            const int st = w->alert ? 2 : on ? 1 : 0;
            lv_obj_set_style_text_color(w->title, lv_color_hex(look_title_ink(s_theme->look, &p, st)), 0);
            if (w->fstate != st) frame_render(w, FR_ALL);
            mote_set(w, on && !w->building);
        } else {
            lv_obj_set_style_text_color(w->title, lv_color_hex(HEX && w->alert ? 0xffffff : on ? s_theme->ink : s_theme->dim), 0);
        }
        if (HEX) {
            lv_obj_set_style_border_color(w->box, lv_color_hex(w->alert ? s_theme->ball : on ? s_theme->ink : s_theme->dim), 0);
            if (w->alert) lv_obj_set_style_border_color(w->inset, lv_color_hex(HEX_ALERT_INSET), 0);
            else lv_obj_remove_local_style_prop(w->inset, LV_STYLE_BORDER_COLOR, 0);
            if (w->band != (w->alert ? 2 : on ? 1 : 0)) title_frame(w);
        } else {
            lv_obj_remove_local_style_prop(w->box, LV_STYLE_BORDER_COLOR, 0);
            lv_obj_remove_local_style_prop(w->inset, LV_STYLE_BORDER_COLOR, 0);
        }
        lv_obj_t *f = lv_group_get_focused(w->group);
        uint32_t n = lv_group_get_obj_count(w->group);
        for (uint32_t k = 0; k < n; k++) {
            lv_obj_t *o = lv_group_get_obj_by_index(w->group, k);
            lv_obj_t *cur = lv_obj_get_child(o, 0);
            if (o == f) {
                lv_image_set_src(cur, &s_cur[on ? 0 : 1]);
                lv_obj_set_hidden(cur, false);
            } else {
                lv_obj_set_hidden(cur, true);
            }
            // The focused item's text slides when it does not fit; the others are cut off.
            lv_obj_t *l = lv_obj_get_child(o, 1);
            if (l && lv_obj_check_type(l, &lv_label_class)) {
                lv_label_long_mode_t m = (o == f && on) ? LV_LABEL_LONG_MODE_SCROLL : LV_LABEL_LONG_MODE_CLIP;
                if (lv_label_get_long_mode(l) != m) lv_label_set_long_mode(l, m);
            }
            // Hex: the item in focus reads amber, the others warm white (plain or rich text: its own colours stay).
            if (l) {
                if (HEX) lv_obj_set_style_text_color(l, lv_color_hex(o == f && on ? s_theme->ink : text_rgb()), 0);
                else if (FRAMED) {
                    look_pal_t p;
                    look_palette(&p);
                    lv_obj_set_style_text_color(l, lv_color_hex(o == f && on ? look_focus_text(s_theme->look, &p) : text_rgb()), 0);
                }
                else lv_obj_remove_local_style_prop(l, LV_STYLE_TEXT_COLOR, 0);
            }
        }
    }
}

static void nudge_tick(lv_timer_t *t)
{
    static bool out;
    out = !out;
    fh1_win_t *w = top();
    lv_obj_t *f = w ? lv_group_get_focused(w->group) : NULL;
    if (f) lv_obj_set_style_translate_x(lv_obj_get_child(f, 0), out ? s_u : 0, 0);
}

static void raise(fh1_win_t *w)
{
    int i;
    for (i = 0; i < s_nwins && s_wins[i] != w; i++) {}
    if (i == s_nwins) return;
    memmove(&s_wins[i], &s_wins[i + 1], (s_nwins - i - 1) * sizeof(s_wins[0]));
    s_wins[s_nwins - 1] = w;
    lv_obj_move_foreground(w->box);
    restyle();
}

// ── Windows ──

static void box_ext_draw(lv_event_t *e)
{
    // half a corner ball past the outer border; framed looks: the corners' flowers, the butterfly
    lv_event_set_ext_draw_size(e, FRAMED ? U(10) + 1 : U(5) + 1);
}

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_scrollable(o, false);
    return o;
}

// ── Corners: a ball on each (Gen), or the corner cut at 45 degrees (Hex) ──
// A Hex corner is a sprite laid over the box's square corner: outside the cut it paints the paper (the desktop and
// the windows are both black there), then the 2-unit border along the cut, the 1-unit gap, and the inner line cut
// parallel to it. At the top the title band takes the inner line's place, so the top corners leave it out.
// One set per state of the window, like its border: dim, in focus, alert (in PSRAM: internal RAM is scarce).
#define CHAMFER 9                           // the corner sprites' size, in units
static uint32_t *s_cham_px[3][4];
static lv_image_dsc_t s_cham[3][4];

static void make_chamfers(void)
{
    const int S = U(CHAMFER);
    for (int k = 0; k < 3; k++) {
    const uint32_t paper = argb(s_theme->paper), inset = argb(k == 2 ? HEX_ALERT_INSET : inset_rgb());
    const uint32_t ink = argb(k == 2 ? s_theme->ball : k == 1 ? s_theme->ink : s_theme->dim);
    for (int i = 0; i < 4; i++) {          // 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right
        if (!s_cham_px[k][i]) s_cham_px[k][i] = heap_caps_malloc(18 * 18 * 4, MALLOC_CAP_SPIRAM);
        uint32_t *px = s_cham_px[k][i];
        if (!px) return;
        for (int py = 0; py < S; py++) {
            for (int px_ = 0; px_ < S; px_++) {
                const int x = ((i & 1) ? S - 1 - px_ : px_) / s_u, y = ((i & 2) ? S - 1 - py : py) / s_u, d = x + y;
                uint32_t c = 0;
                if (d < 5) c = paper;
                else if (d <= 6) c = ink;
                else if (d == 7 && x >= 2 && y >= 2) c = paper;     // inside the border only: no notch in it
                else if (d == 8 && x >= 3 && y >= 3) c = (i & 2) ? inset : 0;
                else if ((x == 3 || y == 3) && d < 8) c = paper;
                px[py * S + px_] = c;
            }
        }
        lv_image_dsc_t *d = &s_cham[k][i];
        d->header.magic = LV_IMAGE_HEADER_MAGIC;
        d->header.cf = LV_COLOR_FORMAT_ARGB8888;
        d->header.w = S;
        d->header.h = S;
        d->header.stride = S * 4;
        d->data_size = S * S * 4;
        d->data = (const uint8_t *)px;
    }
    }
}

static void shake_anim(void *var, int32_t v);
static void stripe_anim(void *var, int32_t v);

// Places corner ornament i (0 top-left .. 3 bottom-right) of a box whose outer edge starts at left/top, w x h,
// in its parent's content coordinates.
static void corner_place(lv_obj_t *b, int i, int left, int top, int w, int h, int state)
{
    lv_anim_delete(b, shake_anim);             // a ball shaking when the theme changes stops, upright
    lv_image_set_src(b, NULL);
    if (HEX) {
        const int S = U(CHAMFER);
        lv_image_set_src(b, &s_cham[state][i]);
        lv_image_set_rotation(b, 0);
        lv_obj_set_pos(b, left + ((i & 1) ? w - S : 0), top + ((i & 2) ? h - S : 0));
    } else {
        const int d = sp_ball.dsc.header.w;
        lv_image_set_src(b, &sp_ball.dsc);
        lv_image_set_pivot(b, d / 2, d / 2);
        lv_obj_set_pos(b, left + ((i & 1) ? w - U(2) : U(2)) - d / 2, top + ((i & 2) ? h - U(2) : U(2)) - d / 2);
    }
}

// Where a window's body starts: under the pill (Gen), or under the band, its line and a gap (Hex).
#define BAND_ROWS 10
static int body_top(void) { return HEX ? U(BAND_ROWS + 3) : U(7); }

// ── The Hex title band ──
// Across the top, one unit inside the border: warning stripes at 45 degrees, 4 units on and 4 off, and the title
// on a black plate leaning with them (a parallelogram as wide as the title on every row), then an amber line.
// Amber in the window in focus, dim in the others, red for an alert; the stripes run in the idle flourish.
static void title_band(fh1_win_t *win)
{
    const int bw = win->cw - U(2), BH = U(BAND_ROWS + 1), tw = lv_obj_get_width(win->title);
    if (bw <= 0) return;
    // Opaque: RGB565, half the bytes of ARGB, copied without blending (it is redrawn as the stripes run).
    uint16_t *px = win->frame_px ? heap_caps_realloc(win->frame_px, bw * BH * 2, MALLOC_CAP_SPIRAM)
                                 : heap_caps_malloc(bw * BH * 2, MALLOC_CAP_SPIRAM);
    if (!px) return;
    win->frame_px = (uint32_t *)px;
    const bool focus = win == top();
    const int state = win->alert ? 2 : focus ? 1 : 0;
    const uint32_t col = state == 2 ? s_theme->ball : state == 1 ? s_theme->ink : s_theme->dim;
    const uint16_t stripe = lv_color_to_u16(lv_color_hex(col)), black = lv_color_to_u16(lv_color_hex(s_theme->paper));
    const int pw = tw + U(6) + U(BAND_ROWS - 1), c = (bw - pw) / 2;
    for (int y = 0; y < BH; y++) {
        const int r = y / s_u;
        uint16_t *row = px + y * bw;
        if (r == BAND_ROWS) {
            for (int x = 0; x < bw; x++) row[x] = stripe;
            continue;
        }
        const int x0 = c + U(BAND_ROWS - 1 - r) - U(4);
        for (int x = 0; x < bw; x++) {
            if (tw > 0 && x >= x0 && x < x0 + pw) row[x] = black;
            else row[x] = ((x / s_u + r + win->phase) % 8) < 4 ? stripe : black;
        }
    }
    win->frame_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    win->frame_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    win->frame_dsc.header.w = bw;
    win->frame_dsc.header.h = BH;
    win->frame_dsc.header.stride = bw * 2;
    win->frame_dsc.data_size = bw * BH * 2;
    win->frame_dsc.data = (const uint8_t *)px;
    if (!win->frame) win->frame = lv_image_create(win->box);
    if (lv_image_get_src(win->frame) == &win->frame_dsc && win->band == state && win->band_w == bw) {
        // The stripes moved: same picture, new pixels; only the band is drawn again (raising the objects again
        // would redraw the whole window).
        lv_obj_invalidate(win->frame);
        lv_obj_set_pos(win->title, U(1) + (bw - tw) / 2, U(1) + U(1) - 1);    // (the title may have changed)
        return;
    } else {
        lv_image_set_src(win->frame, NULL);
        lv_image_set_src(win->frame, &win->frame_dsc);
    }
    win->band_w = bw;
    lv_obj_set_pos(win->frame, U(1), U(1));
    lv_obj_move_foreground(win->frame);
    // The letters' top on the band's second row (the font leaves a blank row above its glyphs).
    lv_obj_set_pos(win->title, U(1) + (bw - tw) / 2, U(1) + U(1) - 1);
    lv_obj_move_foreground(win->title);
    for (int i = 0; i < 4; i++) {
        if (!win->balls[i]) continue;
        if (win->band != state) lv_image_set_src(win->balls[i], &s_cham[state][i]);
        lv_obj_move_foreground(win->balls[i]);
    }
    win->band = state;
}


// ── The framed looks (Garden, Command) ──
// The frame's four sides, its corners and the title's plate are pictures drawn by ui_look.c for the window's size
// and state (dim, in focus, alert), in PSRAM; drawn again when the window's state changes, while it opens or
// closes (frame_build), and in small parts as its decoration runs (look_tick). The box keeps its 2-unit border,
// unseen, so everything inside a window sits where it does in the other looks.

static int win_state(fh1_win_t *w) { return w->alert ? 2 : w == top() ? 1 : 0; }

// Moves an object only if it is somewhere else (a style set anew costs a refresh even when nothing changes).
static void move_to(lv_obj_t *o, int x, int y)
{
    if (lv_obj_get_style_x(o, 0) != x || lv_obj_get_style_y(o, 0) != y) lv_obj_set_pos(o, x, y);
}

static uint32_t *px_buf(uint32_t **pp, size_t bytes)
{
    uint32_t *p = *pp ? heap_caps_realloc(*pp, bytes, MALLOC_CAP_SPIRAM) : heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (p) *pp = p;
    return p;
}

// Shows an ARGB picture: new pixels in the same picture are only redrawn; another size or buffer is set afresh.
static void show_argb(lv_obj_t *img, lv_image_dsc_t *d, uint32_t *px, int w, int h)
{
    const bool same = lv_image_get_src(img) == d && d->header.cf == LV_COLOR_FORMAT_ARGB8888 && d->header.w == w &&
                      d->header.h == h && d->data == (const uint8_t *)px;
    lv_image_cache_drop(d);
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_ARGB8888;
    d->header.w = w;
    d->header.h = h;
    d->header.stride = w * 4;
    d->data_size = w * h * 4;
    d->data = (const uint8_t *)px;
    if (same) {
        lv_obj_invalidate(img);
    } else {
        lv_image_set_src(img, NULL);
        lv_image_set_src(img, d);
    }
}

// How far the frame's sides have got: Garden's vine grows all through the opening, Command's plates close in its
// middle third (frame_build).
static int strip_grow(fh1_win_t *w)
{
    if (s_theme->look != LOOK_SCIFI) return w->grow;
    return w->grow <= 96 ? 0 : w->grow >= 192 ? 256 : (w->grow - 96) * 256 / 96;
}

static void corner_home(fh1_win_t *w, int i, int *x, int *y)
{
    const int S = U(look_corner_size(s_theme->look)), o = U(look_corner_out(s_theme->look));
    *x = (i & 1) ? -U(2) + w->cw + U(4) - S + o : -U(2) - o;
    *y = (i & 2) ? -U(2) + w->ch + U(4) - S + o : -U(2) - o;
}

// Bottom to top: the sides, the mote (Garden: over the corners), the title's plate, the title, the scan line.
static void frame_order(fh1_win_t *w)
{
    for (int k = 0; k < 4; k++) if (w->strip[k]) lv_obj_move_foreground(w->strip[k]);
    const bool fly = s_theme->look == LOOK_GARDEN;         // the butterfly lands on the flowers; the spark goes under
    if (w->mote && !fly) lv_obj_move_foreground(w->mote);
    for (int i = 0; i < 4; i++) lv_obj_move_foreground(w->balls[i]);
    if (w->mote && fly) lv_obj_move_foreground(w->mote);
    if (w->frame) lv_obj_move_foreground(w->frame);
    lv_obj_move_foreground(w->title);
    if (w->scan) lv_obj_move_foreground(w->scan);
}

static void frame_render(fh1_win_t *w, int mask)
{
    const int look = s_theme->look, state = win_state(w), W = w->cw + U(4), H = w->ch + U(4);
    look_pal_t p;
    look_palette(&p);
    bool created = false;
    const int O = U(look_strip_out(look)), T = U(look_strip_thick(look)) + O, g = strip_grow(w);
    for (int side = 0; side < 4; side++) {
        if (!(mask & (1 << side))) continue;
        const bool horiz = side < 2;
        const int sw = horiz ? W : T, sh = horiz ? T : H;
        uint32_t *px = px_buf(&w->strip_px[side], (size_t)sw * sh * 4);
        if (!px) continue;
        look_strip(look, &p, side, horiz ? W : H, W, H, state, g, w->phase, px);
        // Each side is drawn with its outer edge first: the bottom's goes last, the right's on the right.
        if (side == 1) {
            for (int y = 0; y < sh / 2; y++)
                for (int x = 0; x < sw; x++) {
                    const uint32_t t = px[y * sw + x];
                    px[y * sw + x] = px[(sh - 1 - y) * sw + x];
                    px[(sh - 1 - y) * sw + x] = t;
                }
        } else if (side == 3) {
            for (int y = 0; y < sh; y++)
                for (int x = 0; x < sw / 2; x++) {
                    const uint32_t t = px[y * sw + x];
                    px[y * sw + x] = px[y * sw + sw - 1 - x];
                    px[y * sw + sw - 1 - x] = t;
                }
        }
        if (!w->strip[side]) {
            w->strip[side] = lv_image_create(w->box);
            created = true;
        }
        show_argb(w->strip[side], &w->strip_dsc[side], px, sw, sh);
        move_to(w->strip[side], side == 2 ? -U(2) - O : side == 3 ? -U(2) + W - T + O : -U(2),
                       side == 0 ? -U(2) - O : side == 1 ? -U(2) + H - T + O : -U(2));
    }
    if (mask & FR_CORNERS) {
        const int S = U(look_corner_size(look));
        uint32_t *px = px_buf(&w->corner_px, (size_t)S * S * 4 * 4);
        for (int i = 0; px && i < 4; i++) {
            look_corner(look, &p, i, state, w->grow, w->phase, px + i * S * S);
            show_argb(w->balls[i], &w->corner_dsc[i], px + i * S * S, S, S);
            if (!w->building) {
                int x, y;
                corner_home(w, i, &x, &y);
                move_to(w->balls[i], x, y);
            }
        }
    }
    if (mask & FR_TITLE) {
        const int tw = lv_obj_get_width(w->title);
        int pw, ph, x, y, tx, ty;
        look_title_box(look, s_u, w->cw, tw, s_line, &pw, &ph, &x, &y, &tx, &ty);
        uint32_t *px = px_buf(&w->frame_px, (size_t)pw * ph * 4);
        if (px) {
            look_title(look, &p, pw, ph, tw, state, w->phase, px);
            if (!w->frame) {
                w->frame = lv_image_create(w->box);
                created = true;
            }
            show_argb(w->frame, &w->frame_dsc, px, pw, ph);
            move_to(w->frame, x, y);
        }
        move_to(w->title, tx, ty);
        if (w->fstate != state || created)
            lv_obj_set_style_text_color(w->title, lv_color_hex(look_title_ink(look, &p, state)), 0);
    }
    if (created) frame_order(w);
    w->fstate = state;
}

// Back to a plain box (a theme without a drawn frame): the frame's pictures go.
static void frame_drop(fh1_win_t *w)
{
    for (int k = 0; k < 4; k++) {
        if (w->strip[k]) lv_obj_delete(w->strip[k]);
        w->strip[k] = NULL;
        lv_image_cache_drop(&w->strip_dsc[k]);
        heap_caps_free(w->strip_px[k]);
        w->strip_px[k] = NULL;
        lv_image_cache_drop(&w->corner_dsc[k]);
    }
    heap_caps_free(w->corner_px);
    w->corner_px = NULL;
    if (w->mote) lv_obj_delete(w->mote);
    if (w->scan) lv_obj_delete(w->scan);
    w->mote = w->scan = NULL;
}

// The window's contents (body, title, footer) fading in at the end of Command's build.
static void content_opa(fh1_win_t *w, int opa)
{
    if (w->copa == opa) return;
    w->copa = opa;
    lv_obj_t *o[3] = { w->body, w->title, w->footer };
    for (int k = 0; k < 3; k++) {
        if (!o[k]) continue;
        if (opa >= 255) lv_obj_remove_local_style_prop(o[k], LV_STYLE_OPA, 0);
        else lv_obj_set_style_opa(o[k], opa, 0);
    }
}

// The opening (v from 0 to 256) and the closing (back to 0).
// Garden: the window fades in at once while the vine grows round it clockwise from the top-left, and each corner's
// flower opens as the vine reaches it. Command builds itself: the corner brackets fly out from the middle (first
// third), the plates close from the corners (second), the glass lights up behind a scan line and the contents come
// on (last).
static void frame_build(fh1_win_t *w, int v)
{
    const int prev = w->grow, W = w->cw + U(4), H = w->ch + U(4);
    w->grow = v;
    if (s_theme->look == LOOK_GARDEN) {
        lv_obj_set_style_opa(w->box, v >= 64 ? LV_OPA_COVER : v * 4, 0);
        // the sides the vine's tip passed over, and the corners (their flowers)
        const int lw = W / s_u, lh = H / s_u, P = 2 * (lw + lh);
        const int a = P * (prev < v ? prev : v) / 256, b = P * (prev < v ? v : prev) / 256;
        const int s0[4] = { 0, lw + lh, 2 * lw + lh, lw }, s1[4] = { lw, 2 * lw + lh, P, lw + lh };   // top, bottom, left, right
        int mask = FR_CORNERS;
        for (int k = 0; k < 4; k++)
            if (a < s1[k] && b >= s0[k]) mask |= 1 << k;
        frame_render(w, mask);
        return;
    }
    // Command
    const int t = v >= 96 ? 256 : v * 256 / 96, e = 256 - (256 - t) * (256 - t) / 256;   // eased out
    const int S = U(look_corner_size(LOOK_SCIFI));
    for (int i = 0; i < 4; i++) {
        int x, y;
        corner_home(w, i, &x, &y);
        const int fx = (W - S) / 2 - U(2), fy = (H - S) / 2 - U(2);
        lv_obj_set_pos(w->balls[i], fx + (x - fx) * e / 256, fy + (y - fy) * e / 256);
    }
    const int pg = prev <= 96 ? 0 : prev >= 192 ? 256 : (prev - 96) * 256 / 96, g = strip_grow(w);
    if (g != pg) frame_render(w, FR_STRIPS);
    const int glass = v < 192 ? 0 : v >= 256 ? 255 : (v - 192) * 4;
    if (glass >= 255) lv_obj_remove_local_style_prop(w->box, LV_STYLE_BG_OPA, 0);
    else lv_obj_set_style_bg_opa(w->box, glass, 0);
    if (w->frame) {
        if (glass >= 255) lv_obj_remove_local_style_prop(w->frame, LV_STYLE_OPA, 0);
        else lv_obj_set_style_opa(w->frame, glass, 0);
    }
    if (v > 192 && v < 256) {
        if (!w->scan) {
            w->scan = plain(w->box);
            lv_obj_set_style_bg_opa(w->scan, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(w->scan, lv_color_hex(s_theme->ink), 0);
            lv_obj_set_size(w->scan, W - U(12), s_u);
        }
        lv_obj_set_pos(w->scan, U(4), -U(2) + U(3) + (H - U(6)) * (v - 192) / 64);
    } else if (w->scan) {
        lv_obj_delete(w->scan);
        w->scan = NULL;
    }
    content_opa(w, v < 224 ? 0 : v >= 256 ? 255 : (v - 224) * 8);
}

// The opening's end (or its interruption by a theme change): everything as it stays.
static void frame_finish(fh1_win_t *w)
{
    w->building = false;
    w->grow = 256;
    lv_obj_remove_local_style_prop(w->box, LV_STYLE_OPA, 0);
    lv_obj_remove_local_style_prop(w->box, LV_STYLE_BG_OPA, 0);
    if (w->frame) lv_obj_remove_local_style_prop(w->frame, LV_STYLE_OPA, 0);
    content_opa(w, 255);
    if (w->scan) lv_obj_delete(w->scan);
    w->scan = NULL;
}

static void frame_anim(lv_anim_t *a, int32_t v) { frame_build(lv_anim_get_user_data(a), v); }

static void restyle(void);
static void frame_opened(lv_anim_t *a)
{
    fh1_win_t *w = lv_anim_get_user_data(a);
    frame_finish(w);
    frame_render(w, FR_ALL);
    restyle();                          // its mote, if it has the focus
}

static void frame_animate(fh1_win_t *w, int from, int to, int ms, lv_anim_completed_cb_t done)
{
    w->building = true;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, w->box);
    lv_anim_set_custom_exec_cb(&a, frame_anim);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms > 0 ? ms : 1);
    lv_anim_set_user_data(&a, w);
    lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
}

// The mote of the window in focus (a butterfly, a spark): one picture of two, moved by mote_tick.
static uint32_t *s_mote_px[2];
static lv_image_dsc_t s_mote[2];

static void make_motes(void)
{
    look_pal_t p;
    look_palette(&p);
    for (int k = 0; k < 2; k++) {
        lv_image_cache_drop(&s_mote[k]);
        s_mote[k].data = NULL;
        if (!FRAMED) continue;
        if (!s_mote_px[k]) s_mote_px[k] = heap_caps_malloc(16 * 16 * 4, MALLOC_CAP_SPIRAM);
        int w, h;
        if (!s_mote_px[k] || !look_mote(s_theme->look, &p, k, s_mote_px[k], &w, &h)) continue;
        s_mote[k].header.magic = LV_IMAGE_HEADER_MAGIC;
        s_mote[k].header.cf = LV_COLOR_FORMAT_ARGB8888;
        s_mote[k].header.w = w;
        s_mote[k].header.h = h;
        s_mote[k].header.stride = w * 4;
        s_mote[k].data_size = w * h * 4;
        s_mote[k].data = (const uint8_t *)s_mote_px[k];
    }
}

static void mote_set(fh1_win_t *w, bool on)
{
    if (on && !w->mote && s_mote[0].data) {
        w->mote = lv_image_create(w->box);
        lv_image_set_src(w->mote, &s_mote[0]);
        lv_obj_set_hidden(w->mote, true);           // until mote_tick places it
        frame_order(w);
    } else if (!on && w->mote) {
        lv_obj_delete(w->mote);
        w->mote = NULL;
    }
}

// ── The title frame ──
// The title sits in a small pill the window's border runs into: the outer line steps up around the text
// and rounds over it, the inner line dips under it into a rounded cup. Hand-drawn pixel art: the left end
// below, the right end its mirror image, and the middle column repeated under the text. Rows start 4 above
// the box's top edge: the outer line is in rows 4-5, the inner line in row 7.
// '#' ink, '.' paper, ' ' see-through (the box draws itself there).
#define FRAME_ROWS 12
#define FRAME_RISE 4                    // rows above the box's top edge
#define FRAME_PAD 1                     // paper columns between the ends and the text
static const char *const s_frame_end[FRAME_ROWS] = {
    "      ###",
    "     ####",
    "    ##...",
    "    ##...",
    "#####....",
    "####.....",
    ".........",
    "####.....",
    "....#....",
    "....#....",
    "....#....",
    ".....####",
};
static const char s_frame_mid[FRAME_ROWS + 1] = "##.........#";
#define FRAME_END_W 9

static void title_frame(fh1_win_t *win)
{
    if (FRAMED) {
        frame_render(win, FR_TITLE);
        return;
    }
    if (HEX) {
        title_band(win);
        return;
    }
    const int tw = (lv_obj_get_width(win->title) + s_u - 1) / s_u;
    const int W = tw + 2 * (FRAME_END_W + FRAME_PAD), H = FRAME_ROWS;
    if (W > 240) return;
    const int PW = W * s_u, PH = H * s_u;
    uint32_t *px = win->frame_px ? heap_caps_realloc(win->frame_px, PW * PH * 4, MALLOC_CAP_SPIRAM)
                                 : heap_caps_malloc(PW * PH * 4, MALLOC_CAP_SPIRAM);
    if (!px) return;
    win->frame_px = px;
    const uint32_t ink = argb(s_theme->ink), paper = argb(s_theme->paper);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            char ch = x < FRAME_END_W ? s_frame_end[y][x]
                    : x >= W - FRAME_END_W ? s_frame_end[y][W - 1 - x]
                    : s_frame_mid[y];
            uint32_t c = ch == '#' ? ink : ch == '.' ? paper : 0;
            for (int sy = 0; sy < s_u; sy++) {
                for (int sx = 0; sx < s_u; sx++) px[(y * s_u + sy) * PW + x * s_u + sx] = c;
            }
        }
    }
    win->frame_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    win->frame_dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    win->frame_dsc.header.w = PW;
    win->frame_dsc.header.h = PH;
    win->frame_dsc.header.stride = PW * 4;
    win->frame_dsc.data_size = PW * PH * 4;
    win->frame_dsc.data = (const uint8_t *)px;
    if (!win->frame) win->frame = lv_image_create(win->box);
    lv_image_set_src(win->frame, NULL);
    lv_image_set_src(win->frame, &win->frame_dsc);
    // Content coordinates start inside the 2-unit outer line: the box's top edge is at -2.
    const int fx0 = (win->cw - PW) / 2, fy0 = -U(2) - U(FRAME_RISE);
    lv_obj_set_pos(win->frame, fx0, fy0);
    lv_obj_move_foreground(win->frame);
    // The letters' top in row 2, one paper row under the pill's top line. The font leaves one blank row
    // above its glyphs.
    lv_obj_set_pos(win->title, fx0 + (PW - lv_obj_get_width(win->title)) / 2, fy0 + U(2) - 1);
    lv_obj_move_foreground(win->title);
}

static void title_layout(fh1_win_t *win)
{
    lv_obj_update_layout(win->title);
    title_frame(win);
}

// ── Opening: an outline grows from the top-left corner to the full size, then the window appears.
// Closing is the same in reverse: the window goes, its outline shrinks back into the corner. ──

static void open_anim(void *var, int32_t v)
{
    lv_obj_t *ghost = var;
    fh1_win_t *win = lv_obj_get_user_data(ghost);
    const int w = lv_obj_get_width(win->box), h = lv_obj_get_height(win->box);
    lv_obj_set_pos(ghost, lv_obj_get_x(win->box), lv_obj_get_y(win->box));
    lv_obj_set_size(ghost, U(12) + (w - U(12)) * v / 256, U(12) + (h - U(12)) * v / 256);
}

static void open_done(lv_anim_t *a)
{
    fh1_win_t *win = lv_anim_get_user_data(a);
    lv_obj_delete(win->ghost);
    win->ghost = NULL;
    lv_obj_set_style_opa(win->box, LV_OPA_COVER, 0);
}

static void win_destroy(void *p);

static void close_done(lv_anim_t *a) { lv_async_call(win_destroy, lv_anim_get_user_data(a)); }

static void close_animated(fh1_win_t *win)
{
    if (FRAMED) {                 // the opening in reverse, from wherever it got to
        lv_anim_delete(win->box, NULL);
        mote_set(win, false);
        const int from = win->grow;
        frame_animate(win, from, 0, 350 * from / 256, close_done);
        return;
    }
    int from = 256;
    if (win->ghost) {             // still opening: shrink from where it got to
        lv_anim_delete(win->ghost, open_anim);
        const int w = lv_obj_get_width(win->box);
        from = w > U(12) ? (lv_obj_get_width(win->ghost) - U(12)) * 256 / (w - U(12)) : 0;
    } else {
        win->ghost = plain(lv_screen_active());
        lv_obj_add_style(win->ghost, &st_box, 0);
        lv_obj_set_user_data(win->ghost, win);
    }
    lv_obj_set_hidden(win->box, true);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, win->ghost);
    lv_anim_set_exec_cb(&a, open_anim);
    lv_anim_set_values(&a, from, 0);
    lv_anim_set_duration(&a, 300 * from / 256);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_user_data(&a, win);
    lv_anim_set_completed_cb(&a, close_done);
    open_anim(win->ghost, from);
    lv_anim_start(&a);
}

static void open_animated(fh1_win_t *win)
{
    ui_sound(s_theme->voice, UI_SND_OPEN);
    if (FRAMED) {
        win->grow = 0;
        frame_build(win, 0);
        frame_animate(win, 0, 256, s_theme->look == LOOK_GARDEN ? 700 : 650, frame_opened);
        return;
    }
    // Invisible, not hidden, while the outline grows: LVGL gives no focus to what is inside a hidden object,
    // and the window's first item must be focused (cursor shown, A works) as soon as it is built.
    lv_obj_set_style_opa(win->box, LV_OPA_TRANSP, 0);
    win->ghost = plain(lv_screen_active());
    lv_obj_add_style(win->ghost, &st_box, 0);
    lv_obj_set_user_data(win->ghost, win);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, win->ghost);
    lv_anim_set_exec_cb(&a, open_anim);
    lv_anim_set_values(&a, 0, 256);
    lv_anim_set_duration(&a, 300);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_user_data(&a, win);
    lv_anim_set_completed_cb(&a, open_done);
    open_anim(win->ghost, 0);
    lv_anim_start(&a);
}

fh1_win_t *fh1_win_create(const char *title, int w, int h)
{
    if (s_nwins >= MAX_WINS) return NULL;
    w = U(w);
    h = U(h);
    const int top_y = s_bar_h, bottom_y = s_h - s_bar_h;    // between the two bars
    const int right = s_w - U(5), bottom = bottom_y - U(6);     // leave room for the corner balls
    if (w > right - U(5)) w = right - U(5);
    if (h > bottom - top_y - U(8)) h = bottom - top_y - U(8);
    // Cascade: each window opens a step down and right of the last. One that would run off the screen is
    // narrowed (or shortened) to fit, so the stack stays visible; only when that would cost more than a
    // quarter of it does a new cascade start at the corner.
    int x = U(5) + U(10) * s_cascade, y = top_y + U(8) + U(10) * s_cascade;
    if ((x + w > right && right - x < w * 3 / 4) || (y + h > bottom && bottom - y < h * 3 / 4)) {
        s_cascade = 0;
        x = U(5);
        y = top_y + U(8);
    }
    if (x + w > right) w = right - x;
    if (y + h > bottom) h = bottom - y;
    s_cascade = (s_cascade + 1) % 8;

    fh1_win_t *win = calloc(1, sizeof(*win));
    win->box = plain(lv_screen_active());
    lv_obj_add_style(win->box, &st_box, 0);
    // The corner balls and the title sit on the border line, half outside the box: let them draw there,
    // and grow the box's drawing area so those parts are redrawn too.
    lv_obj_set_overflow_visible(win->box, true);
    lv_obj_add_event_cb(win->box, box_ext_draw, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
    lv_obj_refresh_ext_draw_size(win->box);
    lv_obj_set_pos(win->box, x, y);
    lv_obj_set_size(win->box, w, h);

    // Content coordinates start inside the 2-unit outer border.
    const int cw = w - U(4), ch = h - U(4);
    win->cw = cw;
    win->ch = ch;
    lv_obj_t *inset = win->inset = plain(win->box);
    lv_obj_add_style(inset, &st_inset, 0);
    lv_obj_set_pos(inset, U(1), U(1));
    lv_obj_set_size(inset, cw - U(2), ch - U(2));
    // An ornament on each corner of the double border (corner_place). Content coordinates start inside the outer
    // line: the box's outer edge is at -2.
    win->band = -1;
    win->fstate = -1;
    win->copa = 255;
    if (FRAMED) lv_obj_set_hidden(inset, true);
    for (int i = 0; i < 4; i++) {
        win->balls[i] = lv_image_create(win->box);
        if (!FRAMED) corner_place(win->balls[i], i, -U(2), -U(2), w, h, 1);
    }

    win->title = lv_label_create(win->box);
    lv_obj_add_style(win->title, &st_title, 0);
    lv_label_set_text(win->title, title);
    title_layout(win);

    win->body = plain(win->box);
    win->body_y = body_top();
    lv_obj_set_pos(win->body, U(3), win->body_y);
    lv_obj_set_size(win->body, cw - U(6), ch - win->body_y - U(3));
    lv_obj_set_style_pad_all(win->body, U(1), 0);
    lv_obj_set_style_pad_row(win->body, U(1), 0);
    lv_obj_set_flex_flow(win->body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(win->body, true);
    lv_obj_set_scrollbar_mode(win->body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(win->body, LV_DIR_VER);

    win->group = lv_group_create();
    lv_group_set_wrap(win->group, true);

    s_wins[s_nwins++] = win;
    restyle();
    open_animated(win);
    return win;
}

// After a theme change: the body moves under the new title, the corners change, the title is drawn again.
static void win_relayout(fh1_win_t *w)
{
    w->body_y = body_top();
    const int bottom = w->footer ? lv_obj_get_y(w->footer) : w->ch - U(3);
    lv_obj_set_y(w->body, w->body_y);
    lv_obj_set_height(w->body, bottom - w->body_y);
    lv_obj_refresh_ext_draw_size(w->box);
    lv_obj_set_hidden(w->inset, FRAMED);
    lv_image_cache_drop(&w->frame_dsc);
    if (w->building) {                  // a theme change while it opens: open at once
        lv_anim_delete(w->box, NULL);
        frame_finish(w);
    }
    if (FRAMED) {
        if (w->mote) {
            lv_obj_delete(w->mote);     // restyle gives it the new look's mote
            w->mote = NULL;
        }
        for (int i = 0; i < 4; i++) {
            lv_anim_delete(w->balls[i], shake_anim);
            lv_image_set_rotation(w->balls[i], 0);
        }
        w->grow = 256;
        frame_render(w, FR_ALL);
        return;
    }
    frame_drop(w);
    for (int i = 0; i < 4; i++) corner_place(w->balls[i], i, -U(2), -U(2), w->cw + U(4), w->ch + U(4), w == top() ? 1 : 0);
    w->band = -1;
    title_frame(w);
}

lv_obj_t *fh1_win_body(fh1_win_t *win) { return win->body; }
void fh1_win_set_title(fh1_win_t *win, const char *title)
{
    if (!lv_obj_check_type(win->title, &lv_label_class)) {     // was rich text: back to a plain label
        lv_obj_delete(win->title);
        win->title = lv_label_create(win->box);
        lv_obj_add_style(win->title, &st_title, 0);
    }
    lv_label_set_text(win->title, title);
    title_layout(win);
    restyle();
}

void fh1_win_set_title_rich(fh1_win_t *win, const fh1_ui_span_t *spans, int n)
{
    lv_obj_delete(win->title);
    win->title = fh1_ui_rich_text(win->box, spans, n);
    lv_obj_add_style(win->title, &st_title, 0);
    title_layout(win);
    restyle();
}
void fh1_win_focus(fh1_win_t *win) { raise(win); }
void fh1_win_on_key(fh1_win_t *win, fh1_win_key_cb cb, void *user) { win->key_cb = cb; win->key_user = user; }
void fh1_win_on_close(fh1_win_t *win, fh1_win_close_cb cb, void *user) { win->close_cb = cb; win->close_user = user; }
void fh1_win_set_owner(fh1_win_t *win, void *owner) { win->owner = owner; }

fh1_win_t *fh1_win_top(void) { return top(); }

bool fh1_win_is_open(fh1_win_t *win)
{
    for (int i = 0; i < s_nwins; i++) {
        if (s_wins[i] == win) return true;
    }
    return false;
}

static void views_reset(fh1_win_t *win);

void fh1_win_clear(fh1_win_t *win)
{
    lv_obj_clean(win->body);
    lv_group_remove_all_objs(win->group);
    views_reset(win);
}

static void win_destroy(void *p)
{
    lv_anim_delete(p, stripe_anim);
    fh1_win_t *win = p;
    if (win->ghost) lv_obj_delete(win->ghost);      // its animations go with it
    free(win->list);
    free(win->views);
    lv_group_delete(win->group);
    frame_drop(win);
    lv_obj_delete(win->box);
    lv_image_cache_drop(&win->frame_dsc);
    heap_caps_free(win->frame_px);
    free(win);
}

void fh1_win_close(fh1_win_t *win)
{
    int i;
    for (i = 0; i < s_nwins && s_wins[i] != win; i++) {}
    if (i == s_nwins) return;
    memmove(&s_wins[i], &s_wins[i + 1], (s_nwins - i - 1) * sizeof(s_wins[0]));
    s_nwins--;
    if (s_nwins == 0) s_cascade = 0;
    // Hidden now, its outline shrinks away, then it is deleted: a window may be closed from its own
    // button's callback, so nothing is deleted here. While the screen belongs to a program, no animation.
    restyle();
    if (win->close_cb) win->close_cb(win, win->close_user);
    if (s_paused) {
        lv_obj_set_hidden(win->box, true);
        if (win->ghost) lv_obj_set_hidden(win->ghost, true);
        lv_async_call(win_destroy, win);
    } else {
        close_animated(win);
    }
}

size_t fh1_win_count_owned(void *owner)
{
    size_t n = 0;
    for (int i = 0; i < s_nwins; i++) {
        if (s_wins[i]->owner == owner) n++;
    }
    return n;
}

void fh1_win_close_owned(void *owner)
{
    for (int i = s_nwins - 1; i >= 0; i--) {
        if (i < s_nwins && s_wins[i]->owner == owner) fh1_win_close(s_wins[i]);
    }
}

// ── Widgets ──

typedef struct {
    void (*cb)(void *user);
    void *user;
} btn_cb_t;

static void btn_event(lv_event_t *e)
{
    btn_cb_t *bc = lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_DELETE) {
        free(bc);
    } else if (lv_event_get_code(e) == LV_EVENT_CLICKED && bc->cb) {
        bc->cb(bc->user);
    }
}

lv_obj_t *fh1_ui_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_add_style(l, &st_text, 0);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(l, text);
    return l;
}

// Text that slides sideways, back and forth, when it is longer than the room it has: slowly (16 units a
// second), resting 2 s at each end, so it can be read. The label's own scrolling, with these timings.
static lv_anim_t s_slide;

static void slide_style(lv_obj_t *l)
{
    if (!s_slide.repeat_delay) {
        lv_anim_init(&s_slide);
        lv_anim_set_repeat_count(&s_slide, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_reverse_delay(&s_slide, 2000);
        lv_anim_set_repeat_delay(&s_slide, 2000);
        lv_anim_set_delay(&s_slide, 2000);       // starts after a rest too
    }
    lv_obj_set_style_anim(l, &s_slide, 0);
    lv_obj_set_style_anim_duration(l, lv_anim_speed_clamped(U(16), 500, 10000), 0);   // LVGL's longest
}

static lv_obj_t *line_label(lv_obj_t *parent)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_add_style(l, &st_text, 0);
    slide_style(l);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_SCROLL);
    return l;
}

lv_obj_t *fh1_ui_line(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = line_label(parent);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_text(l, text);
    return l;
}

lv_obj_t *fh1_ui_value(lv_obj_t *parent, const char *name)
{
    lv_obj_t *row = plain(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, U(3), 0);
    lv_obj_t *n = lv_label_create(row);              // child 0: the name, in the meters' column
    lv_obj_add_style(n, &st_text, 0);
    lv_obj_set_style_text_color(n, lv_color_hex(s_theme->dim), 0);
    lv_label_set_text(n, name);
    lv_obj_set_width(n, U(60));
    lv_obj_t *v = line_label(row);                   // child 1: the value
    lv_obj_set_flex_grow(v, 1);
    lv_label_set_text(v, "");
    return row;
}

void fh1_ui_value_set(lv_obj_t *row, const char *text)
{
    lv_obj_t *v = lv_obj_get_child(row, 1);
    if (strcmp(lv_label_get_text(v), text) != 0) lv_label_set_text(v, text);    // the slide starts over on a change
}

lv_obj_t *fh1_ui_button(fh1_win_t *win, lv_obj_t *parent, const char *text, void (*cb)(void *user), void *user)
{
    lv_obj_t *b = plain(parent ? parent : win->body);
    lv_obj_add_style(b, &st_item, 0);
    lv_obj_set_size(b, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_t *cur = lv_image_create(b);           // child 0: the cursor
    lv_obj_add_style(cur, &st_cursor, 0);
    lv_image_set_src(cur, &s_cur[0]);
    lv_obj_align(cur, LV_ALIGN_LEFT_MID, cursor_ofs(), 0);
    lv_obj_set_hidden(cur, true);
    lv_obj_t *l = lv_label_create(b);             // child 1: the text
    lv_label_set_text(l, text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_CLIP);
    slide_style(l);                               // slides when focused (restyle())
    lv_obj_set_width(l, LV_PCT(100));
    btn_cb_t *bc = calloc(1, sizeof(*bc));
    bc->cb = cb;
    bc->user = user;
    lv_obj_add_event_cb(b, btn_event, LV_EVENT_ALL, bc);
    lv_obj_set_state_user_1(b, true);        // focusable: found again when a view is shown (a marker; no style uses it)
    lv_group_add_obj(win->group, b);
    restyle();
    return b;
}

// ── Hopping letters: a wave runs through each group's marked letters, one every 100 ms, then all rest for
// 500 ms. One shared timer; each tick redraws at most two small labels per group, and nothing while paused.

#define MAX_HOP_GROUPS 8
#define MAX_HOP_LETTERS 12
#define HOP_STEP_MS 100
#define HOP_REST_TICKS 5        // 5 x 100 ms between waves
typedef struct {
    lv_obj_t *letters[MAX_HOP_LETTERS];
    int n, cur;                 // cur: the letter up, or n.. while resting
} hop_group_t;
static hop_group_t *s_hops[MAX_HOP_GROUPS];
static lv_timer_t *s_hop_timer;

static void hop_tick(lv_timer_t *t)
{
    for (int g = 0; g < MAX_HOP_GROUPS; g++) {
        hop_group_t *h = s_hops[g];
        if (!h || !h->n) continue;
        if (h->cur < h->n) lv_obj_set_style_translate_y(h->letters[h->cur], 0, 0);
        h->cur = (h->cur + 1) % (h->n + HOP_REST_TICKS);
        if (h->cur < h->n) lv_obj_set_style_translate_y(h->letters[h->cur], -U(1), 0);
    }
}

static void hop_deleted(lv_event_t *e)
{
    hop_group_t *h = lv_event_get_user_data(e);
    for (int g = 0; g < MAX_HOP_GROUPS; g++) {
        if (s_hops[g] == h) s_hops[g] = NULL;
    }
    free(h);
}

lv_obj_t *fh1_ui_rich_text(lv_obj_t *parent, const fh1_ui_span_t *spans, int n)
{
    lv_obj_t *row = plain(parent);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    hop_group_t *h = NULL;
    int max_bl = 0;
    for (int i = 0; i < n; i++) {
        const lv_font_t *f = spans[i].font ? spans[i].font : s_font;
        if (f->base_line > max_bl) max_bl = f->base_line;
    }
    for (int i = 0; i < n; i++) {
        lv_obj_t *l = lv_label_create(row);
        lv_label_set_text(l, spans[i].text);
        // A label sized to its text drops trailing spaces: keep them as padding.
        size_t len = strlen(spans[i].text), sp = 0;
        while (sp < len && spans[i].text[len - 1 - sp] == ' ') sp++;
        if (sp) {
            const lv_font_t *f = spans[i].font ? spans[i].font : s_font;
            lv_obj_set_style_pad_right(l, sp * lv_font_get_glyph_width(f, ' ', 0), 0);
        }
        if (spans[i].color) lv_obj_set_style_text_color(l, lv_color_hex(spans[i].color & 0xFFFFFF), 0);
        if (spans[i].font) lv_obj_set_style_text_font(l, spans[i].font, 0);
        // Bottoms are aligned: lift the parts whose font sits lower so all baselines line up.
        const lv_font_t *f = spans[i].font ? spans[i].font : s_font;
        if (f->base_line < max_bl) lv_obj_set_style_pad_bottom(l, max_bl - f->base_line, 0);
        if (spans[i].hop) {
            if (!h) h = calloc(1, sizeof(*h));
            if (h && h->n < MAX_HOP_LETTERS) h->letters[h->n++] = l;
        }
    }
    if (h) {
        int g = 0;
        while (g < MAX_HOP_GROUPS && s_hops[g]) g++;
        if (g == MAX_HOP_GROUPS) {
            free(h);
        } else {
            s_hops[g] = h;
            lv_obj_add_event_cb(row, hop_deleted, LV_EVENT_DELETE, h);
            if (!s_hop_timer) s_hop_timer = lv_timer_create(hop_tick, HOP_STEP_MS, NULL);
        }
    }
    return row;
}

lv_obj_t *fh1_ui_button_spans(fh1_win_t *win, lv_obj_t *parent, const fh1_ui_span_t *spans, int n,
                              void (*cb)(void *user), void *user)
{
    lv_obj_t *b = fh1_ui_button(win, parent, "", cb, user);
    lv_obj_t *label = lv_obj_get_child(b, 1);
    lv_obj_t *row = fh1_ui_rich_text(b, spans, n);   // replaces the label as child 1
    lv_obj_move_to_index(row, 1);
    lv_obj_delete(label);
    return b;
}

void fh1_ui_button_set_text(lv_obj_t *btn, const char *text)
{
    lv_label_set_text(lv_obj_get_child(btn, 1), text);
}

void fh1_ui_dim(lv_obj_t *obj, bool dim)
{
    if (dim) lv_obj_add_style(obj, &st_dim, 0);
    else lv_obj_remove_style(obj, &st_dim, 0);
}

int fh1_ui_focused_index(fh1_win_t *win)
{
    lv_obj_t *f = lv_group_get_focused(win->group);
    for (uint32_t i = 0; f && i < lv_group_get_obj_count(win->group); i++) {
        if (lv_group_get_obj_by_index(win->group, i) == f) return (int)i;
    }
    return -1;
}

void fh1_ui_focus_index(fh1_win_t *win, int index)
{
    uint32_t n = lv_group_get_obj_count(win->group);
    if (n == 0 || index < 0) return;
    if ((uint32_t)index >= n) index = n - 1;
    lv_obj_t *o = lv_group_get_obj_by_index(win->group, index);
    lv_group_focus_obj(o);
    lv_obj_scroll_to_view(o, LV_ANIM_OFF);
    restyle();
}

// ── Button chips and window footers ──
// Each physical button has a chip, drawn in the style of player 1's controller (fh1_gamepad_style): the FH1's
// caps by colour (A red, B yellow, X blue, Y green), an Xbox controller's letters in their colours, a Nintendo
// one's letters, a PlayStation one's shapes, each for the button in the place the FH1 key comes from (A: the
// right face button: B on an Xbox pad, the circle on a PlayStation one). Start and Select are white pills with
// an icon; Menu is a dark pill with a "three bars" icon. Chips change with the controller (checked twice a
// second).

// The face buttons per style: the chip's colour and its letter or shape (NULL: colour alone).
static void face_look(int style, int key, uint32_t *bg, sprite_t **icon)
{
    static const uint32_t caps[4] = { 0xe5302c, 0xf4c21f, 0x2f6fe8, 0x2fb36a };      // A, B, X, Y
    static sprite_t *const xbox[4] = { &sx_b, &sx_a, &sx_y, &sx_x };                  // east, south, north, west
    static sprite_t *const nintendo[4] = { &sn_a, &sn_b, &sn_x, &sn_y };
    static sprite_t *const ps[4] = { &sp_ps_o, &sp_ps_x, &sp_ps_t, &sp_ps_s };
    const int i = key - FH1_KEY_A;
    *bg = 0x26282c;
    *icon = NULL;
    if (style == FH1_KEYS_XBOX) *icon = xbox[i];
    else if (style == FH1_KEYS_NINTENDO) *icon = nintendo[i];
    else if (style == FH1_KEYS_PLAYSTATION) *icon = ps[i];
    else *bg = caps[i];
}

static void key_look(int style, int key, uint32_t *bg, sprite_t **icon)
{
    if (key >= FH1_KEY_A && key <= FH1_KEY_Y) {
        face_look(style, key, bg, icon);
        return;
    }
    *bg = key == FH1_KEY_MENU ? 0x26282c : 0xf2f2f2;
    *icon = key == FH1_KEY_START ? &sp_start : key == FH1_KEY_SELECT ? &sp_select : &sp_menu;
}

#define MAX_CHIPS 48
static struct { lv_obj_t *obj; int key; } s_chips[MAX_CHIPS];
static int s_chip_style = -1;
static lv_timer_t *s_chip_timer;

static void chip_decorate(lv_obj_t *c, int key, int style)
{
    uint32_t bg;
    sprite_t *icon;
    key_look(style, key, &bg, &icon);
    lv_obj_clean(c);
    const int h = U(11);
    int w = h;
    if (icon && (key < FH1_KEY_A || key > FH1_KEY_Y)) w = icon->dsc.header.w + U(6);   // a pill round its icon
    if (!(w & 1)) w++;                     // odd widths centre an odd icon exactly
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, s_u, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(s_theme->ink), 0);
    if (icon) {
        lv_obj_t *im = lv_image_create(c);
        lv_image_set_src(im, &icon->dsc);
        lv_obj_center(im);
    }
}

static void chip_deleted(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    for (int i = 0; i < MAX_CHIPS; i++)
        if (s_chips[i].obj == o) s_chips[i].obj = NULL;
}

static void chip_watch(lv_timer_t *t)
{
    const int style = fh1_gamepad_style(0);
    if (style == s_chip_style) return;
    s_chip_style = style;
    for (int i = 0; i < MAX_CHIPS; i++)
        if (s_chips[i].obj) chip_decorate(s_chips[i].obj, s_chips[i].key, style);
}

lv_obj_t *fh1_ui_key_chip(lv_obj_t *parent, int key)
{
    lv_obj_t *c = plain(parent);
    if (key == FH1_HINT_LEFTRIGHT || key == FH1_HINT_UPDOWN) {     // the D-pad: its own shape, no pill
        sprite_t *sp = key == FH1_HINT_LEFTRIGHT ? &sp_lr : &sp_ud;
        lv_obj_set_size(c, sp->dsc.header.w, sp->dsc.header.h);
        lv_obj_t *im = lv_image_create(c);
        lv_image_set_src(im, &sp->dsc);
        return c;
    }
    if (key < FH1_KEY_A || key > FH1_KEY_MENU) key = FH1_KEY_MENU;
    if (s_chip_style < 0) s_chip_style = fh1_gamepad_style(0);
    if (!s_chip_timer) s_chip_timer = lv_timer_create(chip_watch, 500, NULL);
    chip_decorate(c, key, s_chip_style);
    for (int i = 0; i < MAX_CHIPS; i++)
        if (!s_chips[i].obj) {
            s_chips[i].obj = c;
            s_chips[i].key = key;
            lv_obj_add_event_cb(c, chip_deleted, LV_EVENT_DELETE, NULL);
            break;
        }
    return c;
}

// The same chips for full-screen apps (SDK 0.8), into an RGB565 buffer at 1 pixel a unit: 11 pixels high, the
// face buttons round, the others pills. Returns the x after it.
static uint16_t to565(uint32_t argb) { return (uint16_t)(((argb >> 8) & 0xF800) | ((argb >> 5) & 0x07E0) | ((argb >> 3) & 0x001F)); }

static void px_sprite(uint16_t *px, int w, int h, int x, int y, const sprite_t *sp)
{
    const int sw = strlen(sp->rows[0]);
    for (int r = 0; r < sp->h; r++)
        for (int c = 0; c < sw; c++) {
            const uint32_t v = sprite_colour(sp->rows[r][c]);
            const int xx = x + c, yy = y + r;
            if ((v >> 24) && xx >= 0 && yy >= 0 && xx < w && yy < h) px[yy * w + xx] = to565(v);
        }
}

int fh1_px_key(uint16_t *px, int w, int h, int x, int y, int key)
{
    if (key == FH1_HINT_LEFTRIGHT || key == FH1_HINT_UPDOWN) {
        px_sprite(px, w, h, x, y, key == FH1_HINT_LEFTRIGHT ? &sp_lr : &sp_ud);
        return x + 11;
    }
    if (key < FH1_KEY_A || key > FH1_KEY_MENU) return x;
    uint32_t bg;
    sprite_t *icon;
    key_look(fh1_gamepad_style(0), key, &bg, &icon);
    const bool round = key >= FH1_KEY_A && key <= FH1_KEY_Y;
    const int iw = icon ? (int)strlen(icon->rows[0]) : 0, ih = icon ? icon->h : 0;
    const int cw = round ? 11 : iw + 6, ch = 11;
    const uint16_t fill = to565(0xff000000u | bg), edge = to565(0xff000000u | 0x101014);
    // A rounded box: corners cut by distance from the nearest end circle's centre.
    for (int yy = 0; yy < ch; yy++)
        for (int xx = 0; xx < cw; xx++) {
            const int cx = xx < 5 ? 5 : xx > cw - 6 ? cw - 6 : xx;
            const int dx = 2 * (xx - cx), dy = 2 * yy - 10, d = dx * dx + dy * dy;
            if (d > 121) continue;
            const int X = x + xx, Y = y + yy;
            if (X >= 0 && Y >= 0 && X < w && Y < h) px[Y * w + X] = d > 81 ? edge : fill;
        }
    if (icon) px_sprite(px, w, h, x + (cw - iw) / 2, y + (ch - ih) / 2, icon);
    return x + cw;
}

// ── Icons ──

static void icon_slide(void *var, int32_t v) { lv_obj_set_y(var, v); }
static void icon_blink(void *var, int32_t v) { lv_obj_set_style_image_opa(var, v ? LV_OPA_TRANSP : LV_OPA_COVER, 0); }

lv_obj_t *fh1_ui_icon(lv_obj_t *parent, fh1_ui_icon_t icon, bool big, bool animated)
{
    if ((unsigned)icon >= FH1_ICON_COUNT) icon = FH1_ICON_WARNING;
    const icon_def_t *d = &s_icon_defs[icon];
    const int z = big ? 2 * s_u : s_u;
    lv_obj_t *box = plain(parent);                 // clips the moving part to the icon's square
    lv_obj_set_size(box, strlen(d->still->rows[0]) * z, d->still->h * z);
    lv_obj_t *part = NULL;
    if (d->part) {
        part = lv_image_create(box);
        lv_image_set_src(part, big ? &d->part->dsc2 : &d->part->dsc);
        lv_obj_set_pos(part, d->px * z, d->py * z);
    }
    lv_obj_t *still = lv_image_create(box);
    lv_image_set_src(still, big ? &d->still->dsc2 : &d->still->dsc);
    if (!animated || d->move == MOVE_NONE) return box;
    lv_obj_t *mover = part ? part : still;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, mover);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_duration(&a, d->ms);
    switch (d->move) {
    case MOVE_SLIDE:
        lv_anim_set_exec_cb(&a, icon_slide);
        lv_anim_set_values(&a, d->from * z, d->to * z);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
        lv_anim_set_repeat_delay(&a, 150);
        break;
    case MOVE_BOB:
        lv_anim_set_exec_cb(&a, icon_slide);
        lv_anim_set_values(&a, 0, -d->to * z);
        lv_anim_set_reverse_duration(&a, d->ms);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        break;
    default:
        lv_anim_set_exec_cb(&a, icon_blink);
        lv_anim_set_values(&a, 0, 1);
        lv_anim_set_reverse_duration(&a, d->ms);
        lv_anim_set_path_cb(&a, lv_anim_path_step);
        break;
    }
    lv_anim_start(&a);
    return box;
}

lv_obj_t *fh1_ui_button_lead_icon(lv_obj_t *btn, fh1_ui_icon_t icon)
{
    lv_obj_t *im = fh1_ui_icon(btn, icon, false, false);         // after the text: child 1 stays the text
    lv_obj_align(im, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_pad_left(lv_obj_get_child(btn, 1), U(13), 0);
    return im;
}

// ── Pixel pictures made at run time (avatars) ──
typedef struct {
    lv_image_dsc_t dsc;
    uint32_t *buf;
    int w, h, z;
} pixels_t;

static void pixels_deleted(lv_event_t *e)
{
    pixels_t *p = lv_event_get_user_data(e);
    lv_image_cache_drop(&p->dsc);
    heap_caps_free(p->buf);
    free(p);
}

lv_obj_t *fh1_ui_pixels(lv_obj_t *parent, int w, int h, int zoom)
{
    pixels_t *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->w = w, p->h = h, p->z = zoom * s_u;
    const int W = w * p->z, H = h * p->z;
    p->buf = heap_caps_calloc(W * H, 4, MALLOC_CAP_SPIRAM);       // internal RAM is for Wi-Fi
    if (!p->buf) {
        free(p);
        return NULL;
    }
    p->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    p->dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    p->dsc.header.w = W;
    p->dsc.header.h = H;
    p->dsc.header.stride = W * 4;
    p->dsc.data_size = W * H * 4;
    p->dsc.data = (const uint8_t *)p->buf;
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, &p->dsc);
    lv_obj_set_user_data(img, p);
    lv_obj_add_event_cb(img, pixels_deleted, LV_EVENT_DELETE, p);
    return img;
}

void fh1_ui_pixels_set(lv_obj_t *img, const uint32_t *argb)
{
    pixels_t *p = img ? lv_obj_get_user_data(img) : NULL;
    if (!p) return;
    const int W = p->w * p->z;
    for (int y = 0; y < p->h; y++) {
        uint32_t *row = p->buf + (size_t)y * p->z * W;
        const uint32_t *src = argb + (size_t)y * p->w;
        for (int x = 0; x < p->w; x++)
            for (int k = 0; k < p->z; k++) row[x * p->z + k] = src[x];
        for (int k = 1; k < p->z; k++) memcpy(row + (size_t)k * W, row, W * 4);
    }
    lv_image_cache_drop(&p->dsc);
    lv_obj_invalidate(img);
}

lv_obj_t *fh1_ui_button_lead_pixels(lv_obj_t *btn, int w, int h)
{
    lv_obj_t *im = fh1_ui_pixels(btn, w, h, 1);
    if (!im) return NULL;
    lv_obj_align(im, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_pad_left(lv_obj_get_child(btn, 1), U(w + 4), 0);
    lv_obj_set_style_min_height(btn, U(h + 2), 0);
    lv_obj_t *l = lv_obj_get_child(btn, 1);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    return im;
}

lv_obj_t *fh1_ui_button_icon(lv_obj_t *btn, fh1_ui_icon_t icon)
{
    lv_obj_t *im = fh1_ui_icon(btn, icon, false, false);
    lv_obj_align(im, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_pad_right(btn, U(11), 0);     // keep the text clear of it
    return im;
}

// The first FH1_HINT_CHARS characters of a hint's word (UTF-8: accents count as one).
static void hint_word(const char *src, char *out, size_t n)
{
    size_t o = 0;
    int chars = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && chars < FH1_HINT_CHARS;) {
        int len = *p < 0x80 ? 1 : (*p >> 5) == 6 ? 2 : (*p >> 4) == 14 ? 3 : 4;
        if (o + len >= n) break;
        memcpy(out + o, p, len);
        o += len;
        p += len;
        chars++;
    }
    out[o] = 0;
}

// The footer: actions in two columns, a chip and one short word each, the same grid in every window.
void fh1_win_set_hints(fh1_win_t *win, const fh1_hint_t *hints, int n)
{
    if (win->footer) lv_obj_delete(win->footer);
    win->footer = plain(win->box);
    const int fw = win->cw - U(6);
    lv_obj_set_width(win->footer, fw);
    lv_obj_set_height(win->footer, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(win->footer, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_top(win->footer, U(2), 0);
    lv_obj_set_style_pad_row(win->footer, U(1), 0);
    lv_obj_set_style_border_side(win->footer, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(win->footer, s_u, 0);
    lv_obj_set_style_border_color(win->footer, lv_color_hex(s_theme->dim), 0);
    for (int i = 0; i < n; i++) {
        lv_obj_t *cell = plain(win->footer);
        lv_obj_set_size(cell, fw / 2, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(cell, U(3), 0);
        fh1_ui_key_chip(cell, hints[i].key);
        lv_obj_t *l = lv_label_create(cell);
        lv_obj_add_style(l, &st_text, 0);
        char w[FH1_HINT_CHARS * 4 + 1];
        hint_word(hints[i].text, w, sizeof(w));
        lv_label_set_text(l, w);
    }
    lv_obj_update_layout(win->footer);
    const int fh = lv_obj_get_height(win->footer);
    lv_obj_set_pos(win->footer, U(3), win->ch - U(3) - fh);
    if (win->copa < 255) lv_obj_set_style_opa(win->footer, win->copa, 0);     // Command: still building
    lv_obj_set_height(win->body, win->ch - U(3) - fh - win->body_y);
}

// ── Sliders: a value from min to max as a bar with the value written in it. Focused, Left/Right move it
// a step; the app hears every step (to preview it) and, once the moving stops, a last call marked done (to
// save it, so flash is written once). ──

typedef struct {
    lv_obj_t *bar, *fill, *text;
    int value, min, max, step;
    const char *fmt;
    fh1_ui_slider_cb cb;
    void *user;
    lv_timer_t *settle;
} ui_slider_t;

static void slider_show(ui_slider_t *sl)
{
    const int span = sl->max - sl->min;
    lv_obj_set_width(sl->fill, LV_PCT(span ? (sl->value - sl->min) * 100 / span : 0));
    lv_label_set_text_fmt(sl->text, sl->fmt, sl->value);
}

static void slider_settled(lv_timer_t *t)
{
    ui_slider_t *sl = lv_timer_get_user_data(t);
    sl->settle = NULL;
    if (sl->cb) sl->cb(sl->value, true, sl->user);
}

static void slider_event(lv_event_t *e)
{
    ui_slider_t *sl = lv_event_get_user_data(e);
    if (sl->settle) {                      // deleted while moving: the last value still counts
        lv_timer_delete(sl->settle);
        if (sl->cb) sl->cb(sl->value, true, sl->user);
    }
    free(sl);
}

static bool slider_key(fh1_win_t *w, int key)
{
    if (key != FH1_KEY_LEFT && key != FH1_KEY_RIGHT) return false;
    lv_obj_t *f = lv_group_get_focused(w->group);
    if (!f || !lv_obj_has_state(f, LV_STATE_USER_3)) return false;
    ui_slider_t *sl = lv_obj_get_user_data(f);
    int v = sl->value + (key == FH1_KEY_RIGHT ? sl->step : -sl->step);
    v = v < sl->min ? sl->min : v > sl->max ? sl->max : v;
    if (v == sl->value) return true;
    sl->value = v;
    slider_show(sl);
    if (sl->cb) sl->cb(v, false, sl->user);
    if (sl->settle) lv_timer_reset(sl->settle);
    else {
        sl->settle = lv_timer_create(slider_settled, 700, sl);
        lv_timer_set_repeat_count(sl->settle, 1);
    }
    return true;
}

lv_obj_t *fh1_ui_slider(fh1_win_t *win, lv_obj_t *parent, const char *name, const char *fmt, int value, int min,
                        int max, int step, fh1_ui_slider_cb cb, void *user)
{
    ui_slider_t *sl = calloc(1, sizeof(*sl));
    if (!sl) return NULL;
    lv_obj_t *b = fh1_ui_button(win, parent, name, NULL, NULL);
    lv_obj_set_state_user_3(b, true);
    lv_obj_set_user_data(b, sl);
    lv_obj_add_event_cb(b, slider_event, LV_EVENT_DELETE, sl);
    // A row: the name in the meters' column, then the bar.
    lv_obj_set_ignore_layout(lv_obj_get_child(b, 0), true);       // the cursor keeps its place
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, U(3), 0);
    lv_obj_t *l = lv_obj_get_child(b, 1);
    lv_obj_set_width(l, U(60));
    sl->bar = plain(b);
    lv_obj_set_flex_grow(sl->bar, 1);
    lv_obj_set_height(sl->bar, U(11));
    lv_obj_set_style_radius(sl->bar, U(3), 0);
    lv_obj_set_style_border_width(sl->bar, s_u, 0);
    lv_obj_set_style_border_color(sl->bar, lv_color_hex(s_theme->ink), 0);
    lv_obj_set_style_bg_color(sl->bar, lv_color_hex(s_theme->paper), 0);
    lv_obj_set_style_bg_opa(sl->bar, LV_OPA_COVER, 0);
    lv_obj_set_style_clip_corner(sl->bar, true, 0);
    sl->fill = plain(sl->bar);
    lv_obj_set_height(sl->fill, LV_PCT(100));
    lv_obj_set_style_bg_opa(sl->fill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(sl->fill, lv_color_hex(0x7ab8f0), 0);
    sl->text = lv_label_create(sl->bar);
    lv_obj_add_style(sl->text, &st_text, 0);
    lv_obj_center(sl->text);
    sl->value = value < min ? min : value > max ? max : value;
    sl->min = min;
    sl->max = max;
    sl->step = step > 0 ? step : 1;
    sl->fmt = fmt ? fmt : "%d%%";
    sl->cb = cb;
    sl->user = user;
    slider_show(sl);
    return b;
}

// ── Views: a window with several pages side by side (one per device of a kind, say). A line on top names
// the page shown; Left/Right slide to the next or previous one. Only the page shown takes focus. ──

#define MAX_VIEWS 8
typedef struct ui_views {
    lv_obj_t *head, *head_label, *strip;
    lv_obj_t *page[MAX_VIEWS];
    char title[MAX_VIEWS][32];
    int n, cur, w;
} ui_views_t;

static void views_reset(fh1_win_t *win)
{
    if (!win->views) return;
    free(win->views);
    win->views = NULL;
    lv_obj_set_flex_flow(win->body, LV_FLEX_FLOW_COLUMN);    // back to a plain body
    lv_obj_set_scrollable(win->body, true);
}

static void views_label(fh1_win_t *win)
{
    ui_views_t *v = win->views;
    lv_label_set_text_fmt(v->head_label, "%s  %d/%d", v->title[v->cur], v->cur + 1, v->n);
}

// Only the page shown is in the window's focus group.
static void views_focus_add(fh1_win_t *win, lv_obj_t *o)
{
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
        lv_obj_t *c = lv_obj_get_child(o, i);
        if (lv_obj_has_state(c, LV_STATE_USER_1)) lv_group_add_obj(win->group, c);
        else views_focus_add(win, c);
    }
}

static void views_strip_anim(void *var, int32_t x) { lv_obj_set_style_translate_x(var, x, 0); }

void fh1_win_show_view(fh1_win_t *win, int i)
{
    ui_views_t *v = win->views;
    if (!v || v->n == 0) return;
    i = (i % v->n + v->n) % v->n;
    const int from = -v->cur * v->w, to = -i * v->w;
    v->cur = i;
    views_label(win);
    lv_group_remove_all_objs(win->group);
    views_focus_add(win, v->page[i]);
    if (lv_group_get_obj_count(win->group)) lv_group_focus_obj(lv_group_get_obj_by_index(win->group, 0));
    restyle();
    if (from == to) {
        lv_obj_set_style_translate_x(v->strip, to, 0);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, v->strip);
    lv_anim_set_exec_cb(&a, views_strip_anim);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, 220);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

int fh1_win_view(fh1_win_t *win) { return win->views ? win->views->cur : 0; }

static bool views_key(fh1_win_t *win, int key)
{
    ui_views_t *v = win->views;
    if (v->n < 2 || (key != FH1_KEY_LEFT && key != FH1_KEY_RIGHT)) return false;
    fh1_win_show_view(win, v->cur + (key == FH1_KEY_RIGHT ? 1 : -1));
    return true;
}

lv_obj_t *fh1_win_add_view(fh1_win_t *win, const char *title)
{
    lv_obj_t *body = win->body;
    if (!win->views) {
        win->views = calloc(1, sizeof(ui_views_t));
        if (!win->views) return body;
        ui_views_t *v = win->views;
        lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scrollable(body, false);
        lv_obj_update_layout(body);
        v->w = lv_obj_get_content_width(body);
        // The line on top: the left/right chip, then the page's name and number.
        v->head = plain(body);
        lv_obj_set_size(v->head, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(v->head, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(v->head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(v->head, U(3), 0);
        lv_obj_set_style_pad_bottom(v->head, U(1), 0);
        lv_obj_set_style_border_side(v->head, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(v->head, s_u, 0);
        lv_obj_set_style_border_color(v->head, lv_color_hex(s_theme->dim), 0);
        fh1_ui_key_chip(v->head, FH1_HINT_LEFTRIGHT);
        v->head_label = lv_label_create(v->head);
        lv_obj_add_style(v->head_label, &st_text, 0);
        // The pages, side by side in a strip as wide as all of them, that slides; the body clips it. (A strip
        // only one page wide would be skipped by LVGL once slid off its own area, pages and all.)
        v->strip = plain(body);
        lv_obj_set_width(v->strip, v->w);
        lv_obj_set_flex_grow(v->strip, 1);
    }
    ui_views_t *v = win->views;
    if (v->n == MAX_VIEWS) return v->page[v->n - 1];
    lv_obj_t *pg = plain(v->strip);
    lv_obj_set_pos(pg, v->n * v->w, 0);
    lv_obj_set_size(pg, v->w, LV_PCT(100));
    lv_obj_set_flex_flow(pg, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(pg, U(1), 0);
    lv_obj_set_style_pad_top(pg, U(1), 0);
    lv_obj_set_scrollable(pg, true);
    lv_obj_set_scroll_dir(pg, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(pg, LV_SCROLLBAR_MODE_OFF);
    snprintf(v->title[v->n], sizeof(v->title[0]), "%s", title);
    v->page[v->n++] = pg;
    lv_obj_set_width(v->strip, v->n * v->w);
    views_label(win);
    return pg;
}

// ── Paged lists: only the rows that fit exist; their texts come from the app when the page changes, so a
// list of thousands costs no more than a page. Up/Down move (and cross pages), Left/Right turn pages. ──

#define LIST_MAX_ROWS 24
typedef struct ui_list {
    fh1_win_t *win;
    int count, top, rows;
    fh1_ui_list_item_cb item;
    fh1_ui_list_pick_cb pick;
    void *user;
    lv_obj_t *row[LIST_MAX_ROWS], *icon[LIST_MAX_ROWS], *pager;
    struct { struct ui_list *l; int i; } ctx[LIST_MAX_ROWS];
} ui_list_t;

static void list_fill(ui_list_t *l)
{
    for (int i = 0; i < l->rows; i++) {
        int idx = l->top + i;
        bool on = idx < l->count;
        lv_obj_set_hidden(l->row[i], !on);
        if (l->icon[i]) {
            lv_obj_delete(l->icon[i]);
            l->icon[i] = NULL;
            lv_obj_set_style_pad_right(l->row[i], 0, 0);
        }
        if (!on) continue;
        char t[96] = "";
        int icon = -1;
        l->item(idx, t, sizeof(t), &icon, l->user);
        fh1_ui_button_set_text(l->row[i], t);
        if (icon >= 0) l->icon[i] = fh1_ui_button_icon(l->row[i], (fh1_ui_icon_t)icon);
    }
    const int pages = (l->count + l->rows - 1) / l->rows;
    lv_obj_set_hidden(l->pager, pages <= 1);
    if (pages > 1) lv_label_set_text_fmt(l->pager, T("Page %d/%d (%d)"), l->top / l->rows + 1, pages, l->count);
}

// Show item idx: turn to its page and focus its row.
static void list_go(ui_list_t *l, int idx)
{
    if (l->count <= 0) return;
    idx = (idx % l->count + l->count) % l->count;            // wraps both ways
    int top = idx / l->rows * l->rows;
    if (top != l->top) {
        l->top = top;
        list_fill(l);
    }
    lv_group_focus_obj(l->row[idx - top]);
    restyle();
}

static int list_focused(ui_list_t *l)
{
    lv_obj_t *f = lv_group_get_focused(l->win->group);
    for (int i = 0; i < l->rows; i++) {
        if (l->row[i] == f) return l->top + i;
    }
    return -1;
}

static bool list_key(ui_list_t *l, int key)
{
    int cur = list_focused(l);
    if (cur < 0 || l->count <= 0) return false;
    switch (key) {
    case FH1_KEY_UP: list_go(l, cur - 1); return true;
    case FH1_KEY_DOWN: list_go(l, cur + 1); return true;
    case FH1_KEY_LEFT: list_go(l, cur - l->rows < 0 && cur != 0 ? 0 : cur - l->rows); return true;
    case FH1_KEY_RIGHT: list_go(l, cur + l->rows >= l->count && cur != l->count - 1 ? l->count - 1 : cur + l->rows); return true;
    default: return false;
    }
}

static void list_row_cb(void *user)
{
    struct { struct ui_list *l; int i; } *c = user;
    ui_list_t *l = c->l;
    if (l->top + c->i < l->count) l->pick(l->top + c->i, l->user);
}

lv_obj_t *fh1_ui_list(fh1_win_t *win, int count, fh1_ui_list_item_cb item, fh1_ui_list_pick_cb pick, void *user)
{
    ui_list_t *l = calloc(1, sizeof(*l));
    if (!l) return NULL;
    l->win = win;
    l->count = count;
    l->item = item;
    l->pick = pick;
    l->user = user;
    win->list = l;
    lv_obj_t *body = win->body;
    lv_obj_update_layout(body);
    const int avail = lv_obj_get_content_height(body) - s_line - U(1);   // room for the pager line
    // As many rows as fit: make them one by one while the next still fits.
    for (int i = 0; i < LIST_MAX_ROWS; i++) {
        l->ctx[i].l = l;
        l->ctx[i].i = i;
        lv_obj_t *r = fh1_ui_button(win, body, "", list_row_cb, &l->ctx[i]);
        lv_obj_update_layout(body);
        if (i > 0 && lv_obj_get_y2(r) - lv_obj_get_y(l->row[0]) >= avail) {
            lv_group_remove_obj(r);
            lv_obj_delete(r);
            break;
        }
        l->row[i] = r;
        l->rows = i + 1;
    }
    l->pager = lv_label_create(body);
    lv_obj_add_style(l->pager, &st_text, 0);
    lv_obj_set_style_text_color(l->pager, lv_color_hex(s_theme->dim), 0);
    list_fill(l);
    if (count > 0) list_go(l, 0);
    return body;
}

void fh1_ui_list_refresh(fh1_win_t *win, int count)
{
    ui_list_t *l = win->list;
    if (!l) return;
    int cur = list_focused(l);
    l->count = count;
    l->top = -1;                       // forces a refill
    list_go(l, cur < 0 ? 0 : cur >= count ? count - 1 : cur);
    if (count <= 0) {
        l->top = 0;
        list_fill(l);
    }
}

int fh1_ui_list_index(fh1_win_t *win) { return win->list ? list_focused(win->list) : -1; }

void fh1_ui_list_go(fh1_win_t *win, int index)
{
    ui_list_t *l = win->list;
    if (l && index >= 0 && index < l->count) list_go(l, index);
}

// ── Meters: a bar with its value written inside, instead of a sentence of numbers ──

lv_obj_t *fh1_ui_meter(lv_obj_t *parent, const char *name)
{
    lv_obj_t *row = plain(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, U(3), 0);
    if (name) {
        lv_obj_t *l = lv_label_create(row);
        lv_obj_add_style(l, &st_text, 0);
        lv_label_set_text(l, name);
        lv_obj_set_width(l, U(60));        // names up to 7 letters and a gap: the bars of a window line up
    }
    lv_obj_t *bar = plain(row);
    lv_obj_set_flex_grow(bar, 1);
    lv_obj_set_height(bar, U(11));
    lv_obj_set_style_radius(bar, U(3), 0);
    lv_obj_set_style_border_width(bar, s_u, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(s_theme->ink), 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(s_theme->paper), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_clip_corner(bar, true, 0);
    lv_obj_t *fill = plain(bar);                   // child 0
    lv_obj_set_size(fill, 0, LV_PCT(100));
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    lv_obj_t *text = lv_label_create(bar);         // child 1
    lv_obj_add_style(text, &st_text, 0);
    lv_obj_center(text);
    return row;
}

void fh1_ui_meter_set(lv_obj_t *meter, int pct, const char *text, uint32_t colour)
{
    lv_obj_t *bar = lv_obj_get_child(meter, -1);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    lv_obj_t *fill = lv_obj_get_child(bar, 0);
    lv_obj_set_width(fill, LV_PCT(pct));
    lv_obj_set_style_bg_color(fill, lv_color_hex(colour), 0);
    lv_label_set_text(lv_obj_get_child(bar, 1), text);
}

uint32_t fh1_ui_level_colour(int pct_good)
{
    return pct_good >= 66 ? 0x7ad08f : pct_good >= 33 ? 0xf6d365 : 0xf08a80;
}

// ── The idle flourish: now and then a corner ball of the focused window shakes ──

static void shake_anim(void *var, int32_t v) { lv_image_set_rotation(var, v); }
static void shake_done(lv_anim_t *a) { lv_image_set_rotation(a->var, 0); }

// The looks' running decoration, a step every 125 ms on the window in focus only (the others are still):
// Hex's warning stripes run one unit a step, as a warning bar's do. Nothing runs while a program owns the screen
// or the window is still opening.
static void stripe_anim(void *var, int32_t v) {}      // (no animation object any more; kept for lv_anim_delete)

static void look_tick(lv_timer_t *t)
{
    fh1_win_t *w = top();
    if (!w || s_paused || w->ghost || w->building) return;
    if (HEX) {
        w->phase = (w->phase + 7) & 7;               // the stripes move right: '/' bands drift with the reading
        title_frame(w);
    } else if (FRAMED) {
        // Garden: the flowers sway (a step every 500 ms). Command: the corner lights blink, the tab's lights cycle.
        w->phase = (w->phase + 1) % 240;
        int mask = 0;
        if (s_theme->look == LOOK_GARDEN) {
            if (w->phase % 4 == 0) mask |= FR_CORNERS;
        } else {
            if (w->phase % 6 == 0) mask |= FR_CORNERS;
            if (w->phase % 3 == 0) mask |= FR_TITLE;
        }
        if (mask) frame_render(w, mask);
    }
}

// The mote of the window in focus moves 20 times a second (only it: a small picture, a small redraw).
static void mote_tick(lv_timer_t *t)
{
    fh1_win_t *w = top();
    if (!w || !w->mote || s_paused || w->building) return;
    int x, y, f;
    const bool seen = look_mote_at(s_theme->look, s_u, w->cw + U(4), w->ch + U(4), tick_ms(), &x, &y, &f);
    if (lv_obj_is_hidden(w->mote) == seen) lv_obj_set_hidden(w->mote, !seen);
    if (!seen) return;
    if (lv_image_get_src(w->mote) != &s_mote[f]) lv_image_set_src(w->mote, &s_mote[f]);
    move_to(w->mote, x - U(2), y - U(2));
}

static void on_flourish(fh1_event_id_t id, const void *data, size_t len, void *user)
{
    fh1_win_t *w = top();
    if (!w || s_paused || w->ghost || w->building) return;
    if (HEX || FRAMED) return;            // their decorations run all the time (look_tick, mote_tick)
    lv_obj_t *b = w->balls[esp_random() % 4];
    if (lv_anim_get(b, shake_anim)) return;
    // Three wobbles of 200 ms: -15 to +15 degrees and back, then rest at 0.
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, b);
    lv_anim_set_exec_cb(&a, shake_anim);
    lv_anim_set_values(&a, -150, 150);
    lv_anim_set_duration(&a, 100);
    lv_anim_set_reverse_duration(&a, 100);
    lv_anim_set_repeat_count(&a, 3);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_completed_cb(&a, shake_done);
    lv_anim_start(&a);
}

// ── The transfer overlay: a file or a firmware arriving (FH1_EV_TRANSFER) shows in a box in the middle of
// the screen, over everything: an animated icon, what it is, a progress bar. It opens like a window, from
// its middle outwards, and closes the same way in reverse a little after the end. ──

// What a file is, from its extension: the icon and words its transfer is shown with.
#define MAX_FILE_TYPES 16
static struct {
    char ext[8];
    fh1_ui_icon_t icon;
    const char *receiving, *received;
} s_ftypes[MAX_FILE_TYPES];
static int s_nftypes;

void fh1_ui_register_file_type(const char *ext, fh1_ui_icon_t icon, const char *receiving, const char *received)
{
    if (*ext == '.') ext++;
    for (int i = 0; i < s_nftypes; i++) {
        if (strcasecmp(s_ftypes[i].ext, ext) == 0) {       // the last registration wins (an app over a default)
            s_ftypes[i].icon = icon;
            s_ftypes[i].receiving = receiving;
            s_ftypes[i].received = received;
            return;
        }
    }
    if (s_nftypes >= MAX_FILE_TYPES) return;
    strlcpy(s_ftypes[s_nftypes].ext, ext, sizeof(s_ftypes[0].ext));
    s_ftypes[s_nftypes].icon = icon;
    s_ftypes[s_nftypes].receiving = receiving;
    s_ftypes[s_nftypes].received = received;
    s_nftypes++;
}

static int file_type(const char *name)
{
    const char *dot = strrchr(name, '.');
    for (int i = 0; dot && i < s_nftypes; i++) {
        if (strcasecmp(s_ftypes[i].ext, dot + 1) == 0) return i;
    }
    return -1;
}

static struct {
    lv_obj_t *ghost, *box, *row, *icon, *title, *name, *meter;
    lv_timer_t *timer;
    int icon_id;
    uint32_t last_ms;               // the last report
    uint32_t close_at;              // 0: stays open
    bool closing;
} s_ov = { .icon_id = -1 };

static void ov_ghost_anim(void *var, int32_t v)
{
    const int w = lv_obj_get_width(s_ov.box), h = lv_obj_get_height(s_ov.box);
    lv_obj_set_size(var, U(12) + (w - U(12)) * v / 256, U(12) + (h - U(12)) * v / 256);
}

static void ov_opened(lv_anim_t *a)
{
    lv_obj_set_hidden(s_ov.ghost, true);
    lv_obj_set_hidden(s_ov.box, false);
}

static void ov_destroy(void *arg)
{
    if (!s_paused) fh1_app_request_yield(false);     // no program waiting for it any more
    if (s_ov.timer) lv_timer_delete(s_ov.timer);
    if (s_ov.ghost) lv_obj_delete(s_ov.ghost);
    if (s_ov.box) lv_obj_delete(s_ov.box);
    memset(&s_ov, 0, sizeof(s_ov));
    s_ov.icon_id = -1;
}

static void ov_closed(lv_anim_t *a) { lv_async_call(ov_destroy, NULL); }

static void ov_animate(int32_t from, int32_t to, lv_anim_completed_cb_t done)
{
    lv_anim_delete(s_ov.ghost, ov_ghost_anim);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_ov.ghost);
    lv_anim_set_exec_cb(&a, ov_ghost_anim);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, 250);
    lv_anim_set_path_cb(&a, to > from ? lv_anim_path_ease_out : lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&a, done);
    ov_ghost_anim(s_ov.ghost, from);
    lv_anim_start(&a);
}

static void ov_close(void)
{
    if (!s_ov.box || s_ov.closing) return;
    s_ov.closing = true;
    // Still opening: shrink from where the outline got to.
    int from = 256;
    if (lv_anim_get(s_ov.ghost, ov_ghost_anim)) {
        const int w = lv_obj_get_width(s_ov.box);
        from = (lv_obj_get_width(s_ov.ghost) - U(12)) * 256 / (w - U(12));
    }
    lv_obj_set_hidden(s_ov.box, true);
    lv_obj_set_hidden(s_ov.ghost, false);
    ov_animate(from, 0, ov_closed);
}

static void ov_tick(lv_timer_t *t)
{
    uint32_t now = tick_ms();
    // Closes a moment after the end, or when the reports stopped coming (the sender went away).
    if ((s_ov.close_at && (int32_t)(now - s_ov.close_at) >= 0) || now - s_ov.last_ms > 15000) ov_close();
}

static void ov_open(void)
{
    const int W = U(176);
    s_ov.box = plain(lv_layer_top());
    lv_obj_add_style(s_ov.box, &st_box, 0);
    lv_obj_set_width(s_ov.box, W);
    lv_obj_set_height(s_ov.box, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(s_ov.box, U(6), 0);
    lv_obj_set_style_pad_row(s_ov.box, U(4), 0);
    lv_obj_set_flex_flow(s_ov.box, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *top_row = s_ov.row = plain(s_ov.box);
    lv_obj_set_size(top_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top_row, U(6), 0);
    lv_obj_t *col = plain(top_row);                // child 0; the icon goes before it
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, U(2), 0);
    s_ov.title = lv_label_create(col);
    lv_obj_add_style(s_ov.title, &st_text, 0);
    s_ov.name = line_label(col);
    lv_obj_set_width(s_ov.name, LV_PCT(100));
    lv_obj_set_style_text_color(s_ov.name, lv_color_hex(s_theme->dim), 0);
    s_ov.meter = fh1_ui_meter(s_ov.box, NULL);
    lv_obj_center(s_ov.box);
    lv_obj_set_hidden(s_ov.box, true);
    s_ov.ghost = plain(lv_layer_top());
    lv_obj_add_style(s_ov.ghost, &st_box, 0);
    lv_obj_center(s_ov.ghost);
    s_ov.timer = lv_timer_create(ov_tick, 200, NULL);
}

// Dressed like a window, once its size is known: the inner line and a ball on each corner.
static void ov_dress(void)
{
    const int w = lv_obj_get_width(s_ov.box), h = lv_obj_get_height(s_ov.box);
    const int edge = -U(8);                         // the box's edge, in its content's coordinates
    lv_obj_t *inset = plain(s_ov.box);
    lv_obj_add_style(inset, &st_inset, 0);
    lv_obj_set_ignore_layout(inset, true);
    lv_obj_set_pos(inset, edge + U(3), edge + U(3));
    lv_obj_set_size(inset, w - U(6), h - U(6));
    lv_obj_move_to_index(inset, 0);
    lv_obj_set_overflow_visible(s_ov.box, true);
    lv_obj_add_event_cb(s_ov.box, box_ext_draw, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
    lv_obj_refresh_ext_draw_size(s_ov.box);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_image_create(s_ov.box);
        lv_obj_set_ignore_layout(b, true);
        corner_place(b, i, edge, edge, w, h, 1);
    }
}

static void on_transfer(fh1_event_id_t id, const void *data, size_t len, void *user)
{
    if (len != sizeof(fh1_transfer_t)) return;
    const fh1_transfer_t *t = data;
    // A program owns the screen: ask it to lend it (fh1_ui_lend_screen) so the overlay shows over its picture.
    if (s_paused) fh1_app_request_yield(true);
    if (s_ov.closing) {               // a new transfer while the last one's box goes: start afresh
        lv_anim_delete(s_ov.ghost, ov_ghost_anim);
        ov_destroy(NULL);
    }
    bool fresh = !s_ov.box;
    if (fresh) ov_open();
    // Firmware being installed has its own look; an uploaded file is shown as what its extension says.
    const bool fw = t->kind == FH1_XFER_FIRMWARE;
    const int ft = fw ? -1 : file_type(t->name);
    const fh1_ui_icon_t icon = fw ? FH1_ICON_FIRMWARE : ft >= 0 ? s_ftypes[ft].icon : FH1_ICON_DOWNLOAD;
    if (s_ov.icon_id != (int)icon) {
        s_ov.icon_id = icon;
        if (s_ov.icon) lv_obj_delete(s_ov.icon);
        s_ov.icon = fh1_ui_icon(s_ov.row, icon, true, true);
        lv_obj_move_to_index(s_ov.icon, 0);
    }
    lv_label_set_text(s_ov.title, t->state == FH1_XFER_FAILED ? (fw ? T("Update failed") : T("Transfer failed"))
                                  : t->state == FH1_XFER_DONE ? (fw ? T("Firmware written") : ft >= 0 ? T(s_ftypes[ft].received) : T("File received"))
                                  : fw ? T("Updating firmware") : ft >= 0 ? T(s_ftypes[ft].receiving) : T("Receiving"));
    const char *name = t->name[0] ? t->name : T("Over the network");
    if (strcmp(lv_label_get_text(s_ov.name), name) != 0) lv_label_set_text(s_ov.name, name);
    char pct[24];
    uint32_t colour = 0x3a78e0;
    if (t->state == FH1_XFER_FAILED) {
        snprintf(pct, sizeof(pct), "%s", T("Failed"));
        colour = 0xf08a80;
    } else if (t->state == FH1_XFER_DONE) {
        snprintf(pct, sizeof(pct), "%s", fw ? T("Restarting...") : "100%");
        colour = 0x7ad08f;
    } else {
        snprintf(pct, sizeof(pct), "%d%%", t->pct);
    }
    fh1_ui_meter_set(s_ov.meter, t->state == FH1_XFER_FAILED ? 100 : t->pct, pct, colour);
    s_ov.last_ms = tick_ms();
    s_ov.close_at = t->state == FH1_XFER_RUNNING ? 0 : s_ov.last_ms + (t->state == FH1_XFER_DONE ? 1500 : 3000);
    if (fresh) {
        lv_obj_update_layout(s_ov.box);
        ov_dress();
        ov_animate(0, 256, ov_opened);
    }
}

// ── Dialogs ──

typedef struct {
    void (*cb)(bool yes, void *user);
    void *user;
    fh1_win_t *win;
} confirm_t;

static void msg_ok(void *user) { fh1_win_close(user); }

static void centre(fh1_win_t *w) { lv_obj_center(w->box); }

void fh1_ui_message(const char *title, const char *text)
{
    fh1_win_t *w = fh1_win_create(title, 220, 96);
    if (!w) return;
    w->alert = true;
    fh1_ui_label(w->body, text);
    fh1_ui_button(w, NULL, T("OK"), msg_ok, w);
    centre(w);
    ui_sound(s_theme->voice, UI_SND_ALERT);
}

static void confirm_yes(void *user)
{
    confirm_t *c = user;
    void (*cb)(bool, void *) = c->cb;
    c->cb = NULL;
    if (cb) cb(true, c->user);
    fh1_win_close(c->win);
}

static void confirm_no(void *user)
{
    confirm_t *c = user;
    void (*cb)(bool, void *) = c->cb;
    c->cb = NULL;
    if (cb) cb(false, c->user);
    fh1_win_close(c->win);
}

static void confirm_closed(fh1_win_t *w, void *user)
{
    confirm_t *c = user;
    if (c->cb) c->cb(false, c->user);     // closed with B: counts as "no"
    free(c);
}

void fh1_ui_confirm(const char *title, const char *text, void (*cb)(bool yes, void *user), void *user)
{
    fh1_win_t *w = fh1_win_create(title, 230, 104);
    if (!w) {
        if (cb) cb(false, user);
        return;
    }
    w->alert = true;
    ui_sound(s_theme->voice, UI_SND_ALERT);
    confirm_t *c = calloc(1, sizeof(*c));
    c->cb = cb;
    c->user = user;
    c->win = w;
    fh1_ui_label(w->body, text);
    fh1_ui_button(w, NULL, T("NO"), confirm_no, c);     // first, so the safe answer has the cursor
    fh1_ui_button(w, NULL, T("YES"), confirm_yes, c);
    fh1_win_on_close(w, confirm_closed, c);
    centre(w);
}

// ── On-screen keyboard: a 10 x 4 character grid and a row of SET / SPACE / DEL / OK ──
// D-pad moves, A types, B deletes (or cancels when empty), SELECT changes the set, START is OK.

#define KB_COLS 10
#define KB_ROWS 5                       // 4 character rows + the special row
static const char *const s_kb_sets[3][4] = {
    { "1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm_.@" },
    { "1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM_.@" },
    { "!#$%&*+=?/", "()[]{}<>:;", "'\"`~^|\\,. ", "@-_ 0123  " },
};
static const char *const s_kb_set_names[3] = { "ABC", "#+=", "abc" };   // what the SET key switches to

typedef struct {
    fh1_win_t *win;
    lv_obj_t *text, *keys[KB_ROWS][KB_COLS], *special[4];
    int r, c, set;
    char buf[65];
    size_t max;
    bool done, secret;                // secret: typed characters show as *
    void (*cb)(const char *text, void *user);
    void *user;
} kb_t;

static int kb_special_index(int c) { return c < 2 ? 0 : c < 6 ? 1 : c < 8 ? 2 : 3; }

static void kb_draw(kb_t *k)
{
    if (k->secret) {
        char stars[sizeof(k->buf) + 1];
        size_t n = strlen(k->buf);
        memset(stars, '*', n);
        stars[n] = '_';
        stars[n + 1] = 0;
        lv_label_set_text(k->text, stars);
    } else {
        lv_label_set_text_fmt(k->text, "%s_", k->buf);
    }
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < KB_COLS; c++) {
            char t[2] = { s_kb_sets[k->set][r][c], 0 };
            lv_label_set_text(k->keys[r][c], t);
            bool on = (r == k->r && c == k->c);
            if (on) lv_obj_add_style(k->keys[r][c], &st_tab_on, 0);
            else lv_obj_remove_style(k->keys[r][c], &st_tab_on, 0);
        }
    }
    lv_label_set_text(k->special[0], s_kb_set_names[k->set]);
    for (int i = 0; i < 4; i++) {
        bool on = (k->r == 4 && kb_special_index(k->c) == i);
        if (on) lv_obj_add_style(k->special[i], &st_tab_on, 0);
        else lv_obj_remove_style(k->special[i], &st_tab_on, 0);
    }
}

static void kb_finish(kb_t *k, bool ok)
{
    k->done = true;
    if (k->cb) k->cb(ok ? k->buf : NULL, k->user);
    k->cb = NULL;
    fh1_win_close(k->win);
}

static void kb_type(kb_t *k, char ch)
{
    size_t n = strlen(k->buf);
    if (n < k->max && ch) {
        k->buf[n] = ch;
        k->buf[n + 1] = 0;
    }
}

static bool kb_key(fh1_win_t *w, int key, void *user)
{
    kb_t *k = user;
    size_t n = strlen(k->buf);
    switch (key) {
    case FH1_KEY_UP: k->r = (k->r + KB_ROWS - 1) % KB_ROWS; break;
    case FH1_KEY_DOWN: k->r = (k->r + 1) % KB_ROWS; break;
    case FH1_KEY_LEFT: k->c = (k->c + KB_COLS - 1) % KB_COLS; break;
    case FH1_KEY_RIGHT: k->c = (k->c + 1) % KB_COLS; break;
    case FH1_KEY_SELECT: k->set = (k->set + 1) % 3; break;
    case FH1_KEY_START: kb_finish(k, true); return true;
    case FH1_KEY_B:
        if (n == 0) {
            kb_finish(k, false);
            return true;
        }
        k->buf[n - 1] = 0;
        break;
    case FH1_KEY_A:
        if (k->r < 4) {
            kb_type(k, s_kb_sets[k->set][k->r][k->c]);
        } else {
            switch (kb_special_index(k->c)) {
            case 0: k->set = (k->set + 1) % 3; break;
            case 1: kb_type(k, ' '); break;
            case 2: if (n) k->buf[n - 1] = 0; break;
            case 3: kb_finish(k, true); return true;
            }
        }
        break;
    case FH1_KEY_MENU:
        return false;
    default:
        return true;
    }
    kb_draw(k);
    return true;
}

static void kb_closed(fh1_win_t *w, void *user)
{
    kb_t *k = user;
    if (!k->done && k->cb) k->cb(NULL, k->user);     // closed some other way: cancelled
    memset(k->buf, 0, sizeof(k->buf));                 // it may have held a PIN or a passphrase
    free(k);
}

static lv_obj_t *kb_cell(lv_obj_t *row, int w)
{
    lv_obj_t *l = lv_label_create(row);
    lv_obj_set_width(l, w);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_radius(l, U(2), 0);
    return l;
}

static void kb_open(const char *title, const char *initial, size_t max_len, bool secret, void (*done)(const char *text, void *user), void *user);

void fh1_ui_keyboard(const char *title, const char *initial, size_t max_len, void (*done)(const char *text, void *user), void *user)
{
    kb_open(title, initial, max_len, false, done, user);
}

void fh1_ui_keyboard_secret(const char *title, size_t max_len, void (*done)(const char *text, void *user), void *user)
{
    kb_open(title, "", max_len, true, done, user);
}

static void kb_open(const char *title, const char *initial, size_t max_len, bool secret, void (*done)(const char *text, void *user), void *user)
{
    fh1_win_t *w = fh1_win_create(title, 228, 150);
    if (!w) {
        if (done) done(NULL, user);
        return;
    }
    kb_t *k = calloc(1, sizeof(*k));
    k->win = w;
    k->max = max_len < sizeof(k->buf) - 1 ? max_len : sizeof(k->buf) - 1;
    k->cb = done;
    k->user = user;
    k->secret = secret;
    k->r = 1;
    if (initial) strlcpy(k->buf, initial, k->max + 1);
    k->text = fh1_ui_label(w->body, "");
    const int cell = U(18);
    for (int r = 0; r < KB_ROWS; r++) {
        lv_obj_t *row = plain(w->body);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_add_style(row, &st_text, 0);
        if (r < 4) {
            for (int c = 0; c < KB_COLS; c++) k->keys[r][c] = kb_cell(row, cell);
        } else {
            const char *const names[4] = { "", T("SPACE"), T("DEL"), T("OK") };
            static const int span[4] = { 2, 4, 2, 2 };
            for (int i = 0; i < 4; i++) {
                k->special[i] = kb_cell(row, cell * span[i]);
                lv_label_set_text(k->special[i], names[i]);
            }
        }
    }
    const fh1_hint_t hints[] = { { FH1_KEY_A, T("Type") }, { FH1_KEY_B, T("Delete") }, { FH1_KEY_SELECT, T("Letters") },
                                 { FH1_KEY_START, T("Done") } };
    fh1_win_set_hints(w, hints, 4);
    fh1_win_on_key(w, kb_key, k);
    fh1_win_on_close(w, kb_closed, k);
    lv_obj_center(w->box);
    kb_draw(k);
}

// ── Theme ──

#define PAT_MAX_W 48
#define PAT_MAX_H 28
static uint16_t s_pattern[PAT_MAX_W * PAT_MAX_H];
static lv_image_dsc_t s_pattern_dsc;

// Hex: a honeycomb of flat-topped cells, 16 x 14 units with sides of 8 (as near to regular as whole pixels go),
// drawn with 1-unit lines on a 24 x 14 tile: one cell, and the next column's offset by half a cell.
static void hex_line(bool *on, int W, int H, int x0, int y0, int x1, int y1)
{
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        on[((y0 % H + H) % H) * W + (x0 % W + W) % W] = true;
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static uint16_t *s_pattern_big;        // framed looks' tiles: larger, in PSRAM

static void make_pattern(void)
{
    int W = 8, H = 8;
    lv_image_cache_drop(&s_pattern_dsc);
    s_pattern_dsc.data = (const uint8_t *)s_pattern;
    if (FRAMED) {
        if (!s_pattern_big) s_pattern_big = heap_caps_malloc(LOOK_DESK_MAX * LOOK_DESK_MAX * 2, MALLOC_CAP_SPIRAM);
        if (s_pattern_big) {
            look_pal_t p;
            look_palette(&p);
            look_desk(s_theme->look, &p, s_pattern_big, &W, &H);
            s_pattern_dsc.data = (const uint8_t *)s_pattern_big;
        } else {
            for (int i = 0; i < 64; i++) s_pattern[i] = lv_color_to_u16(lv_color_hex(s_theme->desk_a));
        }
    } else if (HEX) {
        enum { S = 8, DX = 4, DY = 7 };
        W = 2 * (DX + S);
        H = 2 * DY;
        bool on[2 * (DX + S) * 2 * DY] = { false };
        static const int v[6][2] = { { DX, 0 }, { DX + S, 0 }, { 2 * DX + S, DY }, { DX + S, 2 * DY }, { DX, 2 * DY }, { 0, DY } };
        for (int cell = 0; cell < 2; cell++) {
            const int ox = cell ? DX + S : 0, oy = cell ? DY : 0;
            for (int i = 0; i < 6; i++)
                hex_line(on, W, H, v[i][0] + ox, v[i][1] + oy, v[(i + 1) % 6][0] + ox, v[(i + 1) % 6][1] + oy);
        }
        const uint16_t a = lv_color_to_u16(lv_color_hex(s_theme->desk_a)), b = lv_color_to_u16(lv_color_hex(s_theme->desk_b));
        for (int y = 0; y < H * s_u; y++)
            for (int x = 0; x < W * s_u; x++) s_pattern[y * W * s_u + x] = on[(y / s_u) * W + x / s_u] ? b : a;
        W *= s_u;
        H *= s_u;
    } else {
        // Gen 1: a dot every 4 pixels; Gen 2: a small diamond lattice.
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                bool dot = s_theme->lattice ? (((x + y) % 8 == 0) || ((x - y + 8) % 8 == 0)) : (x % 4 == 0 && y % 4 == 0);
                s_pattern[y * 8 + x] = lv_color_to_u16(lv_color_hex(dot ? s_theme->desk_b : s_theme->desk_a));
            }
        }
    }
    s_pattern_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_pattern_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_pattern_dsc.header.w = W;
    s_pattern_dsc.header.h = H;
    s_pattern_dsc.header.stride = W * 2;
    s_pattern_dsc.data_size = W * H * 2;
}

// After a theme change, the colours the widgets were given of their own (a footer's line, a pager, a slider, a
// meter...) follow: the old theme's paper, ink and dim become the new one's.
static void recolour_tree(lv_obj_t *o, const theme_t *from)
{
    static const lv_style_prop_t props[] = { LV_STYLE_TEXT_COLOR, LV_STYLE_BORDER_COLOR, LV_STYLE_BG_COLOR };
    const uint32_t old[3] = { from->paper, from->ink, from->dim }, now[3] = { s_theme->paper, s_theme->ink, s_theme->dim };
    for (size_t k = 0; k < sizeof(props) / sizeof(props[0]); k++) {
        lv_style_value_t v;
        if (lv_obj_get_local_style_prop(o, props[k], &v, 0) != LV_STYLE_RES_FOUND) continue;
        for (int c = 0; c < 3; c++) {
            if (lv_color_eq(v.color, lv_color_hex(old[c]))) {
                v.color = lv_color_hex(now[c]);
                lv_obj_set_local_style_prop(o, props[k], v, 0);
                break;
            }
        }
    }
    const uint32_t n = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < n; i++) recolour_tree(lv_obj_get_child(o, i), from);
}

static void apply_theme(void)
{
    static const theme_t *applied;
    if (applied && applied != s_theme) recolour_tree(lv_screen_active(), applied);
    applied = s_theme;
    lv_color_t paper = lv_color_hex(s_theme->paper), ink = lv_color_hex(s_theme->ink);
    lv_style_set_bg_color(&st_box, paper);
    lv_style_set_border_color(&st_box, ink);
    lv_style_set_radius(&st_box, HEX || FRAMED ? 0 : U(4));
    lv_style_set_border_opa(&st_box, FRAMED ? LV_OPA_TRANSP : LV_OPA_COVER);     // framed: the frame is drawn over it
    lv_style_set_border_color(&st_inset, lv_color_hex(inset_rgb()));
    lv_style_set_radius(&st_inset, HEX ? 0 : U(6));
    lv_style_set_radius(&st_tab_on, HEX ? 0 : U(2));
    lv_style_set_pad_left(&st_item, item_pad());
    lv_style_set_text_color(&st_title, ink);
    lv_style_set_text_color(&st_text, lv_color_hex(text_rgb()));
    lv_style_set_text_color(&st_item, lv_color_hex(text_rgb()));
    lv_style_set_text_color(&st_dim, lv_color_hex(s_theme->dim));
    lv_style_set_image_recolor(&st_cursor, ink);
    lv_style_set_image_recolor_opa(&st_cursor, FRAMED ? LV_OPA_TRANSP : LV_OPA_COVER);   // framed: cursors in colour
    lv_style_set_bg_color(&st_bar, paper);
    lv_style_set_border_color(&st_bar, ink);
    lv_style_set_text_color(&st_bar, ink);
    lv_style_set_bg_color(&st_tab_on, ink);
    lv_style_set_text_color(&st_tab_on, paper);
    lv_style_set_bg_color(&st_icon, ink);
    lv_style_set_border_color(&st_icon, ink);
    lv_style_set_image_recolor(&st_icon, ink);
    lv_style_set_bg_color(&st_icon_off, lv_color_hex(s_theme->dim));
    lv_style_set_border_color(&st_icon_off, lv_color_hex(s_theme->dim));
    lv_style_set_image_recolor(&st_icon_off, lv_color_hex(s_theme->dim));
    make_sprites();
    make_pattern();
    make_cursors();
    make_chamfers();
    make_motes();
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(s_theme->desk_a), 0);
    lv_obj_set_style_bg_image_src(scr, NULL, 0);
    lv_obj_set_style_bg_image_src(scr, &s_pattern_dsc, 0);
    lv_obj_report_style_change(NULL);
    for (int i = 0; i < s_nwins; i++) win_relayout(s_wins[i]);
    // The cursors may have changed size: give each item's cursor its picture again.
    for (int i = 0; i < s_nwins; i++) {
        uint32_t n = lv_group_get_obj_count(s_wins[i]->group);
        for (uint32_t k = 0; k < n; k++) {
            lv_obj_t *cur = lv_obj_get_child(lv_group_get_obj_by_index(s_wins[i]->group, k), 0);
            if (cur && lv_obj_check_type(cur, &lv_image_class)) {
                lv_image_set_src(cur, NULL);
                lv_image_set_src(cur, &s_cur[0]);
                lv_obj_align(cur, LV_ALIGN_LEFT_MID, cursor_ofs(), 0);
            }
        }
    }
    restyle();
    lv_obj_invalidate(scr);
}

int fh1_ui_themes(const char *const **ids, const char *const **names)
{
    static const char *id_list[NTHEMES], *name_list[NTHEMES];
    for (size_t i = 0; i < NTHEMES; i++) {
        id_list[i] = s_themes[i].id;
        name_list[i] = T(s_themes[i].name);
    }
    if (ids) *ids = id_list;
    if (names) *names = name_list;
    return NTHEMES;
}

const char *fh1_ui_theme(void) { return s_theme->id; }

void fh1_ui_set_theme(const char *id)
{
    for (size_t i = 0; i < NTHEMES; i++) {
        if (strcmp(s_themes[i].id, id) == 0) s_theme = &s_themes[i];
    }
    fh1_settings_set_str("ui", "theme", s_theme->id);
    if (s_calls) apply_theme();
}

static void init_styles(void)
{
    lv_style_init(&st_box);
    lv_style_set_bg_opa(&st_box, LV_OPA_COVER);
    lv_style_set_border_width(&st_box, U(2));
    lv_style_set_radius(&st_box, U(4));
    lv_style_set_text_font(&st_box, s_font);

    lv_style_init(&st_inset);
    lv_style_set_border_width(&st_inset, s_u);
    lv_style_set_radius(&st_inset, U(6));        // the inner line bends diagonally into the corner ball


    lv_style_init(&st_title);
    lv_style_set_text_font(&st_title, s_font);

    lv_style_init(&st_text);
    lv_style_set_text_font(&st_text, s_font);

    lv_style_init(&st_item);
    lv_style_set_text_font(&st_item, s_font);
    lv_style_set_pad_left(&st_item, U(9));
    lv_style_set_pad_ver(&st_item, s_u);

    lv_style_init(&st_dim);
    lv_style_init(&st_cursor);
    lv_style_set_image_recolor_opa(&st_cursor, LV_OPA_COVER);

    lv_style_init(&st_bar);
    lv_style_set_bg_opa(&st_bar, LV_OPA_COVER);
    lv_style_set_border_width(&st_bar, U(2));
    lv_style_set_text_font(&st_bar, s_font);

    lv_style_init(&st_tab_on);
    lv_style_set_bg_opa(&st_tab_on, LV_OPA_COVER);
    lv_style_set_radius(&st_tab_on, U(2));

    lv_style_init(&st_icon);
    lv_style_set_bg_opa(&st_icon, LV_OPA_COVER);
    lv_style_set_image_recolor_opa(&st_icon, LV_OPA_COVER);
    lv_style_init(&st_icon_off);
    lv_style_set_bg_opa(&st_icon_off, LV_OPA_COVER);
    lv_style_set_image_recolor_opa(&st_icon_off, LV_OPA_COVER);
}

// ── Desktop ──

static void icon_on(lv_obj_t *o, bool on)
{
    lv_obj_remove_style(o, on ? &st_icon_off : &st_icon, 0);
    lv_obj_add_style(o, on ? &st_icon : &st_icon_off, 0);
}

// Signal and charge read at a glance: green, yellow, red.
static uint32_t level_colour(int level, int max)
{
    return level * 3 >= max * 2 ? 0x2fb36a : level * 3 >= max ? 0xf4c21f : 0xe5302c;
}


// ── The tray: gamepads and the sound, drawn at run time (their colour and level change) ──
// Each icon: a 11 x 7 shape ('x' takes the item's colour, other letters the sprite palette), a blank row,
// and a level gauge two rows high (a gamepad's battery: its level in the level's colour over a dark track),
// in units. The same gamepad icon, without the gauge, stands before a gamepad's name elsewhere
// (fh1_ui_pad_icon).

#define TRAY_W 11
#define TRAY_H 10
#define PAD_H 7
#define TRAY_ITEMS 5                  // 4 gamepads and the sound

static const char *const s_pad_shapes[FH1_PAD_KINDS][7] = {
    [FH1_PAD_FLAT] = { "...........", ".KKKKKKKKK.", "KxxxxxxxxxK", "KxKxxxxxKxK", "KxxxxxxxxxK", ".KKKKKKKKK.", "..........." },
    [FH1_PAD_PLAYSTATION] = { "..KKKKKKK..", ".KxxxxxxxK.", "KxKxxxxxKxK", "KxxxxxxxxxK", "KxxxKKKxxxK", "KxxK...KxxK", ".KK.....KK." },
    [FH1_PAD_PRO] = { "...........", ".KKKKKKKKK.", "KxKxxxxxKxK", "KxxxxxxxxxK", "KxxxxKxxxxK", ".KxxK.KxxK.", "..KK...KK.." },
    [FH1_PAD_JOYCON_L] = { "....KKK....", "...KxxK....", "..KxKxK....", "..KxxxK....", "..KxKxK....", "...KxxK....", "....KKK...." },
    [FH1_PAD_JOYCON_R] = { "....KKK....", "....KxxK...", "....KxKxK..", "....KxxxK..", "....KxKxK..", "....KxxK...", "....KKK...." },
    [FH1_PAD_REMOTE] = { "....KKK....", "...KxxxK...", "...KxKxK...", "...KxxxK...", "...KxKxK...", "...KxxxK...", "....KKK...." },
    [FH1_PAD_KEYS] = { "...........", "KKKKKKKKKKK", "KxxxxxxxxxK", "KxKxKxKxKxK", "KxxxxxxxxxK", "KKKKKKKKKKK", "..........." },
};

typedef struct {
    lv_obj_t *img;
    lv_image_dsc_t dsc;
    uint32_t *px;
    uint32_t key;                     // what is drawn: redrawn only when it changes
    uint8_t h;                        // rows: TRAY_H, or PAD_H for an icon without its gauge
} tray_t;
static tray_t s_tray[TRAY_ITEMS];

static void tray_px(uint32_t *px, int W, int x, int y, uint32_t c)
{
    const int z = s_u;
    for (int j = 0; j < z; j++) {
        for (int i = 0; i < z; i++) px[(y * z + j) * W + x * z + i] = c;
    }
}

static bool tray_begin(tray_t *t, uint32_t key)
{
    if (t->key == key && t->px) return false;
    if (!t->h) t->h = TRAY_H;
    const int W = TRAY_W * s_u, H = t->h * s_u;
    if (!t->px) t->px = heap_caps_malloc(W * H * 4, MALLOC_CAP_SPIRAM);
    if (!t->px) return false;
    t->key = key;
    memset(t->px, 0, W * H * 4);
    return true;
}

static void tray_end(tray_t *t)
{
    const int W = TRAY_W * s_u, H = t->h * s_u;
    t->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    t->dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    t->dsc.header.w = W;
    t->dsc.header.h = H;
    t->dsc.header.stride = W * 4;
    t->dsc.data_size = W * H * 4;
    t->dsc.data = (const uint8_t *)t->px;
    lv_image_cache_drop(&t->dsc);
    lv_image_set_src(t->img, &t->dsc);
    lv_obj_invalidate(t->img);
}

// A gamepad: its shape in its colour, its battery as a line in the level's colour (none when unknown).
static void tray_pad(tray_t *t, uint8_t kind, uint32_t rgb, int battery)
{
    if (kind >= FH1_PAD_KINDS) kind = FH1_PAD_FLAT;
    const int lvl = battery < 0 ? -1 : battery / 10;
    if (!tray_begin(t, 0x80000000u | (uint32_t)kind << 28 | (rgb & 0xFFFFFF) << 4 | (uint32_t)(lvl + 1))) return;
    const int W = TRAY_W * s_u;
    for (int y = 0; y < 7; y++) {
        for (int x = 0; x < TRAY_W; x++) {
            char c = s_pad_shapes[kind][y][x];
            if (c != '.') tray_px(t->px, W, x, y, c == 'x' ? argb(rgb) : sprite_colour(c));
        }
    }
    if (battery >= 0 && t->h >= TRAY_H) {
        int len = (battery * TRAY_W + 50) / 100;
        if (len < 1) len = 1;
        for (int x = 0; x < TRAY_W; x++) {
            const uint32_t c = x < len ? argb(level_colour(battery, 100)) : argb(0x3a3f46);
            tray_px(t->px, W, x, 8, c);
            tray_px(t->px, W, x, 9, c);
        }
    }
    tray_end(t);
}

// A gamepad's icon for the interface (lists, pickers): its own buffer, freed with it.
static void pad_icon_deleted(lv_event_t *e)
{
    tray_t *t = lv_event_get_user_data(e);
    if (t->px) {
        lv_image_cache_drop(&t->dsc);
        heap_caps_free(t->px);
    }
    heap_caps_free(t);
}

lv_obj_t *fh1_ui_pad_icon(lv_obj_t *parent, uint8_t kind, uint32_t rgb)
{
    lv_obj_t *im = lv_image_create(parent);
    tray_t *t = heap_caps_calloc(1, sizeof(*t), MALLOC_CAP_SPIRAM);
    if (!t) return im;
    t->img = im;
    t->h = PAD_H;
    lv_obj_set_user_data(im, t);
    lv_obj_add_event_cb(im, pad_icon_deleted, LV_EVENT_DELETE, t);
    tray_pad(t, kind, rgb, -1);
    return im;
}

void fh1_ui_pad_icon_set(lv_obj_t *icon, uint8_t kind, uint32_t rgb)
{
    tray_t *t = icon ? lv_obj_get_user_data(icon) : NULL;
    if (t) tray_pad(t, kind, rgb, -1);
}

lv_obj_t *fh1_ui_lead_pad(lv_obj_t *obj, uint8_t kind, uint32_t rgb)
{
    lv_obj_t *im = fh1_ui_pad_icon(obj, kind, rgb);
    if (lv_obj_check_type(obj, &lv_label_class)) {
        // A line: the icon inside the label (a label lays nothing out), the text after it. Children align in
        // the content area, which starts after the padding: the icon goes back into it.
        lv_obj_set_style_pad_left(obj, U(TRAY_W + 3), 0);
        lv_obj_align(im, LV_ALIGN_LEFT_MID, -U(TRAY_W + 3), 0);
    } else {
        lv_obj_align(im, LV_ALIGN_LEFT_MID, 0, 0);       // a button: child 1 stays the text
        lv_obj_set_style_pad_left(lv_obj_get_child(obj, 1), U(TRAY_W + 3), 0);
    }
    return im;
}

// The sound: an orange speaker; waves for the volume (one to three), a red cross when it is 0.
static void tray_sound(tray_t *t, int volume)
{
    const int waves = volume <= 0 ? 0 : volume < 34 ? 1 : volume < 67 ? 2 : 3;
    if (!tray_begin(t, 0x40000000u | (uint32_t)waves)) return;
    const int W = TRAY_W * s_u;
    static const char *const body[7] = { "...oo", "..ooo", "ooooo", "ooooo", "ooooo", "..ooo", "...oo" };
    for (int y = 0; y < 7; y++) {
        for (int x = 0; x < 5; x++) {
            if (body[y][x] == 'o') tray_px(t->px, W, x, y, sprite_colour('o'));
        }
    }
    if (!waves) {
        for (int i = 0; i < 5; i++) {
            tray_px(t->px, W, 6 + i, 1 + i, sprite_colour('r'));
            tray_px(t->px, W, 10 - i, 1 + i, sprite_colour('r'));
        }
    }
    for (int w = 0; w < waves; w++) {
        const int x = 6 + 2 * w, half = 1 + w;         // taller further out
        for (int y = 3 - half; y <= 3 + half; y++) tray_px(t->px, W, x, y, sprite_colour('c'));
    }
    tray_end(t);
}

void fh1_ui_set_status_fn(fh1_ui_status_fn fn) { s_status_fn = fn; }

static void repaint(void *arg) { lv_obj_invalidate(lv_screen_active()); }

// ── Freezing: while a full-screen program runs, the window system keeps nothing in internal RAM. Its heap
// is in PSRAM anyway (lv_mem_psram.c); the draw buffer, the one internal block it needs, is given back and
// taken again when the interface resumes. Only the UI task draws, so only it frees and allocates. ──

static lv_display_t *s_disp;
static void *s_drawbuf;
static TaskHandle_t s_ui_task;
#define DRAWBUF_BYTES (s_w * 32 * 2)

static void ui_freeze(void)
{
    if (!s_drawbuf) return;
    heap_caps_free(s_drawbuf);     // LVGL keeps the stale pointer, but nothing draws until ui_thaw()
    s_drawbuf = NULL;
}

static volatile bool s_drawbuf_retry;   // on each resume: try internal RAM again for a PSRAM draw buffer

static bool ui_thaw(void)
{
    static uint32_t last_try;
    if (s_drawbuf && esp_ptr_internal(s_drawbuf)) return true;
    // A PSRAM buffer (internal RAM was short, e.g. under a paused game's options) moves back to internal
    // RAM as soon as there is room: tried on each resume and every 2 s.
    if (s_drawbuf && !s_drawbuf_retry && tick_ms() - last_try < 2000) return true;
    s_drawbuf_retry = false;
    last_try = tick_ms();
    // The same rule as an app's fast memory: internal only above the reserve Wi-Fi needs.
    void *b = fh1_mem_fast(DRAWBUF_BYTES);
    if (b && s_drawbuf && !esp_ptr_internal(b)) {     // still no room: keep the PSRAM buffer we have
        heap_caps_free(b);
        return true;
    }
    if (!b) return s_drawbuf != NULL;
    heap_caps_free(s_drawbuf);
    s_drawbuf = b;
    lv_display_set_buffers(s_disp, s_drawbuf, NULL, DRAWBUF_BYTES, LV_DISPLAY_RENDER_MODE_PARTIAL);
    return true;
}

void fh1_ui_pause(bool paused, bool repaint_all)
{
    // Keys pressed while a program owned the screen are its own: drop them before the UI takes over.
    if (!paused) {
        fh1_input_flush();
        s_drawbuf_retry = true;
    }
    s_paused = paused;
    fh1_board_direct(paused);        // the program draws into the framebuffer itself; the UI flushes what it draws
    if (paused) {
        // Give the internal RAM back now when called from the UI task; otherwise the UI task does it on its
        // next pass, and the caller waits for that, so what it allocates next can have it.
        if (xTaskGetCurrentTaskHandle() == s_ui_task) ui_freeze();
        for (int i = 0; i < 50 && s_drawbuf; i++) vTaskDelay(pdMS_TO_TICKS(5));
    }
    // Resuming with a repaint redraws the whole screen over what the program drew; without, only what
    // changes is drawn (a menu over a frozen game picture).
    if (!paused && repaint_all) fh1_ui_async(repaint, NULL);
}

// Each status element is touched only when its value changes: in LVGL a restyle redraws the object, and the
// bars would otherwise be redrawn (and sent to an SPI panel) twice a second for nothing.
static void status_tick(lv_timer_t *t)
{
    static int last_bars = -1, last_sd = -1, last_pads = -1, last_sound = -1, last_batt = -2, last_colon = -1;
    fh1_ui_status_t st = { 0 };
    if (s_status_fn) s_status_fn(&st);
    int bars = !st.wifi_up ? 0 : st.wifi_rssi > -55 ? 4 : st.wifi_rssi > -65 ? 3 : st.wifi_rssi > -75 ? 2 : 1;
    if (bars != last_bars) {
        last_bars = bars;
        for (int i = 0; i < 4; i++) {
            icon_on(s_wifi[i], i < bars);
            if (i < bars) lv_obj_set_style_bg_color(s_wifi[i], lv_color_hex(level_colour(bars, 4)), 0);
        }
    }
    if (st.sd != last_sd) { last_sd = st.sd; lv_obj_set_style_image_opa(s_sd, st.sd ? LV_OPA_COVER : LV_OPA_30, 0); }
    if (st.pads != last_pads) {
        last_pads = st.pads;
        for (int i = 0; i < 4; i++) lv_obj_set_hidden(s_tray[i].img, i >= st.pads);
    }
    for (int i = 0; i < st.pads; i++) tray_pad(&s_tray[i], st.pad[i].kind, st.pad[i].rgb, st.pad[i].battery);   // redraws only on change
    if (st.sound != last_sound) { last_sound = st.sound; lv_obj_set_hidden(s_tray[4].img, !st.sound); }
    if (st.sound) tray_sound(&s_tray[4], st.volume);                                                               // likewise
    const int batt = !st.battery ? -1 : st.battery_pct < 0 ? 0 : st.battery_pct > 100 ? 100 : st.battery_pct;
    if (batt != last_batt) {
        last_batt = batt;
        lv_obj_set_hidden(s_batt, batt < 0);
        if (batt >= 0) {
            lv_obj_set_width(s_batt_fill, (U(10) - U(2)) * batt / 100);
            lv_obj_set_style_bg_color(s_batt_fill, lv_color_hex(level_colour(batt, 100)), 0);
        }
    }
    // The colon follows the seconds: shown on even ones, hidden on odd ones, so it changes once a second
    // however often this runs.
    char hh[4] = "--", mm[4] = "--";
    time_t now = time(NULL);
    bool colon = (esp_timer_get_time() / 1000000) % 2 == 0;
    if (now > 1700000000) {
        struct tm tm;
        localtime_r(&now, &tm);
        snprintf(hh, sizeof(hh), "%02d", tm.tm_hour);
        snprintf(mm, sizeof(mm), "%02d", tm.tm_min);
        colon = tm.tm_sec % 2 == 0;
    }
    if (strcmp(lv_label_get_text(s_clk_h), hh) != 0) lv_label_set_text(s_clk_h, hh);
    if (strcmp(lv_label_get_text(s_clk_m), mm) != 0) lv_label_set_text(s_clk_m, mm);
    if (colon != last_colon) { last_colon = colon; lv_obj_set_style_text_opa(s_clk_colon, colon ? LV_OPA_COVER : LV_OPA_TRANSP, 0); }
}

void fh1_ui_set_tabs(const char *const *labels, const int *icons, int n, int current)
{
    lv_obj_clean(s_tabs);
    for (int i = 0; i < n; i++) {
        lv_obj_t *t = plain(s_tabs);              // the icon, then the word; the current tab highlighted
        lv_obj_set_size(t, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(t, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(t, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(t, U(2), 0);
        lv_obj_set_style_pad_hor(t, U(3), 0);
        if (i == current) lv_obj_add_style(t, &st_tab_on, 0);
        if (icons && icons[i] >= 0) fh1_ui_icon(t, (fh1_ui_icon_t)icons[i], false, false);
        lv_obj_t *l = lv_label_create(t);
        lv_label_set_text(l, labels[i]);
    }
}

static lv_obj_t *bar(lv_obj_t *scr, bool bottom)
{
    lv_obj_t *b = plain(scr);
    lv_obj_add_style(b, &st_bar, 0);
    lv_obj_set_size(b, s_w, s_bar_h);
    lv_obj_set_style_border_side(b, bottom ? LV_BORDER_SIDE_TOP : LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_hor(b, U(3), 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, U(3), 0);
    if (bottom) lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, 0);
    return b;
}

static void build_desktop(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_image_tiled(scr, true, 0);
    lv_obj_set_scrollable(scr, false);
    s_bar_h = s_line + U(4);

    // Top: the top-level menus, as tabs.
    s_tabs = bar(scr, false);
    lv_obj_set_style_pad_column(s_tabs, U(1), 0);

    // Bottom: live status. Clock and battery on the left; SD card and Wi-Fi bars on the right.
    s_sbar = bar(scr, true);
    // The clock: hours, colon, minutes as three labels, so the colon blinks by its opacity and the digits
    // never move (a space in its place is narrower).
    s_clock = plain(s_sbar);
    lv_obj_set_size(s_clock, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_clock, LV_FLEX_FLOW_ROW);
    s_clk_h = lv_label_create(s_clock);
    s_clk_colon = lv_label_create(s_clock);
    lv_label_set_text(s_clk_colon, ":");
    s_clk_m = lv_label_create(s_clock);
    s_batt = plain(s_sbar);
    lv_obj_set_size(s_batt, U(12), U(6));
    lv_obj_t *outline = plain(s_batt);
    lv_obj_set_size(outline, U(10), U(6));
    lv_obj_set_style_border_width(outline, s_u, 0);
    lv_obj_add_style(outline, &st_icon, 0);
    lv_obj_set_style_bg_opa(outline, LV_OPA_TRANSP, 0);
    lv_obj_t *nub = plain(s_batt);
    lv_obj_add_style(nub, &st_icon, 0);
    lv_obj_set_size(nub, U(1), U(2));
    lv_obj_align(nub, LV_ALIGN_RIGHT_MID, -U(1), 0);
    s_batt_fill = plain(outline);
    lv_obj_add_style(s_batt_fill, &st_icon, 0);
    lv_obj_set_size(s_batt_fill, 0, U(2));
    lv_obj_align(s_batt_fill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *spacer = plain(s_sbar);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_height(spacer, 1);
    // The tray: gamepads, then the sound (shown once there is something to show).
    for (int i = 0; i < TRAY_ITEMS; i++) {
        s_tray[i].img = lv_image_create(s_sbar);
        lv_obj_set_hidden(s_tray[i].img, true);
    }
    s_sd = lv_image_create(s_sbar);
    lv_image_set_src(s_sd, &sp_sd.dsc);
    lv_obj_t *wifi = plain(s_sbar);
    lv_obj_set_size(wifi, U(11), U(8));
    for (int i = 0; i < 4; i++) {
        s_wifi[i] = plain(wifi);
        lv_obj_set_size(s_wifi[i], U(2), U(2) * (i + 1));
        lv_obj_align(s_wifi[i], LV_ALIGN_BOTTOM_LEFT, U(3) * i, 0);
    }
    lv_timer_create(status_tick, 500, NULL);
}

// ── Keys ──

static void handle_key(int key)
{
    fh1_win_t *w = top();
    const int v = s_theme->voice;
    if (key == FH1_KEY_MENU || (!w && key == FH1_KEY_A)) {
        ui_sound(v, UI_SND_SELECT);
        if (s_menu_fn) s_menu_fn(s_menu_arg);
        return;
    }
    if (!w) return;
    if (key == FH1_KEY_A) ui_sound(v, UI_SND_SELECT);
    else if (key == FH1_KEY_B) ui_sound(v, UI_SND_BACK);
    else if (key == FH1_KEY_UP || key == FH1_KEY_DOWN || key == FH1_KEY_LEFT || key == FH1_KEY_RIGHT) ui_sound(v, UI_SND_MOVE);
    if (slider_key(w, key)) return;
    if (w->views && views_key(w, key)) return;
    if (w->list && list_key(w->list, key)) return;
    if (w->key_cb && w->key_cb(w, key, w->key_user)) return;
    switch (key) {
    case FH1_KEY_UP:
    case FH1_KEY_LEFT:
        lv_group_focus_prev(w->group);
        break;
    case FH1_KEY_DOWN:
    case FH1_KEY_RIGHT:
        lv_group_focus_next(w->group);
        break;
    case FH1_KEY_A: {
        lv_obj_t *f = lv_group_get_focused(w->group);
        if (f) lv_obj_send_event(f, LV_EVENT_CLICKED, NULL);
        return;
    }
    case FH1_KEY_B:
        fh1_win_close(w);
        return;
    case FH1_KEY_SELECT:
        if (s_nwins > 1) raise(s_wins[0]);
        return;
    default:
        return;
    }
    lv_obj_t *f = lv_group_get_focused(w->group);
    if (f) lv_obj_scroll_to_view(f, LV_ANIM_OFF);
    restyle();
}

// ── Notifications: a small box slides in at the top right, under the bar: a picture (animated: redrawn by
// its function every 120 ms), a title and a line; it stays 4 s and slides out. One at a time, the others wait
// in a queue (6); while a full-screen program owns the screen they wait for it to end. ──

#define NOTE_Q 6
#define NOTE_MS 4000
#define NOTE_SLIDE_MS 220
typedef struct {
    char title[40], text[96];
    int w, h;
    fh1_ui_pic_fn draw;
    uint8_t user[64];
} note_t;
static note_t *s_noteq;                // NOTE_Q of them, in PSRAM (internal RAM is for Wi-Fi)
static int s_note_n;
static struct {
    lv_obj_t *box, *pic;
    lv_timer_t *timer;
    uint32_t *px, shown_at;
    note_t cur;
    bool leaving;
} s_note;

static void note_x(void *var, int32_t v) { lv_obj_set_x(var, v); }

static void note_gone(lv_anim_t *a)
{
    if (s_note.box) lv_obj_delete(s_note.box);
    s_note.box = s_note.pic = NULL;
    s_note.leaving = false;
}

static void note_slide(int32_t from, int32_t to, lv_anim_completed_cb_t done)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_note.box);
    lv_anim_set_exec_cb(&a, note_x);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, NOTE_SLIDE_MS);
    lv_anim_set_path_cb(&a, to < from ? lv_anim_path_ease_out : lv_anim_path_ease_in);
    if (done) lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
}

static void note_show(const note_t *n)
{
    ui_sound(s_theme->voice, UI_SND_NOTE);
    s_note.cur = *n;
    const int W = U(176);
    lv_obj_t *b = s_note.box = plain(lv_layer_top());
    lv_obj_add_style(b, &st_box, 0);
    lv_obj_set_size(b, W, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(b, U(4), 0);
    lv_obj_set_style_pad_column(b, U(4), 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_note.pic = n->draw && n->w > 0 ? fh1_ui_pixels(b, n->w, n->h, 1) : NULL;
    if (s_note.pic && !s_note.px) s_note.px = heap_caps_malloc(64 * 64 * 4, MALLOC_CAP_SPIRAM);
    lv_obj_t *col = plain(b);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, U(2), 0);
    lv_obj_t *t = lv_label_create(col);
    lv_obj_add_style(t, &st_text, 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(t, LV_PCT(100));
    lv_label_set_text(t, n->title);
    lv_obj_t *l = lv_label_create(col);
    lv_obj_add_style(l, &st_text, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(s_theme->dim), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_text(l, n->text);
    lv_obj_set_y(b, s_bar_h + U(3));
    if (s_note.pic && s_note.px && n->w * n->h <= 64 * 64) {
        n->draw(n->user, tick_ms(), s_note.px);
        fh1_ui_pixels_set(s_note.pic, s_note.px);
    }
    s_note.shown_at = tick_ms();
    note_slide(s_w, s_w - W - U(4), NULL);
}

static void note_tick(lv_timer_t *t)
{
    const uint32_t now = tick_ms();
    if (s_note.box && !s_note.leaving) {
        if (s_note.pic && s_note.px && s_note.cur.w * s_note.cur.h <= 64 * 64) {
            s_note.cur.draw(s_note.cur.user, now, s_note.px);
            fh1_ui_pixels_set(s_note.pic, s_note.px);
        }
        if (now - s_note.shown_at >= NOTE_MS) {
            s_note.leaving = true;
            note_slide(lv_obj_get_x(s_note.box), s_w, note_gone);
        }
        return;
    }
    if (!s_note.box && s_note_n && !s_paused) {
        note_t n = s_noteq[0];
        memmove(s_noteq, s_noteq + 1, (size_t)(--s_note_n) * sizeof(note_t));
        note_show(&n);
    }
}

void fh1_ui_notify(const char *title, const char *text, int pic_w, int pic_h, fh1_ui_pic_fn draw, const void *user, size_t user_len)
{
    if (!s_noteq) s_noteq = heap_caps_calloc(NOTE_Q, sizeof(note_t), MALLOC_CAP_SPIRAM);
    if (!s_noteq) return;
    if (s_note_n >= NOTE_Q) memmove(s_noteq, s_noteq + 1, (size_t)(--s_note_n) * sizeof(note_t));   // the oldest goes
    note_t *n = &s_noteq[s_note_n++];
    memset(n, 0, sizeof(*n));
    strlcpy(n->title, title ? title : "", sizeof(n->title));
    strlcpy(n->text, text ? text : "", sizeof(n->text));
    n->w = pic_w, n->h = pic_h, n->draw = draw;
    if (user && user_len) memcpy(n->user, user, user_len < sizeof(n->user) ? user_len : sizeof(n->user));
    if (!s_note.timer) s_note.timer = lv_timer_create(note_tick, 120, NULL);
}

// ── Lending the screen to an overlay while a program runs (fh1_ui_lend_screen) ──
// The program stops at a frame's end; the UI takes a copy of its picture, shows it on a screen of its own
// (so no window or bar is drawn over the game) with the overlay on top, and when the overlay is gone, goes
// back to the windows' screen without drawing it and hands the screen back.

typedef enum { LEND_NONE, LEND_ASKED, LEND_ON, LEND_DONE } lend_t;
static volatile lend_t s_lend;
static lv_obj_t *s_lend_scr, *s_lend_prev, *s_lend_img;
static lv_image_dsc_t s_lend_dsc;
static uint16_t *s_lend_px;

void fh1_ui_lend_screen(void)
{
    s_lend = LEND_ASKED;
    while (s_lend != LEND_DONE) vTaskDelay(pdMS_TO_TICKS(10));
    s_lend = LEND_NONE;
}

static void lend_start(void)
{
    int w, h;
    const uint16_t *fb = fh1_board_framebuffer(&w, &h);
    s_lend_px = fb ? heap_caps_malloc(w * h * 2, MALLOC_CAP_SPIRAM) : NULL;
    if (!s_lend_px) {                      // no copy, no overlay now: it shows when the program ends
        fh1_app_request_yield(false);
        s_lend = LEND_DONE;
        return;
    }
    memcpy(s_lend_px, fb, w * h * 2);
    s_lend_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_lend_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_lend_dsc.header.w = w;
    s_lend_dsc.header.h = h;
    s_lend_dsc.header.stride = w * 2;
    s_lend_dsc.data_size = w * h * 2;
    s_lend_dsc.data = (const uint8_t *)s_lend_px;
    if (!s_lend_scr) {
        s_lend_scr = lv_obj_create(NULL);
        lv_obj_remove_style_all(s_lend_scr);
        s_lend_img = lv_image_create(s_lend_scr);
    }
    lv_image_set_src(s_lend_img, &s_lend_dsc);
    s_lend_prev = lv_screen_active();
    lv_screen_load(s_lend_scr);
    s_lend = LEND_ON;
}

static bool lend_over(void) { return !s_ov.box || fh1_app_quit_requested(); }

static void lend_end(void)
{
    lv_refr_now(s_disp);                   // the overlay's last trace replaced by the picture
    lv_screen_load(s_lend_prev);
    lv_inv_area(s_disp, NULL);             // the windows' screen is not drawn: the program's picture is back
    lv_image_set_src(s_lend_img, NULL);
    heap_caps_free(s_lend_px);
    s_lend_px = NULL;
    fh1_app_request_yield(false);
    ui_freeze();
    s_lend = LEND_DONE;
}

// ── Graceful restart (FH1_EV_SHUTDOWN): the running program closes (and saves), the windows close one by
// one in reverse cascade order with their closing animation, then the chip restarts. ──

static lv_timer_t *s_shut;
static uint32_t s_shut_t0, s_shut_last;

static void shutdown_step(lv_timer_t *t)
{
    uint32_t now = tick_ms();
    if (s_paused && now - s_shut_t0 < 5000) return;          // the program is closing
    if (s_nwins) {
        if (now - s_shut_last >= 140) {
            fh1_win_close(top());
            s_shut_last = now;
        }
        return;
    }
    if (s_ov.box && now - s_shut_t0 < 6000) return;           // an overlay finishing ("Restarting...")
    if (now - s_shut_last < 450) return;                      // the last window's animation
    ESP_LOGI(TAG, "restarting");
    esp_restart();
}

static void on_shutdown(fh1_event_id_t id, const void *data, size_t len, void *user)
{
    if (s_shut) return;
    fh1_app_request_quit();
    s_shut_t0 = s_shut_last = tick_ms();
    s_shut = lv_timer_create(shutdown_step, 40, NULL);
}

// ── UI task ──

// Anything that holds the UI task for long makes the device feel hung: log it with its cause.
static void slow(const char *what, int64_t since)
{
    int64_t ms = (esp_timer_get_time() - since) / 1000;
    if (ms > 150) ESP_LOGW(TAG, "UI blocked %lld ms by %s", ms, what);
}

static void ui_task(void *arg)
{
    uint32_t gen = fh1_dev_generation();
    for (;;) {
        ui_call_t c;
        int64_t t0 = esp_timer_get_time();
        while (xQueueReceive(s_calls, &c, 0) == pdTRUE) c.fn(c.arg);
        slow("queued calls", t0);
        t0 = esp_timer_get_time();
        fh1_input_event_t ev;
        while (fh1_input_get(&ev)) {
            // While paused (or lending the screen), keys belong to the program; while shutting down, to nobody.
            if (ev.pressed && !s_paused && !s_shut) handle_key(ev.key);
        }
        if (s_paused && s_lend == LEND_ASKED) lend_start();
        if (s_paused && s_lend != LEND_ON) {
            fh1_events_dispatch();         // an overlay's event asks the program for the screen
            ui_freeze();
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (!ui_thaw()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        slow("key handling", t0);
        fh1_events_dispatch();
        if (gen != fh1_dev_generation()) {
            t0 = esp_timer_get_time();
            gen = fh1_dev_generation();
            for (int i = 0; i < s_nwatchers; i++) s_watchers[i].fn(s_watchers[i].arg);
            slow("device-change refresh", t0);
        }
        t0 = esp_timer_get_time();
        uint32_t wait = lv_timer_handler();
        slow("drawing and timers", t0);
        if (s_lend == LEND_ON && lend_over()) lend_end();
        vTaskDelay(pdMS_TO_TICKS(wait < 5 ? 5 : (wait > 20 ? 20 : wait)));
    }
}

esp_err_t fh1_ui_start(void)
{
    const fh1_display_mode_t *mode = fh1_board_display_mode();
    s_w = mode->w;
    s_h = mode->h;
    s_u = mode->ui_scale;
    s_font = &fh1_font_8;
    s_line = lv_font_get_line_height(s_font);
    char theme[12];
    fh1_settings_get_str("ui", "theme", theme, sizeof(theme));
    for (size_t i = 0; i < NTHEMES; i++) {
        if (strcmp(s_themes[i].id, theme) == 0) s_theme = &s_themes[i];
    }

    lv_init();
    lv_tick_set_cb(tick_ms);
    lv_display_t *disp = lv_display_create(s_w, s_h);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    // One draw buffer in internal RAM: the flush is a plain copy into the frame buffer, so a second
    // buffer would buy little.
    s_disp = disp;
    if (!ui_thaw()) return ESP_ERR_NO_MEM;
    lv_display_set_flush_cb(disp, flush_cb);
    make_cursors();
    init_styles();
    make_sprites();      // the desktop's status icons use them before the theme is applied
    build_desktop();
    apply_theme();
    status_tick(NULL);
    make_fps_overlay();
    lv_timer_create(nudge_tick, 600, NULL);
    lv_timer_create(look_tick, 125, NULL);
    lv_timer_create(mote_tick, 50, NULL);
    fh1_event_subscribe(FH1_EV_UI_IDLE_FLOURISH, on_flourish, NULL);
    fh1_event_subscribe(FH1_EV_TRANSFER, on_transfer, NULL);
    fh1_event_subscribe(FH1_EV_SHUTDOWN, on_shutdown, NULL);
    // The system's own file types; apps add theirs (and may replace these).
    fh1_ui_register_file_type("fpk", FH1_ICON_PACKAGE, "Receiving a package", "Package received");
    fh1_ui_register_file_type("bin", FH1_ICON_FIRMWARE, "Receiving firmware", "Firmware received");
    fh1_event_every(FH1_EV_UI_IDLE_FLOURISH, 8000, 12000);
    s_calls = xQueueCreate(16, sizeof(ui_call_t));
    if (xTaskCreatePinnedToCore(ui_task, "ui", 12 * 1024, NULL, 3, &s_ui_task, 1) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the UI task");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "UI up: %s, %dx%d, unit %d, theme %s", mode->name, s_w, s_h, s_u, s_theme->name);
    return ESP_OK;
}
