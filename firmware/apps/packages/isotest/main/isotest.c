// Iso test: the FH1's isometric engine as a small game (game.h). The main loop: keys once per frame, then the
// current screen; a screen's frame returns the next one.
#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KEYS (FH1_KEY_MENU + 1)

keys_t g_keys;
fart_t g_art;
uint32_t g_t;
bool g_info;

static screen_t s_screen = SCR_TITLE;
static uint32_t s_rep_t, s_frame_ms;

static void read_keys(uint32_t dt)
{
    uint32_t down = 0;
    for (int k = 0; k < KEYS; k++)
        if (fh1_input_is_down(k)) down |= 1u << k;
    g_keys.edge = down & ~g_keys.down;
    // The D-pad repeats while held: first after 300 ms, then every 110 ms.
    static const int keys[4] = { FH1_KEY_UP, FH1_KEY_RIGHT, FH1_KEY_DOWN, FH1_KEY_LEFT };
    static const int dirs[4] = { FART_NE, FART_SE, FART_SW, FART_NW };
    g_keys.dir = -1;
    bool any = false;
    for (int i = 0; i < 4; i++) {
        const uint32_t bit = 1u << keys[i];
        if (!(down & bit)) continue;
        any = true;
        if (g_keys.edge & bit) {
            g_keys.dir = dirs[i];
            s_rep_t = 0;
        } else if (s_rep_t > 300) {
            g_keys.dir = dirs[i];
            s_rep_t = 190;
        }
    }
    s_rep_t = any ? s_rep_t + dt : 0;
    g_keys.down = down;
    // Turning the view: Select, or a controller's shoulder buttons (R1 the same way as Select, L1 back).
    static uint32_t s_pad_prev;
    fh1_pad_t pad;
    const uint32_t pb = fh1_pad(0, &pad) ? pad.buttons : 0, pe = pb & ~s_pad_prev;
    s_pad_prev = pb;
    g_keys.turn = (g_keys.edge & (1u << FH1_KEY_SELECT)) || (pe & FH1_PAD_BTN_R1) ? 1 : (pe & FH1_PAD_BTN_L1) ? -1 : 0;
}

int menu_key(void)
{
    if (g_keys.dir == FART_NE) return FH1_KEY_UP;
    if (g_keys.dir == FART_SW) return FH1_KEY_DOWN;
    if (key(FH1_KEY_A)) return FH1_KEY_A;
    if (key(FH1_KEY_B)) return FH1_KEY_B;
    return -1;
}

// A night-blue gradient with a faint diamond grid, like the map seen from afar.
static void paint_back(uint16_t *px, int w, int h)
{
    for (int y = 0; y < h; y++) {
        const int k = y * 12 / h;
        const uint16_t base = (uint16_t)(((2 + k / 3) << 11) | ((5 + k) << 5) | (10 + k));
        uint16_t *line = px + y * w;
        for (int x = 0; x < w; x++) {
            const int a = (x + 2 * y) & 31, b = (x - 2 * y) & 31;
            line[x] = (a == 0 || b == 0) ? (uint16_t)(base + 0x0861) : base;
        }
    }
}

// The background never changes: painted once, then copied each frame (it was most of a menu frame's work).
static uint16_t *s_back;
static int s_back_w, s_back_h;

void draw_back(const ui_t *u)
{
    if (!s_back || s_back_w != u->w || s_back_h != u->h) {
        free(s_back);
        s_back = malloc((size_t)u->w * u->h * 2);
        if (!s_back) { paint_back(u->px, u->w, u->h); return; }
        s_back_w = u->w;
        s_back_h = u->h;
        paint_back(s_back, u->w, u->h);
    }
    memcpy(u->px, s_back, (size_t)u->w * u->h * 2);
}

void hero_sprite(const ui_t *u, int cls, int x, int y, int dir)
{
    char n[24];
    snprintf(n, sizeof(n), "%s.idle", g_classes[cls].art);
    const fart_anim_t *a = fart_anim(&g_art, n);
    bool mirror;
    int idx = a ? fart_frame(a, dir, g_t, &mirror) : -1;
    if (idx < 0) {
        snprintf(n, sizeof(n), "%s.walk", g_classes[cls].art);
        idx = fart_frame(fart_anim(&g_art, n), dir, 0, &mirror);
    }
    if (idx < 0) return;
    const fart_clip_t clip = { 0, 0, u->w, u->h };
    fart_draw(u->px, u->w, &clip, &g_art.img[idx], x, y, mirror);
}

void hero_draw(const ui_t *u, const hero_t *h, int x, int y, int dir, bool walking, uint32_t t)
{
    char n[24];
    snprintf(n, sizeof(n), "%s.%s", g_classes[h->cls].art, walking ? "walk" : "idle");
    const fart_anim_t *a = fart_anim(&g_art, n);
    bool mirror;
    int idx = a ? fart_frame(a, dir, t, &mirror) : -1;
    if (idx < 0) {                                             // no idle: the walk's first frame
        snprintf(n, sizeof(n), "%s.walk", g_classes[h->cls].art);
        idx = fart_frame(fart_anim(&g_art, n), dir, 0, &mirror);
    }
    if (idx < 0) return;
    const fart_img_t *im = &g_art.img[idx];
    const fart_clip_t clip = { 0, 0, u->w, u->h };
    uint8_t cover;
    const uint16_t *ramp = armour_look(h->armour, &cover);
    int rest = 0, wimg = -1;
    const uint16_t *wshade = NULL;
    const char *wart = weapon_art(h->weapon, &rest, &wshade);
    if (wart) {
        snprintf(n, sizeof(n), "%s.0", wart);
        wimg = fart_image(&g_art, n);
    }
    const bool right = dir == FART_SE || dir == FART_NE, away = dir == FART_NE || dir == FART_NW;
    const int hx = im->hx != FART_NO_HAND ? x + (mirror ? -im->hx : im->hx) : x;
    const int hy = im->hx != FART_NO_HAND ? y + im->hy : y - 16;
    const fart_style_t ws = { .shade = wshade };
    for (int pass = 0; pass < 2; pass++) {
        // Facing away, the weapon is behind the body; facing the viewer, in front of it.
        if (wimg >= 0 && pass == (away ? 0 : 1))
            fart_draw_style(u->px, u->w, &clip, &g_art.img[iso_rot(wimg, right ? rest : 180 - rest)], hx, hy, false, &ws);
        if (pass == 0) {
            const fart_style_t st = { .cover = cover, .ramp = ramp };
            fart_draw_style(u->px, u->w, &clip, im, x, y, mirror, &st);
        }
    }
}

void hero_pose(uint32_t t, int *dir, bool *walking)
{
    // Round, a quarter at a time, walking all along: the heroes' art has no idle (one standing frame at most), so
    // a pause would hold one frame still. A turn every 1.6 s (two loops of the walk).
    static const int turn[4] = { FART_SE, FART_SW, FART_NW, FART_NE };
    *dir = turn[(t / 1600) % 4];
    *walking = true;
}

// ── Transitions: the old screen's last frame, replaced by the new one through a grid of growing diamonds ──

enum { TR_SWEEP, TR_BURST };
static uint16_t *s_old;
static uint32_t s_tr_t, s_tr_len;
static int s_tr_kind;

static void trans_start(const uint16_t *px, int w, int h, int kind)
{
    if (!s_old) s_old = malloc((size_t)w * h * 2);
    if (!s_old) return;
    memcpy(s_old, px, (size_t)w * h * 2);
    s_tr_t = 0;
    s_tr_kind = kind;
    s_tr_len = kind == TR_BURST ? 560 : 400;
}

static void trans_apply(uint16_t *px, int w, int h, uint32_t dt)
{
    if (!s_tr_len) return;
    s_tr_t += dt;
    if (s_tr_t >= s_tr_len) {
        s_tr_len = 0;
        return;
    }
    const int p = (int)(s_tr_t * 1024 / s_tr_len);              // 0..1023
    const int cols = (w + 15) / 16, rows = (h + 15) / 16;
    for (int cy = 0; cy < rows; cy++)
        for (int cx = 0; cx < cols; cx++) {
            // When this cell starts to open: left to right, or from the middle out.
            int delay;
            if (s_tr_kind == TR_SWEEP) delay = cx * 512 / cols + cy * 64 / rows;
            else {
                const int dx = abs(cx * 2 + 1 - cols), dy = abs(cy * 2 + 1 - rows);
                delay = (dx + dy) * 512 / (cols + rows);
            }
            int r = (p - delay) * 3 / 64;                            // the diamond's radius, 0..16 in ~340 units
            if (r >= 16) continue;                                   // fully the new screen
            for (int y = cy * 16; y < cy * 16 + 16 && y < h; y++) {
                uint16_t *dst = px + y * w;
                const uint16_t *src = s_old + y * w;
                const int dy = abs(y - cy * 16 - 8) + (y >= cy * 16 + 8);
                for (int x = cx * 16; x < cx * 16 + 16 && x < w; x++) {
                    const int dx = abs(x - cx * 16 - 8) + (x >= cx * 16 + 8);
                    if (r <= 0 || dx + dy > r) dst[x] = src[x];
                }
            }
        }
}

// ── Development commands: from the console on the FH1 (fh1_dev_command) and from the PC host's scripts ──

void battle_host_place(int i, int c, int r, int dir);
void battle_host_act(int a, int act, int what, int b);
void battle_host_counter(int i);
void battle_host_target(int a, int act, int what, int c, int r);
void battle_host_gear(int i, int weapon, int armour);
void battle_host_end(bool won);
void battle_host_ranged(int i, int range);
void battle_host_cam(int x, int y);
void battle_host_turn(int by);
void battle_host_view(int view);
void battle_host_bench(void);
void battle_host_turnstyle(int style);
void battle_host_relayer(void);
void battle_host_checklayer(void);
void battle_host_tier(int tier);
void battle_host_cast(int el, int tier, int c, int r);
void battle_host_crack(int c, int r, int power);
void battle_host_dropon(int c, int r, int tile);
void battle_host_float(int c, int r, int base, int bob);
void battle_host_kill(int i);
void battle_host_boss(void);
void battle_host_screen(int kind, int ms, int strength);
void battle_host_scenery(const char *what, int c, int r, int v, int fx);
void battle_host_xp(int slot, int xp);
void battle_host_log(void);
void battle_host_hp(int i, int hp);

static screen_t s_dev_next = SCR_QUIT;            // a screen a dev command asks for (SCR_QUIT: none)

// One command: place UNIT C R DIR, act UNIT KIND WHAT TARGET, counter UNIT, target UNIT KIND WHAT C R,
// gear UNIT WEAPON ARMOUR, ranged UNIT RANGE, cam X Y, xp SLOT N, hp UNIT N, win, lose, log (battle only); info,
// nosave (anywhere: the save is not written until the game closes); outside a battle: go REALM FASE BATTLE (to
// its loading screen), won REALM FASE N (battles won in a fase), level HERO N (a roster hero's level), coins N,
// give GOODS N (into the bag), screen NAME (title, party, realms, path, shop). False if unknown or not here.
bool dev_command(const char *line)
{
    char cmd[16] = "", a[16] = "";
    int n[5] = { 0 };
    if (sscanf(line, "%15s", cmd) != 1) return false;
    if (!strcmp(cmd, "info")) {
        g_info = !g_info;
        return true;
    }
    if (!strcmp(cmd, "nosave")) {                 // a test session: the saved game stays as it was
        g_nosave = true;
        fh1_log("dev: saving off until the game closes");
        return true;
    }
    // Anywhere (a battle under way is left): a battle, or the test field in a realm's colours.
    if (!strcmp(cmd, "go") && sscanf(line, "%*s %d %d %d", &n[0], &n[1], &n[2]) == 3 && n[0] >= 0 && n[0] < REALMS &&
        n[1] >= 0 && n[1] < TIERS && n[2] >= 0 && n[2] < g_tier_len[n[1]]) {
        battle_make(n[0], n[1], n[2], &g_battle);
        s_dev_next = SCR_LOAD;
        return true;
    }
    if (!strcmp(cmd, "test")) {
        battle_test(sscanf(line, "%*s %d", &n[0]) == 1 ? n[0] : 1, &g_battle);
        s_dev_next = SCR_LOAD;
        return true;
    }
    if (s_screen != SCR_BATTLE) {
        if (!strcmp(cmd, "won") && sscanf(line, "%*s %d %d %d", &n[0], &n[1], &n[2]) == 3 && n[0] >= 0 &&
                   n[0] < REALMS && n[1] >= 0 && n[1] < TIERS)
            g_save.won[n[0]][n[1]] = (uint8_t)(n[2] > g_tier_len[n[1]] ? g_tier_len[n[1]] : n[2]);
        else if (!strcmp(cmd, "level") && sscanf(line, "%*s %d %d", &n[0], &n[1]) == 2 && n[0] >= 0 && n[0] < ROSTER) {
            g_save.heroes[n[0]].level = (uint8_t)(n[1] < 1 ? 1 : n[1] > LEVEL_MAX ? LEVEL_MAX : n[1]);
            g_save.heroes[n[0]].xp = 0;
        } else if (!strcmp(cmd, "coins") && sscanf(line, "%*s %d", &n[0]) == 1) g_save.coins = (uint32_t)n[0];
        else if (!strcmp(cmd, "give") && sscanf(line, "%*s %d %d", &n[0], &n[1]) == 2 && n[0] >= 0 && n[0] < g_ngoods)
            g_save.bag[n[0]] = (uint8_t)n[1];
        else if (!strcmp(cmd, "screen") && sscanf(line, "%*s %15s", a) == 1) {
            static const struct { const char *name; screen_t s; } scr[] = {
                { "title", SCR_TITLE }, { "party", SCR_PARTY }, { "realms", SCR_REALM }, { "path", SCR_PICK }, { "shop", SCR_SHOP },
            };
            s_dev_next = SCR_QUIT;
            for (size_t k = 0; k < sizeof(scr) / sizeof(scr[0]); k++)
                if (!strcmp(a, scr[k].name)) s_dev_next = scr[k].s;
            if (s_dev_next == SCR_QUIT) return false;
        } else return false;
        return true;
    }
    if (!strcmp(cmd, "place") && sscanf(line, "%*s %d %d %d %d", &n[0], &n[1], &n[2], &n[3]) == 4)
        battle_host_place(n[0], n[1], n[2], n[3]);
    else if (!strcmp(cmd, "act") && sscanf(line, "%*s %d %d %d %d", &n[0], &n[1], &n[2], &n[3]) == 4)
        battle_host_act(n[0], n[1], n[2], n[3]);
    else if (!strcmp(cmd, "counter") && sscanf(line, "%*s %d", &n[0]) == 1) battle_host_counter(n[0]);
    else if (!strcmp(cmd, "target") && sscanf(line, "%*s %d %d %d %d %d", &n[0], &n[1], &n[2], &n[3], &n[4]) == 5)
        battle_host_target(n[0], n[1], n[2], n[3], n[4]);
    else if (!strcmp(cmd, "gear") && sscanf(line, "%*s %d %d %d", &n[0], &n[1], &n[2]) == 3) battle_host_gear(n[0], n[1], n[2]);
    else if (!strcmp(cmd, "ranged") && sscanf(line, "%*s %d %d", &n[0], &n[1]) == 2) battle_host_ranged(n[0], n[1]);
    else if (!strcmp(cmd, "cam") && sscanf(line, "%*s %d %d", &n[0], &n[1]) == 2) battle_host_cam(n[0], n[1]);
    else if (!strcmp(cmd, "turn") && sscanf(line, "%*s %d", &n[0]) == 1) battle_host_turn(n[0]);
    else if (!strcmp(cmd, "view") && sscanf(line, "%*s %d", &n[0]) == 1) battle_host_view(n[0]);
    else if (!strcmp(cmd, "bench")) battle_host_bench();
    else if (!strcmp(cmd, "relayer")) battle_host_relayer();
    else if (!strcmp(cmd, "checklayer")) battle_host_checklayer();
    else if (!strcmp(cmd, "tier") && sscanf(line, "%*s %d", &n[0]) == 1) battle_host_tier(n[0]);
    else if (!strcmp(cmd, "cast") && sscanf(line, "%*s %d %d %d %d", &n[0], &n[1], &n[2], &n[3]) == 4)
        battle_host_cast(n[0], n[1], n[2], n[3]);
    else if (!strcmp(cmd, "crack") && sscanf(line, "%*s %d %d %d", &n[0], &n[1], &n[2]) == 3) battle_host_crack(n[0], n[1], n[2]);
    else if (!strcmp(cmd, "dropon") && sscanf(line, "%*s %d %d %d", &n[0], &n[1], &n[2]) == 3) battle_host_dropon(n[0], n[1], n[2]);
    else if (!strcmp(cmd, "float") && sscanf(line, "%*s %d %d %d %d", &n[0], &n[1], &n[2], &n[3]) == 4)
        battle_host_float(n[0], n[1], n[2], n[3]);
    else if (!strcmp(cmd, "kill") && sscanf(line, "%*s %d", &n[0]) == 1) battle_host_kill(n[0]);
    else if (!strcmp(cmd, "boss")) battle_host_boss();
    else if (!strcmp(cmd, "screen") && sscanf(line, "%*s %d %d %d", &n[0], &n[1], &n[2]) == 3) battle_host_screen(n[0], n[1], n[2]);
    else if (!strcmp(cmd, "turnstyle") && sscanf(line, "%*s %d", &n[0]) == 1) battle_host_turnstyle(n[0]);
    else if ((!strcmp(cmd, "obj") || !strcmp(cmd, "height") || !strcmp(cmd, "top")) &&
             sscanf(line, "%*s %d %d %d %d", &n[0], &n[1], &n[2], &n[3]) == 4)
        battle_host_scenery(cmd, n[0], n[1], n[2], n[3]);
    else if (!strcmp(cmd, "xp") && sscanf(line, "%*s %d %d", &n[0], &n[1]) == 2) battle_host_xp(n[0], n[1]);
    else if (!strcmp(cmd, "hp") && sscanf(line, "%*s %d %d", &n[0], &n[1]) == 2) battle_host_hp(n[0], n[1]);
    else if (!strcmp(cmd, "win")) battle_host_end(true);
    else if (!strcmp(cmd, "lose")) battle_host_end(false);
    else if (!strcmp(cmd, "log")) battle_host_log();
    else return false;
    return true;
}

static void enter(screen_t s)
{
    switch (s) {
    case SCR_TITLE: title_enter(); break;
    case SCR_PARTY: snd_unload(SND_SET_BATTLE); party_enter(); break;
    case SCR_REALM: realm_enter(); break;
    case SCR_PICK: pick_enter(); break;
    case SCR_LOAD: load_enter(); break;
    case SCR_SHOP: shop_enter(); break;
    default: break;
    }
}

static int frame(uint16_t *px, int w, int h, uint32_t dt, void *user)
{
    const uint32_t t0 = fh1_millis();
    g_t += dt;
    read_keys(dt);
    char dev[64];
    while (fh1_dev_command(dev, sizeof(dev)))
        if (!dev_command(dev)) fh1_log("dev: not understood here: %s", dev);
    const ui_t u = { px, w, h };
    screen_t next = s_screen;
    switch (s_screen) {
    case SCR_TITLE: next = title_frame(&u, dt); break;
    case SCR_PARTY: next = party_frame(&u, dt); break;
    case SCR_REALM: next = realm_frame(&u, dt); break;
    case SCR_PICK: next = pick_frame(&u, dt); break;
    case SCR_LOAD: next = load_frame(&u, dt); break;
    case SCR_BATTLE: next = battle_frame(&u, dt); break;
    case SCR_SHOP: next = shop_frame(&u, dt); break;
    default: break;
    }
    if (s_dev_next != SCR_QUIT) {
        next = s_dev_next;
        s_dev_next = SCR_QUIT;
    }
    // Another user took player 1's controller over: their own progress, from the party screen (a battle under way
    // is left: its heroes were the other user's).
    static uint32_t s_who_t;
    char who[24];
    if ((s_who_t += dt) >= 500 && (s_who_t = 0, !game_owner_now(who, sizeof(who)))) {
        if (s_screen == SCR_BATTLE) battle_leave();
        game_load();
        party_notice(who);
        if (s_screen != SCR_TITLE) next = SCR_PARTY;
        if (s_screen == SCR_PARTY) party_enter();
        fh1_log("player 1 is now %s: their progress loaded", who[0] ? who : "(nobody)");
    }
    trans_apply(px, w, h, dt);
    if (g_info) {
        char line[24];
        snprintf(line, sizeof(line), "%u ms", (unsigned)s_frame_ms);
        ui_text(&u, w - 48, h - 12, line, 0xffffff);
    }
    s_frame_ms = fh1_millis() - t0;
    if (next == SCR_QUIT) return FH1_FRAME_END;
    if (next != s_screen) {
        if (s_screen == SCR_BATTLE) battle_leave();               // (a dev command's jump out of a battle)
        trans_start(px, w, h, next == SCR_BATTLE ? TR_BURST : TR_SWEEP);
        s_screen = next;
        enter(next);
        g_keys.edge = 0;
    }
    return FH1_FRAME_SHOW;
}

static void menu_sound(int what)
{
    static const snd_t map[4] = { S_HOVER, S_CONFIRM, S_DECLINE, S_DENIED };
    snd_play(map[what], what == UI_SND_MOVE ? 150 : 210);
}

static void closed(void *user)
{
    snd_close();
    if (s_screen == SCR_BATTLE) battle_leave();
    fart_free(&g_art);
    free(s_old);
    s_old = NULL;
    free(s_back);
    s_back = NULL;
    fh1_app_exit();
}

int app_main(int argc, char *argv[])
{
    char path[96];
    snprintf(path, sizeof(path), "%s/assets/art.fart", fh1_app_dir());
    if (!fart_load(&g_art, path)) {
        fh1_ui_message("Táticas da Ilha", "Faltam os gráficos do jogo (assets/art.fart).");
        return 0;
    }
    game_load();
    // The menus' sounds now (small); a battle's before it starts. Without the asset package the game is silent.
    ui_sound = menu_sound;
    if (snd_open()) {
        snd_load_begin(SND_SET_MENU);
        while (snd_load_step(256 * 1024) < 1.0f) {}
    } else fh1_log("no sounds: install org.fh1.assets.rpg-sfx");
    for (int k = 0; k < KEYS; k++)
        if (fh1_input_is_down(k)) g_keys.down |= 1u << k;   // a key held on entry is not a press
    enter(SCR_TITLE);
    if (!fh1_screen_open(33, frame, closed, NULL)) {
        fh1_ui_message("Táticas da Ilha", "Não foi possível usar a tela.");
        fart_free(&g_art);
    }
    return 0;
}
