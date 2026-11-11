// The screens around battles: title (and the story), party (cards, cascading menus, a name keyboard), the
// realms, a realm's path of fases, the loading screen, the shop. All text in Portuguese.
#include "game.h"
#include <stdio.h>
#include <string.h>

static int len8(const char *s)
{
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if ((*p & 0xC0) != 0x80) n++;
    return n * 8;
}

static void text_centre(const ui_t *u, int y, const char *s, uint32_t rgb, int scale)
{
    const int x = (u->w - len8(s) * scale) / 2;
    fh1_px_text(u->px, u->w, u->h, x + scale, y + scale, s, 0x000000, scale);
    fh1_px_text(u->px, u->w, u->h, x, y, s, rgb, scale);
}

// Icons from the art pack: the bag icons (16 x 16), the big swords (32 x 32), the coin pile.
static void draw_image(const ui_t *u, const char *name, int x, int y, int scale)
{
    const int i = fart_image(&g_art, name);
    if (i < 0) return;
    const fart_clip_t clip = { 0, 0, u->w, u->h };
    if (scale <= 1) {
        fart_draw(u->px, u->w, &clip, &g_art.img[i], x, y, false);
        return;
    }
    // Twice the size: draw into a small buffer, then double each pixel.
    static uint16_t tmp[32 * 32];
    const fart_img_t *im = &g_art.img[i];
    if (im->ox + im->w > 32 || im->oy + im->h > 32) return;
    for (int k = 0; k < 32 * 32; k++) tmp[k] = 0xF81F;           // magenta: empty
    const fart_clip_t tc = { 0, 0, 32, 32 };
    fart_draw(tmp, 32, &tc, im, 0, 0, false);
    for (int yy = 0; yy < 32; yy++)
        for (int xx = 0; xx < 32; xx++) {
            const uint16_t v = tmp[yy * 32 + xx];
            if (v == 0xF81F) continue;
            for (int j = 0; j < scale; j++)
                for (int k = 0; k < scale; k++) {
                    const int px = x + xx * scale + k, py = y + yy * scale + j;
                    if (px >= 0 && py >= 0 && px < u->w && py < u->h) u->px[py * u->w + px] = v;
                }
        }
}

void goods_icon(const ui_t *u, int g, int x, int y, bool big)
{
    char n[16];
    if (big && g_goods[g].big >= 0) {
        snprintf(n, sizeof(n), "sword.%d", g_goods[g].big);
        draw_image(u, n, x, y, 1);
        return;
    }
    snprintf(n, sizeof(n), "icon.%d", g_goods[g].icon);
    draw_image(u, n, x, y, big ? 2 : 1);
}

void coin_icon(const ui_t *u, int x, int y) { draw_image(u, "icon.49", x - 2, y - 3, 1); }

// ── Title ──

static ui_menu_t s_title;
static int s_story = -1;                   // the story's page shown, or -1

static const char *const s_pages[3][4] = {
    { "Há muito tempo, quatro Elementais", "guardavam a Ilha: Ignis, do Fogo;", "Maris, da Água; Gaia, da Terra;",
      "e Zéfiro, do Ar." },
    { "Uma sombra caiu sobre os reinos.", "Os Elementais enlouqueceram, e as", "criaturas tomaram as cores dos",
      "seus senhores." },
    { "Reúna sua equipe, atravesse as", "fases de cada reino e enfrente os", "Elementais para devolver o",
      "equilíbrio à Ilha." },
};

void title_enter(void)
{
    memset(&s_title, 0, sizeof(s_title));
    s_title.items[0] = "Começar";
    s_title.items[1] = "História";
    s_title.items[2] = "Sair";
    s_title.n = 3;
    s_story = -1;
}

screen_t title_frame(const ui_t *u, uint32_t dt)
{
    draw_back(u);
    text_centre(u, u->h / 4 - 10, "Táticas da Ilha", 0xf8d870, 2);
    text_centre(u, u->h / 4 + 12, "Os Quatro Reinos", 0x9fb0d0, 1);
    // The roster in their gear, walking in place and turning, each a little after the one before.
    for (int i = 0; i < ROSTER; i++) {
        int dir;
        bool walking;
        hero_pose(g_t + (uint32_t)i * 550, &dir, &walking);
        hero_draw(u, &g_save.heroes[i], u->w / 2 - 54 + i * 36, u->h / 2 + 22, dir, walking, g_t);
    }
    if (s_story >= 0) {                                    // the story: a page at a time
        const int bw = 296, bh = 92, x = (u->w - bw) / 2, y = (u->h - bh) / 2 + 30;
        ui_box(u, x, y, bw, bh);
        for (int k = 0; k < 4; k++) ui_text(u, x + 12, y + 10 + k * 13, s_pages[s_story][k], 0xe8e8f0);
        char l[24];
        snprintf(l, sizeof(l), "%d/3 {A}", s_story + 1);
        ui_hints(u, x + bw - 12 - ui_hints_width(l), y + bh - 14, l, 0x8a96b0);
        if (key(FH1_KEY_A)) {
            snd_play(S_CONFIRM, 180);
            if (++s_story >= 3) s_story = -1;
        } else if (key(FH1_KEY_B)) {
            snd_play(S_DECLINE, 180);
            s_story = -1;
        }
        return SCR_TITLE;
    }
    int bw, bh;
    ui_menu_size(&s_title, &bw, &bh);
    ui_menu_draw(u, &s_title, (u->w - bw) / 2, u->h - bh - 10, g_t);
    const int r = ui_menu_key(&s_title, menu_key());
    if (r == 0) {
        game_load();
        return SCR_PARTY;
    }
    if (r == 1) s_story = 0;
    return r == 2 ? SCR_QUIT : SCR_TITLE;
}

// ── The party: one card per hero, Pronto under them ──

typedef enum { PS_CARDS, PS_MENU, PS_GEAR, PS_ATTRS, PS_NAME } pstate_t;
static pstate_t s_ps;
static int s_card;                         // 0..ROSTER-1, ROSTER = Pronto
static ui_menu_t s_pmenu, s_gear;
static int s_gear_kind, s_gear_ids[UI_ITEMS];
static char s_msg[48];
static uint32_t s_msg_until;

static void message(const char *s) { strlcpy(s_msg, s, sizeof(s_msg)); s_msg_until = g_t + 1600; }

// Whose progress is shown now, after player 1's user changed (on the party screen, 2.5 s).
void party_notice(const char *who)
{
    char n[24];
    strlcpy(n, who[0] ? who : "sem usuário", sizeof(n));
    if (n[0] >= 'a' && n[0] <= 'z') n[0] -= 32;
    snprintf(s_msg, sizeof(s_msg), "Progresso de %s", n);
    s_msg_until = g_t + 2500;
}

// The cards, then two buttons under them: Loja (ROSTER + 1) and Pronto (ROSTER). Back from the shop, Loja stays chosen.
#define CARD_SHOP (ROSTER + 1)
void party_enter(void)
{
    s_ps = PS_CARDS;
    if (s_card != CARD_SHOP) s_card = 0;
}

static void open_pmenu(void)
{
    const hero_t *h = &g_save.heroes[s_card];
    memset(&s_pmenu, 0, sizeof(s_pmenu));
    s_pmenu.title = h->name;
    s_pmenu.items[0] = h->in_party ? "Tirar da equipe" : "Pôr na equipe";
    s_pmenu.items[1] = "Nome";
    s_pmenu.items[2] = "Arma";
    s_pmenu.notes[2] = g_goods[h->weapon].name;
    s_pmenu.items[3] = "Armadura";
    s_pmenu.notes[3] = g_goods[h->armour].name;
    s_pmenu.items[4] = "Atributos";
    s_pmenu.n = 5;
    s_ps = PS_MENU;
}

// The hero's class's gear in the bag: what it gives; greyed with the level it needs when the hero is below it.
static void open_gear(int kind)
{
    static char notes[UI_ITEMS][24];
    const hero_t *h = &g_save.heroes[s_card];
    const int worn = kind == KIND_WEAPON ? h->weapon : h->armour;
    memset(&s_gear, 0, sizeof(s_gear));
    s_gear.title = kind == KIND_WEAPON ? "Arma" : "Armadura";
    s_gear_kind = kind;
    int n = 0;
    for (int k = 0; k < g_ngoods && n < UI_ITEMS; k++) {
        int need;
        if (g_goods[k].kind != kind || (k != worn && !g_save.bag[k])) continue;
        const bool ok = hero_can_use(h, k, &need);
        if (!ok && h->level >= need) continue;                // another class's
        s_gear.items[n] = g_goods[k].name;
        if (k == worn) snprintf(notes[n], sizeof(notes[n]), "usando");
        else if (!ok) snprintf(notes[n], sizeof(notes[n]), "Nv %d", need);
        else if (kind == KIND_WEAPON) {
            int lo, hi;
            weapon_roll(k, &lo, &hi);
            snprintf(notes[n], sizeof(notes[n]), "+%d~%d", lo, hi);
        } else snprintf(notes[n], sizeof(notes[n]), "Df %d", g_goods[k].power);
        s_gear.notes[n] = notes[n];
        if (!ok) s_gear.off |= 1u << n;
        if (k == worn) s_gear.sel = n;
        s_gear_ids[n++] = k;
    }
    s_gear.n = n;
    s_ps = PS_GEAR;
}

// The name keyboard: letters in a grid, then Maiúsc., Espaço, Apagar, OK.
static char s_name[24];
static int s_kx, s_ky;
static bool s_upper = true;
static const char *const s_rows_up[4] = { "ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZÁÉÍÓ", "ÚÃÕÇÂÊÔÀ-'" };
static const char *const s_rows_lo[4] = { "abcdefghij", "klmnopqrst", "uvwxyzáéíó", "úãõçâêôà-'" };
#define KB_COLS 10
#define KB_ROWS 5                          // the 5th: the four buttons

static const char *kb_letter(int x, int y, char out[4])
{
    const char *row = (s_upper ? s_rows_up : s_rows_lo)[y];
    const unsigned char *p = (const unsigned char *)row;
    for (int i = 0; *p; i++) {
        const int n = (*p & 0xE0) == 0xC0 ? 2 : 1;
        if (i == x) {
            memcpy(out, p, n);
            out[n] = 0;
            return out;
        }
        p += n;
    }
    out[0] = 0;
    return out;
}

static void name_backspace(void)
{
    int n = strlen(s_name);
    while (n > 0 && ((unsigned char)s_name[n - 1] & 0xC0) == 0x80) n--;   // a UTF-8 letter's tail
    if (n > 0) n--;
    s_name[n] = 0;
}

static void name_input(void)
{
    static const int8_t dx[4] = { 1, 0, -1, 0 }, dy[4] = { 0, 1, 0, -1 };
    if (g_keys.dir >= 0) {
        // On the keyboard the D-pad moves as on screen: Up is up (FART_NE), Right is right (FART_SE).
        const int d = g_keys.dir == FART_NE ? 3 : g_keys.dir == FART_SE ? 0 : g_keys.dir == FART_SW ? 1 : 2;
        const int cols = s_ky == KB_ROWS - 1 ? 4 : KB_COLS;
        s_kx = (s_kx + dx[d] + cols) % cols;
        s_ky = (s_ky + dy[d] + KB_ROWS) % KB_ROWS;
        if (s_ky == KB_ROWS - 1 && s_kx > 3) s_kx = 3;
    }
    if (key(FH1_KEY_B)) name_backspace();
    if (key(FH1_KEY_X)) s_upper = !s_upper;
    if (key(FH1_KEY_START)) s_kx = 3, s_ky = KB_ROWS - 1;
    if (!key(FH1_KEY_A)) return;
    if (s_ky < KB_ROWS - 1) {
        char l[4];
        kb_letter(s_kx, s_ky, l);
        if (len8(s_name) / 8 < 8 && strlen(s_name) + strlen(l) < sizeof(s_name)) strcat(s_name, l);
        if (s_upper && len8(s_name) == 8) s_upper = false;          // after the first letter
    } else if (s_kx == 0) s_upper = !s_upper;
    else if (s_kx == 1) {
        if (len8(s_name) / 8 < 8 && s_name[0]) strcat(s_name, " ");
    } else if (s_kx == 2) name_backspace();
    else {
        if (s_name[0]) strlcpy(g_save.heroes[s_card].name, s_name, sizeof(g_save.heroes[s_card].name));
        game_store();
        open_pmenu();
    }
}

static void name_draw(const ui_t *u)
{
    const int cw = 18, bw = KB_COLS * cw + 16, bh = 6 * 16 + 30;
    const int x = (u->w - bw) / 2, y = (u->h - bh) / 2;
    ui_box(u, x, y, bw, bh);
    ui_text(u, x + 8, y + 6, "Nome:", 0xf8d870);
    char shown[32];
    snprintf(shown, sizeof(shown), "%s%s", s_name, (g_t / 300) % 2 ? "_" : " ");
    ui_text(u, x + 56, y + 6, shown, 0xffffff);
    for (int r = 0; r < KB_ROWS - 1; r++)
        for (int c = 0; c < KB_COLS; c++) {
            char l[4];
            kb_letter(c, r, l);
            const bool on = r == s_ky && c == s_kx;
            const int lx = x + 8 + c * cw, ly = y + 24 + r * 16;
            if (on) ui_box(u, lx - 3, ly - 4, 15, 15);
            ui_text(u, lx, ly, l, on ? 0xf8d870 : 0xe0e0e8);
        }
    static const char *const btn[4] = { "Aa", "Espaço", "Apagar", "OK" };
    int bx = x + 8;
    for (int i = 0; i < 4; i++) {
        const bool on = s_ky == KB_ROWS - 1 && s_kx == i;
        const int w8 = len8(btn[i]) + 8;
        if (on) ui_box(u, bx - 4, y + 24 + 4 * 16 + 2, w8, 15);
        ui_text(u, bx, y + 24 + 4 * 16 + 6, btn[i], on ? 0xf8d870 : 0xe0e0e8);
        bx += w8 + 6;
    }
    ui_hints(u, x + 8, y + bh - 12, "{B} apaga {X} Aa {S} OK", 0x8a96b0);
}

static void attrs_draw(const ui_t *u, const hero_t *h)
{
    char l[48];
    const int bw = 196, bh = 130, x = (u->w - bw) / 2, y = (u->h - bh) / 2;
    const class_t *c = &g_classes[h->cls];
    ui_box(u, x, y, bw, bh);
    snprintf(l, sizeof(l), "%s, %s", h->name, c->name);
    ui_text(u, x + 8, y + 6, l, 0xf8d870);
    int lo, hi;
    weapon_roll(h->weapon, &lo, &hi);
    char dmg[16];
    snprintf(dmg, sizeof(dmg), "%d~%d", h->level + lo, h->level + hi);
    char lv[8];
    snprintf(lv, sizeof(lv), "%d", h->level);
    const struct { const char *k; int v; const char *s; } rows[] = {
        { "Nível", 0, lv }, { "PV", hero_hp_max(h), NULL }, { "PM", hero_mp_max(h), NULL }, { "Dano", 0, dmg },
        { "Defesa", hero_defence(h), NULL }, { "Movimento", c->move, NULL }, { "Salto", c->jump, NULL },
        { "Alcance", g_goods[h->weapon].extra, NULL },
    };
    for (int i = 0; i < 8; i++) {
        ui_text(u, x + 8 + (i / 4) * 96, y + 22 + (i % 4) * 11, rows[i].k, 0xc8c8d0);
        if (!rows[i].s) snprintf(l, sizeof(l), "%d", rows[i].v);
        ui_text(u, x + 8 + (i / 4) * 96 + (i / 4 ? 80 : 44), y + 22 + (i % 4) * 11, rows[i].s ? rows[i].s : l, 0xffffff);
    }
    const int el = g_goods[h->weapon].element, ae = g_goods[h->armour].element;
    if (el) {
        snprintf(l, sizeof(l), "Arma: x2 vs %s", g_elements[el_beats(el)].name);
        ui_text(u, x + 8, y + 66, l, g_elements[el].rgb);
    }
    if (ae) {
        snprintf(l, sizeof(l), "Armadura: 1/2 %s", g_elements[ae].name);
        ui_text(u, x + 8, y + 77, l, g_elements[ae].rgb);
    }
    if (h->level >= LEVEL_MAX) snprintf(l, sizeof(l), "Nível máximo");
    else snprintf(l, sizeof(l), "XP %d / %d", h->xp, xp_next(h->level));
    ui_text(u, x + 8, y + 90, l, 0xc8c8d0);
    ui_bar(u, x + 8, y + 101, bw - 16, h->level >= LEVEL_MAX ? 1 : h->xp, h->level >= LEVEL_MAX ? 1 : xp_next(h->level), 0xe0b040);
    ui_text(u, x + 8, y + 114, "B volta", 0x8a96b0);
}

static void card(const ui_t *u, int i, int x, int y, int cw, int ch, bool on)
{
    const hero_t *h = &g_save.heroes[i];
    char l[32];
    ui_box(u, x, y, cw, ch);
    if (on) {                                          // a bright frame round the chosen card
        const uint16_t c = (g_t / 250) % 2 ? 0xffe0 : 0xfea0;
        for (int k = 0; k < cw; k++) {
            if (y >= 0) u->px[y * u->w + x + k] = c;
            u->px[(y + ch - 1) * u->w + x + k] = c;
        }
        for (int k = 0; k < ch; k++) {
            u->px[(y + k) * u->w + x] = c;
            u->px[(y + k) * u->w + x + cw - 1] = c;
        }
    }
    // The chosen card's hero walks and turns; the others stand facing the viewer. All in their gear.
    int dir = FART_SE;
    bool walking = false;
    if (on) hero_pose(g_t, &dir, &walking);
    hero_draw(u, h, x + cw / 2, y + 52, dir, walking, on ? g_t : 0);
    ui_text(u, x + 4, y + 56, h->name, h->in_party ? 0x9fd8ff : 0xc0c0c8);
    ui_text(u, x + 4, y + 66, g_classes[h->cls].name, 0x8a96b0);
    snprintf(l, sizeof(l), "Nv %d", h->level);
    ui_text(u, x + 4, y + 78, l, 0xe0e0e8);
    ui_bar(u, x + 4, y + 89, cw - 8, h->xp, xp_next(h->level), 0xe0b040);
    snprintf(l, sizeof(l), "PV %d", hero_hp_max(h));
    ui_text(u, x + 4, y + 96, l, 0xe0e0e8);
    snprintf(l, sizeof(l), "PM %d", hero_mp_max(h));
    ui_text(u, x + 4, y + 106, l, 0xe0e0e8);
    if (h->in_party) ui_text(u, x + 4, y + 4, "equipe", 0x60f080);
}

screen_t party_frame(const ui_t *u, uint32_t dt)
{
    draw_back(u);
    char l[48];
    snprintf(l, sizeof(l), "Equipe %d/%d", party_count(), PARTY_MAX);
    ui_text(u, 6, 6, l, 0xf8d870);
    snprintf(l, sizeof(l), "%u moedas", (unsigned)g_save.coins);
    ui_text(u, u->w - 6 - len8(l), 6, l, 0xf8d870);
    const int cw = (u->w - 10) / ROSTER - 4, ch = 118;
    for (int i = 0; i < ROSTER; i++) card(u, i, 6 + i * (cw + 4), 20, cw, ch, s_card == i && s_ps == PS_CARDS);
    // Loja and Pronto under the cards.
    const char *ready = party_count() ? "Pronto" : "Escolha a equipe";
    const struct { const char *s; int card; } btn[2] = { { "Loja", CARD_SHOP }, { ready, ROSTER } };
    const int ry = 20 + ch + 10, gap = 12;
    int bx = (u->w - (len8(btn[0].s) + 24 + gap + len8(btn[1].s) + 24)) / 2;
    for (int k = 0; k < 2; k++) {
        const bool on = s_card == btn[k].card && s_ps == PS_CARDS;
        const int bw = len8(btn[k].s) + 24;
        ui_box(u, bx, ry, bw, 20);
        if (on) {
            ui_text(u, bx + 3 + (int)((g_t / 200) % 2), ry + 6, ">", 0xffffff);
            ui_text_wave(u, bx + 12, ry + 6, btn[k].s, 0xf8d870, g_t);
        } else ui_text(u, bx + 12, ry + 6, btn[k].s, 0xc0c0c8);
        bx += bw + gap;
    }
    ui_hints(u, 6, u->h - 12, s_card < ROSTER ? "{A} editar  {B} título" : "{A} ok  {LR} trocar  {B} título", 0x8a96b0);

    switch (s_ps) {
    case PS_CARDS:
        if (g_keys.dir >= 0) snd_play(S_HOVER, 160);
        if (g_keys.dir == FART_SE && s_card < ROSTER) s_card = (s_card + 1) % ROSTER;
        if (g_keys.dir == FART_NW && s_card < ROSTER) s_card = (s_card + ROSTER - 1) % ROSTER;
        if ((g_keys.dir == FART_SE || g_keys.dir == FART_NW) && s_card >= ROSTER) s_card = s_card == ROSTER ? CARD_SHOP : ROSTER;
        if (g_keys.dir == FART_SW && s_card < ROSTER) s_card = ROSTER;
        if (g_keys.dir == FART_NE && s_card >= ROSTER) s_card = 0;
        if (key(FH1_KEY_A)) {
            if (s_card < ROSTER) open_pmenu();
            else if (s_card == CARD_SHOP) {
                snd_play(S_CONFIRM, 200);
                game_store();
                return SCR_SHOP;
            } else if (party_count()) {
                game_store();
                return SCR_REALM;
            }
        }
        if (key(FH1_KEY_B)) {
            snd_play(S_DECLINE, 200);
            return SCR_TITLE;
        }
        break;
    case PS_MENU: {
        const int r = ui_menu_key(&s_pmenu, menu_key());
        hero_t *h = &g_save.heroes[s_card];
        if (r == UI_BACK) s_ps = PS_CARDS;
        else if (r == 0) {
            if (!h->in_party && party_count() >= PARTY_MAX) message("A equipe já tem 3.");
            else h->in_party = !h->in_party;
            open_pmenu();
        } else if (r == 1) {
            strlcpy(s_name, h->name, sizeof(s_name));
            s_kx = s_ky = 0;
            s_upper = !s_name[0];
            s_ps = PS_NAME;
        } else if (r == 2 || r == 3) open_gear(r == 2 ? KIND_WEAPON : KIND_ARMOUR);
        else if (r == 4) s_ps = PS_ATTRS;
        break;
    }
    case PS_GEAR: {
        const int r = ui_menu_key(&s_gear, menu_key());
        if (r == UI_BACK) open_pmenu();
        else if (r >= 0) {
            hero_t *h = &g_save.heroes[s_card];
            uint8_t *slot = s_gear_kind == KIND_WEAPON ? &h->weapon : &h->armour;
            const int g = s_gear_ids[r];
            if (s_gear.off & (1u << r)) break;                // (greyed: not chosen)
            snd_play(S_EQUIP, 220);
            if (*slot != g) {
                g_save.bag[*slot]++;
                g_save.bag[g]--;
                *slot = g;
            }
            open_pmenu();
            s_pmenu.sel = s_gear_kind == KIND_WEAPON ? 2 : 3;
        }
        break;
    }
    case PS_ATTRS:
        if (key(FH1_KEY_B) || key(FH1_KEY_A)) s_ps = PS_MENU;
        break;
    case PS_NAME:
        name_input();
        break;
    }
    // The cascade: the hero's menu by its card, the gear list beside it.
    if (s_ps == PS_MENU || s_ps == PS_GEAR) {
        int bw, bh;
        ui_menu_size(&s_pmenu, &bw, &bh);
        int mx = 6 + s_card * (cw + 4) + cw / 2;
        if (mx + bw > u->w - 4) mx = u->w - 4 - bw;
        ui_menu_draw(u, &s_pmenu, mx, 46, g_t);
        if (s_ps == PS_GEAR) {
            int gw, gh;
            ui_menu_size(&s_gear, &gw, &gh);
            int gx = mx + 20;
            if (gx + gw > u->w - 4) gx = u->w - 4 - gw;
            ui_menu_draw(u, &s_gear, gx, 46 + 18 + (s_gear_kind == KIND_WEAPON ? 2 : 3) * 11, g_t);
        }
    } else if (s_ps == PS_ATTRS) attrs_draw(u, &g_save.heroes[s_card]);
    else if (s_ps == PS_NAME) name_draw(u);
    if (s_msg[0] && g_t < s_msg_until) {
        ui_box(u, (u->w - len8(s_msg) - 16) / 2, u->h / 2 - 10, len8(s_msg) + 16, 20);
        ui_text(u, (u->w - len8(s_msg)) / 2, u->h / 2 - 4, s_msg, 0xffd080);
    }
    return SCR_PARTY;
}

// ── The realms: one card each (its slime, its fases' progress), the story of the one chosen ──

static int fase_text(int realm, int tier, char *out, int n)
{
    if (!tier_open(realm, tier)) return snprintf(out, n, "F%d  --", tier + 1);
    return snprintf(out, n, "F%d %d/%d", tier + 1, g_save.won[realm][tier], g_tier_len[tier]);
}

void realm_enter(void) {}

screen_t realm_frame(const ui_t *u, uint32_t dt)
{
    draw_back(u);
    char l[48];
    ui_text(u, 8, 6, "Reinos", 0xf8d870);
    snprintf(l, sizeof(l), "%u", (unsigned)g_save.coins);
    ui_text(u, u->w - 8 - len8(l), 6, l, 0xf8d870);
    coin_icon(u, u->w - 24 - len8(l), 6);
    const int cw = (u->w - 12) / REALMS - 4, ch = 124, sel = g_save.realm % REALMS;
    const fart_clip_t clip = { 0, 0, u->w, u->h };
    for (int r = 0; r < REALMS; r++) {
        const realm_t *rm = &g_realms[r];
        const int x = 6 + r * (cw + 4), y = 20;
        const uint32_t c = g_elements[rm->element].rgb;
        ui_box(u, x, y, cw, ch);
        // Its colour along the top, its name on it.
        const uint16_t c565 = el_565(rm->element, false);
        for (int yy = y + 2; yy < y + 15; yy++)
            for (int xx = x + 2; xx < x + cw - 2; xx++) u->px[yy * u->w + xx] = r == sel ? c565 : (uint16_t)((c565 >> 1) & 0x7BEF);
        ui_text(u, x + (cw - len8(rm->name)) / 2, y + 5, rm->name, r == sel ? 0xffffff : 0xc0c0c8);
        // Its slime (twice the size), hopping when chosen.
        char n[32];
        snprintf(n, sizeof(n), "slime.%s.idle", rm->key);
        bool mirror;
        const int idx = fart_frame(fart_anim(&g_art, n), FART_SW, r == sel ? g_t : 0, &mirror);
        if (idx >= 0) {
            const fart_style_t st = { .zoom = 2, .tint = r == sel ? FART_PLAIN : FART_DIM };
            fart_draw_style(u->px, u->w, &clip, &g_art.img[idx], x + cw / 2, y + 62, mirror, &st);
        }
        for (int t = 0; t < TIERS; t++) {
            fase_text(r, t, l, sizeof(l));
            ui_text(u, x + 6, y + 70 + t * 11, l, tier_cleared(r, t) ? 0x60f080 : tier_open(r, t) ? 0xe0e0e8 : 0x707890);
        }
        const bool freed = tier_cleared(r, TIERS - 1);
        ui_text(u, x + (cw - len8(rm->elemental)) / 2, y + 106, rm->elemental, freed ? 0x60f080 : c);
        if (r == sel) {                                     // a bright frame
            const uint16_t f = (g_t / 250) % 2 ? 0xffe0 : 0xfea0;
            for (int k = 0; k < cw; k++) u->px[y * u->w + x + k] = u->px[(y + ch - 1) * u->w + x + k] = f;
            for (int k = 0; k < ch; k++) u->px[(y + k) * u->w + x] = u->px[(y + k) * u->w + x + cw - 1] = f;
        }
    }
    const realm_t *rm = &g_realms[sel];
    const int by = 150;
    ui_box(u, 6, by, u->w - 12, u->h - by - 16);
    snprintf(l, sizeof(l), "Reino %s", rm->name);
    ui_text(u, 14, by + 6, l, g_elements[rm->element].rgb);
    ui_text(u, 14, by + 19, rm->about[0], 0xe0e0e8);
    ui_text(u, 14, by + 30, rm->about[1], 0xe0e0e8);
    snprintf(l, sizeof(l), "Criaturas fracas contra %s", g_elements[el_weakness(rm->element)].name);
    ui_text(u, 14, by + 46, l, 0x9fb0d0);
    ui_hints(u, 6, u->h - 12, "{A} entrar  {LR} reino  {B} equipe", 0x8a96b0);

    if (g_keys.dir == FART_SE) g_save.realm = (uint8_t)((sel + 1) % REALMS), snd_play(S_HOVER, 160);
    if (g_keys.dir == FART_NW) g_save.realm = (uint8_t)((sel + REALMS - 1) % REALMS), snd_play(S_HOVER, 160);
    if (key(FH1_KEY_B)) {
        snd_play(S_DECLINE, 200);
        return SCR_PARTY;
    }
    if (key(FH1_KEY_A)) {
        snd_play(S_CONFIRM, 200);
        return SCR_PICK;
    }
    return SCR_REALM;
}

// ── A realm's path: its fases (Up / Down), each a way of battles (Left / Right); won ones can be fought again ──

static int s_node;

static int open_upto(int realm, int tier)                  // the last battle that can be chosen in a fase
{
    const int n = g_tier_len[tier], w = g_save.won[realm][tier];
    return w < n ? w : n - 1;
}

void pick_enter(void)
{
    const int r = g_save.realm % REALMS;
    int t = 0;
    for (int k = 0; k < TIERS; k++)                        // the furthest fase open
        if (tier_open(r, k)) t = k;
    g_save.tier = (uint8_t)t;
    s_node = open_upto(r, t);
}

static void node_pos(const ui_t *u, int n, int i, int *x, int *y)
{
    *x = 24 + (u->w - 70) * i / (n > 1 ? n - 1 : 1);       // the boss (last, twice the size) clear of the edge
    *y = 74 + (i % 2 ? -12 : 12);
}

screen_t pick_frame(const ui_t *u, uint32_t dt)
{
    draw_back(u);
    const int r = g_save.realm % REALMS, t = g_save.tier % TIERS;
    const realm_t *rm = &g_realms[r];
    const int n = g_tier_len[t], won = g_save.won[r][t];
    char l[48];
    snprintf(l, sizeof(l), "%s: %s", rm->name, rm->tiers[t]);
    ui_text(u, 8, 6, l, g_elements[rm->element].rgb);
    snprintf(l, sizeof(l), "%u", (unsigned)g_save.coins);
    ui_text(u, u->w - 8 - len8(l), 6, l, 0xf8d870);
    coin_icon(u, u->w - 24 - len8(l), 6);
    // The fases as tabs (a star once cleared).
    for (int k = 0; k < TIERS; k++) {
        const int x = 8 + k * 72;
        const bool open = tier_open(r, k);
        ui_box(u, x, 18, 68, 15);
        snprintf(l, sizeof(l), open ? "Fase %d" : "Fase ?", k + 1);
        ui_text(u, x + 6, 22, l, k == t ? 0xf8d870 : open ? 0xc0c0c8 : 0x606880);
        if (tier_cleared(r, k)) ui_text(u, x + 56, 22, "*", 0x60f080);
    }
    // The path: dots between the battles, brighter where it has been walked.
    for (int i = 0; i + 1 < n; i++) {
        int x0, y0, x1, y1;
        node_pos(u, n, i, &x0, &y0);
        node_pos(u, n, i + 1, &x1, &y1);
        for (int k = 1; k < 10; k++) {
            const int x = x0 + (x1 - x0) * k / 10, y = y0 + (y1 - y0) * k / 10;
            const uint16_t c = i < won ? 0xfea0 : 0x52aa;
            for (int j = 0; j < 4; j++) {
                const int px = x + (j & 1), py = y + (j >> 1);
                if (px >= 0 && py >= 0 && px < u->w && py < u->h) u->px[py * u->w + px] = c;
            }
        }
    }
    // The battles: a block of the realm's ground (in its colour), a flag when won, a lock when not open yet.
    static const int ground[3] = { 22, 40, 61 };
    const fart_clip_t clip = { 0, 0, u->w, u->h };
    for (int i = 0; i < n; i++) {
        int x, y;
        node_pos(u, n, i, &x, &y);
        const bool beat = i < won, open = i <= won;
        char tn[16];
        snprintf(tn, sizeof(tn), "tile.%d", ground[rm->theme]);
        const int ti = fart_image(&g_art, tn);
        const int bob = i == s_node ? (int)((g_t / 160) % 4 == 3 ? 1 : 0) : 0;
        const fart_style_t st = { .tint = open ? FART_PLAIN : FART_DIM, .shade = el_shade(rm->element),
                                  .zoom = (uint8_t)(i == n - 1 ? 2 : 1) };
        if (ti >= 0) fart_draw_style(u->px, u->w, &clip, &g_art.img[ti], x, y - bob - (i == n - 1 ? 8 : 0), false, &st);
        if (beat) ui_text(u, x - 4, y - 22, "*", 0x60f080);
        else if (!open) ui_text(u, x - 8, y - 4, "??", 0x8a96b0);
        if (i == s_node) ui_text(u, x - 3, y - 26 - (i == n - 1 ? 14 : 0) - (int)((g_t / 150) % 3), "v", 0xffe060);
    }
    // About the chosen battle: its creatures by kind and level, what it may pay.
    battle_def_t b;
    battle_make(r, t, s_node, &b);
    const int bx = 8, by = 112;
    ui_box(u, bx, by, u->w - 16, u->h - by - 16);
    snprintf(l, sizeof(l), "%d. %s", s_node + 1, b.name);
    ui_text(u, bx + 8, by + 6, l, b.boss ? 0xffd060 : 0xf8d870);
    ui_text(u, bx + 8, by + 18, b.about, 0xe0e0e8);
    int coins = 0, ty = by + 32;
    ui_text(u, bx + 8, ty, "Inimigos:", 0xc8c8d0);
    bool shown[E_COUNT][2] = { { false } };                // by kind, casters apart
    for (int k = 0; k < b.n; k++) {
        const int type = b.foes[k].type, cast = b.foes[k].caster;
        foe_t f;
        foe_make(type, r, b.foes[k].level, cast, &f);
        coins += f.coins;
        if (shown[type][cast]) continue;
        shown[type][cast] = true;
        int cnt = 0;
        for (int j = 0; j < b.n; j++) cnt += b.foes[j].type == type && b.foes[j].caster == cast;
        if (f.boss || cast) snprintf(l, sizeof(l), "%s  Nv %d", f.name, b.foes[k].level);
        else snprintf(l, sizeof(l), "%d %s  Nv %d", cnt, cnt > 1 ? foe_plural(type, r) : f.name, b.foes[k].level);
        if (ty < u->h - 44) ui_text(u, bx + 88, ty, l, f.boss ? 0xffd060 : cast ? 0xd8a0ff : g_elements[rm->element].rgb);
        ty += 11;
    }
    coin_icon(u, bx + 12, u->h - 31);
    snprintf(l, sizeof(l), "até %d   %s", coins, s_node < won ? "(vencida)" : b.map == MAP_FIXED ? "(arena)" : "(terreno novo)");
    ui_text(u, bx + 28, u->h - 30, l, 0xc8c8d0);
    ui_hints(u, 6, u->h - 12, "{A} lutar {LR} batalha {UD} fase {B} reinos", 0x8a96b0);

    if (g_keys.dir == FART_SE && s_node < open_upto(r, t)) s_node++, snd_play(S_HOVER, 160);
    if (g_keys.dir == FART_NW && s_node > 0) s_node--, snd_play(S_HOVER, 160);
    if (g_keys.dir == FART_SW || g_keys.dir == FART_NE) {
        const int nt = t + (g_keys.dir == FART_SW ? 1 : -1);
        if (nt >= 0 && nt < TIERS && tier_open(r, nt)) {
            g_save.tier = (uint8_t)nt;
            s_node = open_upto(r, nt);
            snd_play(S_HOVER, 160);
        } else snd_play(S_DENIED, 160);
    }
    if (key(FH1_KEY_B)) {
        snd_play(S_DECLINE, 200);
        return SCR_REALM;
    }
    if (key(FH1_KEY_A)) {
        snd_play(S_CONFIRM, 200);
        g_battle = b;
        return SCR_LOAD;
    }
    return SCR_PICK;
}

// ── Loading: a battle's sounds, read a little each frame, with a bar ──

static uint32_t s_load_t;

void load_enter(void)
{
    s_load_t = 0;
    snd_load_begin(SND_SET_MENU | SND_SET_BATTLE);
}

screen_t load_frame(const ui_t *u, uint32_t dt)
{
    s_load_t += dt;
    const uint32_t t0 = fh1_millis();
    float p = 0;
    do p = snd_load_step(48 * 1024);                     // ~20 ms of reading a frame
    while (p < 1.0f && fh1_millis() - t0 < 20);
    draw_back(u);
    const realm_t *rm = &g_realms[g_battle.realm];
    text_centre(u, u->h / 2 - 52, rm->tiers[g_battle.tier], g_elements[rm->element].rgb, 1);
    text_centre(u, u->h / 2 - 38, g_battle.name, 0xf8d870, 2);
    text_centre(u, u->h / 2 - 10, "Carregando...", 0xc8c8d0, 1);
    const int bw = u->w - 80;
    ui_box(u, 36, u->h / 2 + 6, bw + 8, 14);
    ui_bar(u, 40, u->h / 2 + 11, bw, (int)(p * 1000), 1000, g_elements[rm->element].rgb);
    if (p >= 1.0f && s_load_t > 250) return battle_enter() ? SCR_BATTLE : SCR_PARTY;
    return SCR_LOAD;
}

// ── The shop: buy and sell; Left / Right switch ──

static bool s_selling;
static ui_menu_t s_shop;
static int s_shop_ids[UI_ITEMS];
static int s_first;                        // the list scrolls: the first line shown

static void shop_fill(void)
{
    static char notes[UI_ITEMS][24];
    memset(&s_shop, 0, sizeof(s_shop));
    int n = 0, sel = 0;
    for (int k = 0; k < g_ngoods; k++) {
        if (s_selling ? !g_save.bag[k] : !goods_in_shop(k)) continue;
        if (n >= s_first && n - s_first < UI_ITEMS) {
            const int i = n - s_first;
            s_shop.items[i] = g_goods[k].name;
            snprintf(notes[i], sizeof(notes[i]), "%d", s_selling ? g_goods[k].price / 2 : g_goods[k].price);
            s_shop.notes[i] = notes[i];
            if (!s_selling && g_goods[k].price > (int)g_save.coins) s_shop.off |= 1u << i;
            s_shop_ids[i] = k;
            s_shop.n = i + 1;
        }
        n++;
    }
    (void)sel;
}

void shop_enter(void)
{
    s_selling = false;
    s_first = 0;
    shop_fill();
}

screen_t shop_frame(const ui_t *u, uint32_t dt)
{
    draw_back(u);
    char l[48];
    ui_text(u, 8, 6, "Loja", 0xf8d870);
    snprintf(l, sizeof(l), "%u", (unsigned)g_save.coins);
    ui_text(u, u->w - 8 - len8(l), 6, l, 0xf8d870);
    coin_icon(u, u->w - 24 - len8(l), 6);
    // Tabs
    for (int t = 0; t < 2; t++) {
        const bool on = s_selling == t;
        const int x = 8 + t * 76;
        ui_box(u, x, 18, 70, 16);
        ui_text(u, x + 8, 22, t ? "Vender" : "Comprar", on ? 0xf8d870 : 0x8a96b0);
    }
    const int lx = 8, ly = 38, lw = u->w - 16 - 92;          // (the longest names, "Arco Duplo da Terra", fit)
    ui_box(u, lx, ly, lw, UI_ITEMS * 14 + 10);
    for (int i = 0; i < s_shop.n; i++) {
        const int y = ly + 6 + i * 14, g = s_shop_ids[i];
        const bool off = s_shop.off & (1u << i);
        if (i == s_shop.sel) ui_text(u, lx + 1 + (int)((g_t / 200) % 2), y + 1, ">", 0xffffff);
        goods_icon(u, g, lx + 9, y - 3, false);
        const uint32_t name_c = g_goods[g].element ? g_elements[g_goods[g].element].rgb : 0xf0f0f0;
        if (i == s_shop.sel) ui_text_wave(u, lx + 26, y + 1, s_shop.items[i], off ? 0x707890 : name_c, g_t);
        else ui_text(u, lx + 26, y + 1, s_shop.items[i], off ? 0x707890 : name_c);
        ui_text(u, lx + lw - 8 - len8(s_shop.notes[i]), y + 1, s_shop.notes[i], off ? 0x707890 : 0xf8d870);
    }
    if (!s_shop.n) ui_text(u, lx + 14, ly + 8, "Nada para vender", 0x8a96b0);
    // What the chosen thing is
    if (s_shop.n) {                                    // the chosen thing: who uses it, from which level, what it does
        const int gi = s_shop_ids[s_shop.sel];
        const goods_t *g = &g_goods[gi];
        const int dx = u->w - 8 - 88, dw = 88;
        int y = ly + 6;
        ui_box(u, dx, ly, dw, 150);
        static const char *const kinds[3] = { "Arma", "Armadura", "Item" };
        ui_text(u, dx + 6, y, kinds[g->kind], 0x9fd8ff);
        y += 11;
        if (g->kind != KIND_ITEM) {                    // the classes it is for
            for (int c = 0; c < CLS_COUNT; c++)
                if ((g->kind == KIND_WEAPON ? g_classes[c].weapons : g_classes[c].armours) == g->family) {
                    ui_text(u, dx + 6, y, g_classes[c].name, 0xc8c8d0);
                    y += 10;
                }
            snprintf(l, sizeof(l), "Nível %d+", g->level * LEVEL_STEP > 1 ? g->level * LEVEL_STEP : 1);
            ui_text(u, dx + 6, y, l, 0xc8c8d0);
            y += 13;
        }
        if (g->kind == KIND_WEAPON) {
            int lo, hi;
            weapon_roll(gi, &lo, &hi);
            snprintf(l, sizeof(l), "Dano +%d~%d", lo, hi);
            ui_text(u, dx + 6, y, l, 0xe0e0e8);
            snprintf(l, sizeof(l), "Alcance %d", g->extra);
            ui_text(u, dx + 6, y + 11, l, 0xe0e0e8);
            if (g->element) {
                snprintf(l, sizeof(l), "x2 vs %s", g_elements[el_beats(g->element)].name);
                ui_text(u, dx + 6, y + 22, l, g_elements[g->element].rgb);
            }
        } else if (g->kind == KIND_ARMOUR) {
            snprintf(l, sizeof(l), "Defesa %d", g->power);
            ui_text(u, dx + 6, y, l, 0xe0e0e8);
            if (g->extra) {
                snprintf(l, sizeof(l), "PM +%d", g->extra);
                ui_text(u, dx + 6, y + 11, l, 0xe0e0e8);
            }
            if (g->element) {
                snprintf(l, sizeof(l), "1/2 %s", g_elements[g->element].name);
                ui_text(u, dx + 6, y + 22, l, g_elements[g->element].rgb);
            }
        } else {
            const bool all = g->power >= 999;
            if (g->power) snprintf(l, sizeof(l), all ? "PV todo" : "PV +%d", g->power), ui_text(u, dx + 6, y, l, 0xe0e0e8);
            if (g->extra) snprintf(l, sizeof(l), all ? "PM todo" : "PM +%d", g->extra), ui_text(u, dx + 6, y + 11, l, 0xe0e0e8);
            if (g->revive) ui_text(u, dx + 6, y + 22, "Reanima", 0xe0e0e8);
        }
        goods_icon(u, gi, dx + 28, ly + 104, true);
        snprintf(l, sizeof(l), "Tem %d", g_save.bag[gi]);
        ui_text(u, dx + 6, ly + 138, l, 0xc8c8d0);
    }
    ui_hints(u, 8, u->h - 12, "{A} ok  {LR} comprar/vender  {B} equipe", 0x8a96b0);
    if (s_msg[0] && g_t < s_msg_until) {
        ui_box(u, (u->w - len8(s_msg) - 16) / 2, u->h / 2 - 10, len8(s_msg) + 16, 20);
        ui_text(u, (u->w - len8(s_msg)) / 2, u->h / 2 - 4, s_msg, 0xffd080);
    }

    if (g_keys.dir == FART_SE || g_keys.dir == FART_NW) {
        snd_play(S_HOVER, 160);
        s_selling = !s_selling;
        s_first = 0;
        shop_fill();
    }
    const int k = menu_key();
    if (k == FH1_KEY_UP && s_shop.sel == 0 && s_first > 0) {
        s_first--;
        shop_fill();
        return SCR_SHOP;
    }
    if (k == FH1_KEY_DOWN && s_shop.sel == s_shop.n - 1 && s_shop.n == UI_ITEMS) {
        s_first++;
        shop_fill();
        s_shop.sel = UI_ITEMS - 1;
        return SCR_SHOP;
    }
    const int sel = s_shop.sel;
    const int r = ui_menu_key(&s_shop, k);
    if (r == UI_BACK) {
        game_store();
        return SCR_PARTY;
    }
    if (r >= 0) {
        const int g = s_shop_ids[r];
        snd_play(S_BUY, 220);
        if (s_selling) {
            g_save.bag[g]--;
            g_save.coins += g_goods[g].price / 2;
            snprintf(s_msg, sizeof(s_msg), "Vendeu %s", g_goods[g].name);
        } else if (g_save.bag[g] >= 99) {
            snprintf(s_msg, sizeof(s_msg), "A bolsa está cheia");
        } else {
            g_save.bag[g]++;
            g_save.coins -= g_goods[g].price;
            snprintf(s_msg, sizeof(s_msg), "Comprou %s", g_goods[g].name);
        }
        s_msg_until = g_t + 1200;
        shop_fill();
        s_shop.sel = sel < s_shop.n ? sel : s_shop.n - 1;
        if (s_shop.sel < 0) s_shop.sel = 0;
    }
    return SCR_SHOP;
}
