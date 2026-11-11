// The end of a battle, over the battlefield: the box opens, each enemy knocked out pops in with its coins and XP,
// the coins count up, then each hero's XP bar fills, level by level; a hero that levels up cheers (hops, with
// sparks) under "Subiu de nível!" and what it gained. A win that clears its fase says what it opened.
#include "game.h"
#include <stdio.h>
#include <string.h>

#define T_OPEN 250
#define T_FOE 380                    // per enemy
#define T_COINS 700
#define T_HERO 900                   // per hero

static int len8(const char *s)
{
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if ((*p & 0xC0) != 0x80) n++;
    return n * 8;
}

static uint32_t t_foes(void) { return T_OPEN + 200; }
static uint32_t t_coins(void) { return t_foes() + g_result.nkills * T_FOE + 150; }
static uint32_t t_heroes(void) { return t_coins() + T_COINS + 200; }
uint32_t summary_length(void) { return t_heroes() + g_result.nparty * T_HERO + 200; }

static float progress(uint32_t t, uint32_t start, uint32_t len)
{
    if (t <= start) return 0.0f;
    if (t >= start + len) return 1.0f;
    return (float)(t - start) / len;
}

static void sprite(const ui_t *u, const char *art, const uint16_t *shade, int x, int y, int dir, uint32_t t)
{
    char n[32];
    snprintf(n, sizeof(n), "%s.idle", art);
    bool mirror;
    const int idx = fart_frame(fart_anim(&g_art, n), dir, t, &mirror);
    if (idx < 0) return;
    const fart_clip_t clip = { 0, 0, u->w, u->h };
    const fart_style_t st = { .shade = shade };
    fart_draw_style(u->px, u->w, &clip, &g_art.img[idx], x, y, mirror, &st);
}

// A cheer: a hop every 600 ms, sparks round the head at the top of it.
static int cheer_hop(uint32_t t)
{
    const int p = (int)(t % 600);
    return p < 300 ? (p * (300 - p)) / 900 : 0;                  // up to 25 px
}
static void sparks(const ui_t *u, int x, int y, uint32_t t)
{
    static const int8_t at[6][2] = { { -12, -6 }, { 11, -9 }, { -7, -16 }, { 8, -18 }, { 0, -22 }, { -14, -14 } };
    for (int k = 0; k < 6; k++) {
        if (((t / 100) + k) % 3) continue;
        const int sx = x + at[k][0], sy = y + at[k][1];
        const uint16_t c = k & 1 ? 0xffe0 : 0xffff;
        for (int d = -1; d <= 1; d++) {
            if (sx + d >= 0 && sx + d < u->w && sy >= 0 && sy < u->h) u->px[sy * u->w + sx + d] = c;
            if (sx >= 0 && sx < u->w && sy + d >= 0 && sy + d < u->h) u->px[(sy + d) * u->w + sx] = c;
        }
    }
}

// What a hero shows after `gained` of its XP: its level and XP within that level.
static void xp_at(int level, int xp, int gained, int *out_level, int *out_xp)
{
    xp += gained;
    while (level < LEVEL_MAX && xp >= xp_next(level)) {
        xp -= xp_next(level);
        level++;
    }
    *out_level = level;
    *out_xp = xp;
}

void summary_draw(const ui_t *u, uint32_t t)
{
    char l[48];
    // The box grows from the middle.
    const float open = progress(t, 0, T_OPEN);
    const int fw = u->w - 16, fh = u->h - 12;
    const int bw = 40 + (int)((fw - 40) * open), bh = 20 + (int)((fh - 20) * open);
    ui_box(u, (u->w - bw) / 2, (u->h - bh) / 2, bw, bh);
    if (open < 1.0f) return;
    const int x0 = 8, y0 = 6;

    const bool won = g_result.won;
    snprintf(l, sizeof(l), "%s", won ? "Vitória!" : "Derrota");
    fh1_px_text(u->px, u->w, u->h, (u->w - len8(l) * 2) / 2 + 1, y0 + 7, l, 0x000000, 2);
    fh1_px_text(u->px, u->w, u->h, (u->w - len8(l) * 2) / 2, y0 + 6, l, won ? 0xf8d870 : 0xff8070, 2);
    const char *where = g_battle.name;
    ui_text(u, (u->w - len8(where)) / 2, y0 + 24, where, 0x9fb0d0);
    if (g_result.cleared) {                                      // the fase done: what it opened
        const int el = g_realms[g_battle.realm].element;
        if (g_battle.tier < TIERS - 1) snprintf(l, sizeof(l), "Fase concluída! Loja: nível %d", g_battle.tier + 1);
        else snprintf(l, sizeof(l), "%s vencido! Loja: nível %d", g_realms[g_battle.realm].elemental, g_battle.tier + 1);
        ui_text(u, (u->w - len8(l)) / 2, y0 + 35, l, (t / 300) % 2 ? g_elements[el].rgb : 0xf8d870);
    }

    // The enemies, one by one.
    const int fy = y0 + 76, n = g_result.nkills;
    int coins_so_far = 0;
    if (!n) ui_text(u, (u->w - len8("nenhum inimigo caiu")) / 2, fy - 20, "nenhum inimigo caiu", 0x8a96b0);
    for (int i = 0; i < n; i++) {
        const uint32_t at = t_foes() + i * T_FOE;
        if (t < at) break;
        foe_t e;
        foe_make(g_result.kills[i].type, g_battle.realm, g_result.kills[i].level, false, &e);
        const int slot = n > 1 ? (fw - 40) / n : 0;
        const int cx = n > 1 ? x0 + 20 + slot * i + slot / 2 : u->w / 2;
        const float pop = progress(t, at, 160);
        sprite(u, e.art, e.shade, cx, fy - (int)(6 * (1.0f - pop)), FART_SW, t);
        snprintf(l, sizeof(l), "+%d", g_result.kills[i].coins);
        coin_icon(u, cx - 14, fy + 4);
        ui_text(u, cx, fy + 5, l, 0xf8d870);
        snprintf(l, sizeof(l), "+%d XP", g_result.kills[i].xp);
        ui_text(u, cx - len8(l) / 2, fy + 16, l, 0x9fd8ff);
        coins_so_far += g_result.kills[i].coins;
    }

    // The coins counting up.
    const int cy = y0 + 108;
    if (t >= t_coins()) {
        const float p = progress(t, t_coins(), T_COINS);
        const int shown = (int)(g_result.coins * p + 0.5f);
        coin_icon(u, x0 + 14, cy);
        snprintf(l, sizeof(l), "%d moedas%s", shown, won ? "" : " (metade)");
        ui_text(u, x0 + 32, cy + 1, l, 0xf8d870);
        snprintf(l, sizeof(l), "Bolsa: %u", (unsigned)(g_save.coins - g_result.coins + shown));
        ui_text(u, u->w - 16 - len8(l), cy + 1, l, 0xc8c8d0);
    }

    // The heroes: XP bars filling, levels going up.
    for (int s = 0; s < g_result.nparty; s++) {
        const uint32_t at = t_heroes() + s * T_HERO;
        if (t < at) break;
        const hero_t *h = &g_save.heroes[g_result.hero[s]];
        const int ry = y0 + 124 + s * 34;
        const float p = progress(t, at + 150, T_HERO - 300);
        int lv, xp;
        xp_at(g_result.level_before[s], g_result.xp_before[s], (int)(g_result.xp[s] * p), &lv, &xp);
        // Facing the viewer; once its level has gone up, it cheers.
        const bool up = lv > g_result.level_before[s];
        const int hop = up ? cheer_hop(t) : 0;
        hero_draw(u, h, x0 + 24, ry + 32 - hop, up ? FART_SW : FART_SE, false, t);
        if (up && hop > 14) sparks(u, x0 + 24, ry + 2 - hop, t);
        ui_text(u, x0 + 46, ry + 4, h->name, 0x9fd8ff);
        snprintf(l, sizeof(l), "Nv %d", lv);
        ui_text(u, x0 + 46 + 80, ry + 4, l, lv > g_result.level_before[s] ? 0x80ff90 : 0xe0e0e8);
        snprintf(l, sizeof(l), "+%d XP", (int)(g_result.xp[s] * p + 0.5f));
        ui_text(u, u->w - 16 - len8(l), ry + 4, l, 0x9fd8ff);
        ui_bar(u, x0 + 46, ry + 15, u->w - 16 - (x0 + 46), xp, xp_next(lv), 0xe0b040);
        const int ups = lv - g_result.level_before[s];
        if (ups > 0) {
            const bool blink = (t / 200) % 2;
            ui_text(u, x0 + 46, ry + 22, "Subiu de nível!", blink ? 0x80ff90 : 0xf8d870);
            const class_t *c = &g_classes[h->cls];
            // A new gear level (every LEVEL_STEP levels) says so; else what the levels gave.
            if (lv / LEVEL_STEP > g_result.level_before[s] / LEVEL_STEP && lv / LEVEL_STEP < GEAR_LEVELS)
                snprintf(l, sizeof(l), "Equip. nível %d!", lv / LEVEL_STEP);
            else if (c->mp_level) snprintf(l, sizeof(l), "PV+%d PM+%d Dano+%d", c->hp_level * ups, c->mp_level * ups, ups);
            else snprintf(l, sizeof(l), "PV+%d Dano+%d", c->hp_level * ups, ups);
            ui_text(u, u->w - 16 - len8(l), ry + 22, l, 0xc8c8d0);
        }
    }

    // The key's hint in the top corner (the heroes' rows reach the bottom).
    if (t >= summary_length()) {
        if ((t / 400) % 2) ui_hints(u, u->w - 16 - ui_hints_width("{A} equipe"), y0 + 8, "{A} equipe", 0xf0f0f0);
    } else ui_hints(u, u->w - 16 - ui_hints_width("{A} pular"), y0 + 8, "{A} pular", 0x8a96b0);
}
