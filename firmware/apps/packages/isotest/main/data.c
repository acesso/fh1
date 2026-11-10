// The rules' numbers: elements, the catalogue, classes, realms and their creatures, the battles of each fase,
// levels and saving. firmware/docs/taticas-da-ilha.md says the same in words.
#include "game.h"
#include <stdio.h>
#include <string.h>

#define C565(r, g, b) (uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3))

// ── Elements ──

const element_t g_elements[EL_COUNT] = {
    { "", "", 0xc8c8d0, 0xffffff },
    { "Fogo", "de Fogo", 0xf05028, 0xffd060 },
    { "Água", "de Água", 0x3c8cf0, 0xa8e0ff },
    { "Terra", "da Terra", 0x6cb040, 0xd8c070 },
    { "Ar", "do Ar", 0xf0d050, 0xffffff },
};

int el_beats(int el) { return el == EL_NONE ? EL_NONE : el == EL_AIR ? EL_FIRE : el + 1; }
int el_weakness(int el) { return el == EL_NONE ? EL_NONE : el == EL_FIRE ? EL_AIR : el - 1; }

uint16_t el_565(int el, bool light)
{
    const uint32_t c = light ? g_elements[el].rgb2 : g_elements[el].rgb;
    return C565((c >> 16) & 255, (c >> 8) & 255, c & 255);
}

// 32 shades by brightness: from a dark tone of the colour through the colour to a pale one. The source's brightness
// is stretched (the critters are dark) so its shading still shows.
static void make_shade(uint16_t out[32], uint32_t rgb)
{
    const int r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
    for (int i = 0; i < 32; i++) {
        int t = i * 12 + 24;                                 // 0..255 brightness, stretched
        if (t > 255) t = 255;
        int R, G, B;
        if (t < 150) {                                       // dark to the colour
            const int k = 40 + t * 215 / 150;
            R = r * k / 255;
            G = g * k / 255;
            B = b * k / 255;
        } else {                                             // the colour to pale
            const int k = (t - 150) * 150 / 105;             // up to 150 of 255 toward white
            R = r + (255 - r) * k / 255;
            G = g + (255 - g) * k / 255;
            B = b + (255 - b) * k / 255;
        }
        out[i] = C565(R, G, B);
    }
}

// An Elemental's: its element glowing, three stops from dark to bright (fire: deep red, orange, yellow).
static void make_glow(uint16_t out[32], uint32_t dark, uint32_t mid, uint32_t light)
{
    for (int i = 0; i < 32; i++) {
        const uint32_t a = i < 16 ? dark : mid, b = i < 16 ? mid : light;
        const int k = (i % 16) * 255 / 15;
        const int r = (int)((a >> 16) & 255) + (((int)((b >> 16) & 255) - (int)((a >> 16) & 255)) * k) / 255;
        const int g = (int)((a >> 8) & 255) + (((int)((b >> 8) & 255) - (int)((a >> 8) & 255)) * k) / 255;
        const int bl = (int)(a & 255) + (((int)(b & 255) - (int)(a & 255)) * k) / 255;
        out[i] = C565(r, g, bl);
    }
}

static uint16_t s_shade[EL_COUNT][32], s_glow[EL_COUNT][32], s_metal[3][32], s_el_ramp[EL_COUNT][4];
enum { METAL_STEEL, METAL_GOLD, METAL_MYTH };
const uint16_t *el_shade(int el) { return el > EL_NONE && el < EL_COUNT ? s_shade[el] : NULL; }

// ── The catalogue ──

goods_t g_goods[GOODS_MAX];
int g_ngoods;

static const char *const s_wname[4][GEAR_LEVELS] = {
    { "Espada", "Gládio", "Montante", "Espadão" },
    { "Adaga", "Katana", "Kodachi", "Nodachi" },
    { "Arco", "Arco Longo", "Arco Duplo", "Arco Real" },
    { "Cajado", "Bastão", "Cetro", "Báculo" },
};
static const char *const s_aname[3][GEAR_LEVELS] = {
    { "Cota de Malha", "Placas", "Placas de Ouro", "Arm. Sagrada" },
    { "Couro", "Couro Batido", "Gibão Élfico", "Escamas" },
    { "Manto", "Manto Azul", "Manto Rúnico", "Manto Arcano" },
};
static const uint8_t s_wicon[4][GEAR_LEVELS] = { { 6, 14, 22, 23 }, { 7, 15, 7, 15 }, { 60, 61, 60, 61 }, { 56, 56, 56, 56 } };
static const uint8_t s_aicon[3][GEAR_LEVELS] = { { 0, 1, 8, 16 }, { 9, 17, 9, 17 }, { 18, 18, 18, 18 } };
static const int16_t s_wprice[GEAR_LEVELS] = { 40, 220, 600, 1400 };
static const int16_t s_aprice[GEAR_LEVELS] = { 30, 180, 500, 1200 };
static const int8_t s_def[3][GEAR_LEVELS] = { { 2, 4, 6, 8 }, { 1, 3, 5, 7 }, { 0, 1, 3, 4 } };
static const int8_t s_sword_big[GEAR_LEVELS] = { 18, 24, 21, 13 }, s_el_big[EL_COUNT] = { -1, 4, 1, 2, 3 };

static int add_goods(const char *name, int kind, int family, int level, int element, int power, int extra, int price,
                     bool revive, int icon, int big)
{
    goods_t *g = &g_goods[g_ngoods];
    strlcpy(g->name, name, sizeof(g->name));
    g->kind = kind;
    g->family = family;
    g->level = level;
    g->element = element;
    g->power = power;
    g->extra = extra;
    g->price = price;
    g->revive = revive;
    g->icon = icon;
    g->big = big;
    return g_ngoods++;
}

// The order is the save's: goods are kept by index, so new ones go at the end.
void goods_init(void)
{
    if (g_ngoods) return;
    for (int e = 0; e < EL_COUNT; e++) make_shade(s_shade[e], g_elements[e].rgb);
    make_glow(s_glow[EL_FIRE], 0x700800, 0xf06818, 0xffe070);
    make_glow(s_glow[EL_WATER], 0x082060, 0x2c8cf0, 0xc8f4ff);
    make_glow(s_glow[EL_EARTH], 0x203008, 0x70b030, 0xe8e090);
    make_glow(s_glow[EL_AIR], 0x504010, 0xe8d040, 0xffffe0);
    make_shade(s_metal[METAL_STEEL], 0x9cb4d0);
    make_shade(s_metal[METAL_GOLD], 0xe8b830);
    make_shade(s_metal[METAL_MYTH], 0xb070f0);
    for (int f = FAM_SWORD; f <= FAM_STAFF; f++)
        for (int l = 0; l < GEAR_LEVELS; l++)
            add_goods(s_wname[f][l], KIND_WEAPON, f, l, EL_NONE, 0, f == FAM_BOW ? (l < 2 ? 4 : 5) : 1, s_wprice[l], false,
                      s_wicon[f][l], f == FAM_SWORD ? s_sword_big[l] : -1);
    for (int e = EL_FIRE; e < EL_COUNT; e++)
        for (int f = FAM_SWORD; f <= FAM_STAFF; f++)
            for (int l = 1; l < GEAR_LEVELS; l++) {
                char n[24];
                snprintf(n, sizeof(n), "%s %s", s_wname[f][l], g_elements[e].of);
                add_goods(n, KIND_WEAPON, f, l, e, 0, f == FAM_BOW ? (l < 2 ? 4 : 5) : 1, s_wprice[l] * 3 / 2, false,
                          s_wicon[f][l], f == FAM_SWORD ? s_el_big[e] : -1);
            }
    for (int f = FAM_HEAVY; f <= FAM_ROBE; f++)
        for (int l = 0; l < GEAR_LEVELS; l++)
            add_goods(s_aname[f - FAM_HEAVY][l], KIND_ARMOUR, f, l, EL_NONE, s_def[f - FAM_HEAVY][l],
                      f == FAM_ROBE ? 5 * (l + 1) : 0, s_aprice[l], false, s_aicon[f - FAM_HEAVY][l], -1);
    add_goods("Poção", KIND_ITEM, FAM_NONE, 0, EL_NONE, 25, 0, 20, false, 46, -1);
    add_goods("Poção Forte", KIND_ITEM, FAM_NONE, 1, EL_NONE, 60, 0, 60, false, 45, -1);
    add_goods("Éter", KIND_ITEM, FAM_NONE, 0, EL_NONE, 0, 15, 40, false, 54, -1);
    add_goods("Elixir", KIND_ITEM, FAM_NONE, 1, EL_NONE, 999, 999, 250, true, 47, -1);
    // Elemental armour (levels 1 to 3): the plain one's defence and PM, half the damage of its element.
    static const char *const el_armour[3][3] = { { "Placas", "Couraça", "Armadura" }, { "Couro", "Gibão", "Escamas" },
                                                 { "Manto", "Túnica", "Veste" } };
    for (int e = EL_FIRE; e < EL_COUNT; e++)
        for (int f = FAM_HEAVY; f <= FAM_ROBE; f++)
            for (int l = 1; l < GEAR_LEVELS; l++) {
                char n[24];
                snprintf(n, sizeof(n), "%s %s", el_armour[f - FAM_HEAVY][l - 1], g_elements[e].of);
                add_goods(n, KIND_ARMOUR, f, l, e, s_def[f - FAM_HEAVY][l], f == FAM_ROBE ? 5 * (l + 1) : 0,
                          s_aprice[l] * 3 / 2, false, s_aicon[f - FAM_HEAVY][l], -1);
            }
    // Its colours: four of the element's shades, dark to light.
    for (int e = EL_FIRE; e < EL_COUNT; e++)
        for (int k = 0; k < 4; k++) s_el_ramp[e][k] = s_shade[e][4 + k * 8];
}

static int goods_find(int kind, int family, int level, int element)
{
    for (int g = 0; g < g_ngoods; g++)
        if (g_goods[g].kind == kind && g_goods[g].family == family && g_goods[g].level == level && g_goods[g].element == element)
            return g;
    return 0;
}
static int item_named(const char *n)
{
    for (int g = 0; g < g_ngoods; g++)
        if (!strcmp(g_goods[g].name, n)) return g;
    return 0;
}

void weapon_roll(int g, int *lo, int *hi)
{
    const int l = g_goods[g].kind == KIND_WEAPON ? g_goods[g].level : 0;
    *lo = 2 * l + 1;
    *hi = 2 * l + 2;
}

bool spell_attuned(const hero_t *h, int spell)
{
    return g_spells[spell].element && g_goods[h->weapon].element == g_spells[spell].element;
}

void spell_roll(const hero_t *h, int spell, int *lo, int *hi)
{
    weapon_roll(h->weapon, lo, hi);
    *lo += 1;
    *hi += 1;
    if (spell_attuned(h, spell)) {
        *lo *= 2;
        *hi *= 2;
    }
}

bool goods_in_shop(int g)
{
    const goods_t *x = &g_goods[g];
    // Nothing that does nothing (an armour of no defence and no PM, an item that restores nothing).
    if (x->kind == KIND_ARMOUR && !x->power && !x->extra) return false;
    if (x->kind == KIND_ITEM && !x->power && !x->extra && !x->revive) return false;
    if (!x->level) return true;
    if (x->element) return tier_cleared(x->element - 1, x->level - 1);
    for (int r = 0; r < REALMS; r++)
        if (tier_cleared(r, x->level - 1)) return true;
    return false;
}

const spell_t g_spells[] = {
    { "Fogo", 4, 4, EL_FIRE, false }, { "Onda", 4, 4, EL_WATER, false }, { "Rocha", 4, 4, EL_EARTH, false },
    { "Trovão", 5, 4, EL_AIR, false }, { "Cura", 4, 3, EL_NONE, true },
};
const int g_nspells = sizeof(g_spells) / sizeof(g_spells[0]);

const class_t g_classes[CLS_COUNT] = {
    { "Paladino", "knight", 1 << 4, FAM_SWORD, FAM_HEAVY, 24, 5, 6, 1, 4, 1 },
    { "Ninja", "warrior", 0, FAM_BLADE, FAM_LIGHT, 20, 4, 0, 0, 5, 3 },
    { "Arqueiro", "adventurer", 0, FAM_BOW, FAM_LIGHT, 18, 4, 0, 0, 4, 2 },
    { "Mago", "mage", 0x1f, FAM_STAFF, FAM_ROBE, 15, 3, 14, 3, 4, 1 },
};

// ── Realms ──

const int g_tier_len[TIERS] = { 3, 6, 9 };
static const int s_tier_lo[TIERS] = { 1, 5, 10 }, s_tier_hi[TIERS] = { 4, 9, 15 };

const realm_t g_realms[REALMS] = {
    { "Fogo", "fogo", EL_FIRE, THEME_ROCKS, { "Campos de Cinza", "Encosta Ardente", "Coração do Vulcão" },
      { "Planícies de cinza e rios de lava.", "Ignis arde no coração do vulcão." },
      { "Brasas", "Cinzas", "Fenda", "Gêiser", "Forja", "Caldeira", "Escória", "Fumarola", "Cratera" },
      { E_WOLF, E_BOAR, E_SLIME }, 3, E_WOLF, "Lobo Alfa", "Ignis", C565(40, 10, 8) },
    { "Água", "agua", EL_WATER, THEME_MEADOW, { "Praia Alagada", "Lagos Profundos", "Abismo das Marés" },
      { "Praias alagadas e lagos sem fundo.", "Maris dorme sob as marés." },
      { "Orla", "Mangue", "Recife", "Baía", "Delta", "Lagoa", "Estuário", "Gruta", "Maré Alta" },
      { E_SLIME, E_BADGER, E_WOLF }, 3, E_BOAR, "Javali Rei", "Maris", C565(8, 24, 56) },
    { "Terra", "terra", EL_EARTH, THEME_FOREST, { "Bosque Antigo", "Raízes Profundas", "Bosque Sagrado" },
      { "Florestas antigas e raízes profundas.", "Gaia guarda o bosque sagrado." },
      { "Clareira", "Trilha", "Toca", "Ravina", "Pedreira", "Raízes", "Musgo", "Cânion", "Colina" },
      { E_BADGER, E_STAG, E_BOAR, E_SLIME }, 4, E_STAG, "Cervo Rei", "Gaia", C565(14, 30, 12) },
    { "Ar", "ar", EL_AIR, THEME_ROCKS, { "Colinas do Vento", "Picos Gelados", "Trono das Nuvens" },
      { "Picos altos varridos pelo vento.", "Zéfiro ruge entre as nuvens." },
      { "Colina", "Penhasco", "Planalto", "Desfiladeiro", "Pico", "Mirante", "Garganta", "Aresta", "Cume" },
      { E_STAG, E_WOLF, E_SLIME }, 3, E_WOLF, "Lobo do Vento", "Zéfiro", C565(70, 96, 128) },
};

bool tier_cleared(int realm, int tier) { return g_save.won[realm][tier] >= g_tier_len[tier]; }
bool tier_open(int realm, int tier) { return tier == 0 || tier_cleared(realm, tier - 1); }

// ── Creatures: stats at level 1 and per level; bosses take their realm's art ──

const enemy_t g_enemies[E_COUNT] = {
    [E_WOLF] = { "Lobo", "Lobos", "wolf", false, true, 8, 3, 0, 0, 4, 1, 1, 1, 1 },
    [E_BOAR] = { "Javali", "Javalis", "boar", false, true, 11, 4, 1, 1, 3, 1, 1, 1, 1 },
    [E_BADGER] = { "Texugo", "Texugos", "badger", false, true, 6, 3, 0, 0, 5, 2, 1, 1, 1 },
    [E_STAG] = { "Cervo", "Cervos", "stag", false, true, 9, 3, 1, 0, 5, 2, 1, 1, 1 },
    [E_SLIME] = { "Slime", "Slimes", "slime", true, false, 7, 3, 0, 0, 3, 1, 1, 1, 1 },
    [E_ALPHA] = { "", "", "", false, true, 30, 10, 2, 1, 4, 1, 1, 4, 2 },
    [E_KING] = { "Rei Slime", "Reis Slime", "king", true, false, 40, 12, 3, 2, 3, 1, 1, 6, 2 },
    [E_ELEMENTAL] = { "", "", "slime", true, true, 60, 16, 4, 3, 3, 2, 3, 10, 2 },
};

void foe_make(int type, int realm, int level, bool caster, foe_t *f)
{
    const enemy_t *e = &g_enemies[type];
    const realm_t *rm = &g_realms[realm];
    memset(f, 0, sizeof(*f));
    const enemy_t *look = type == E_ALPHA ? &g_enemies[rm->alpha] : e;
    strlcpy(f->name, type == E_ALPHA ? rm->alpha_name : type == E_ELEMENTAL ? rm->elemental : e->name, sizeof(f->name));
    if (look->realm_art) snprintf(f->art, sizeof(f->art), "%s.%s", look->art, rm->key);
    else strlcpy(f->art, look->art, sizeof(f->art));
    f->shade = type == E_ELEMENTAL ? s_glow[rm->element] : look->tinted ? el_shade(rm->element) : NULL;
    f->hp = e->hp + e->hp_level * (level - 1);
    f->atk = level + 2 + e->atk;
    f->def = e->def;
    f->move = type == E_ALPHA ? look->move : e->move;
    f->jump = type == E_ALPHA ? look->jump : e->jump;
    f->range = e->range;
    f->xp = (6 + 3 * level) * e->reward;
    f->coins = (4 + 2 * level) * e->reward;
    f->element = rm->element;
    f->zoom = e->zoom;
    f->boss = e->reward > 1;
    // A caster (one in each pack from fase 2 on): its realm's element from 4 cells away, a little weaker, worth more.
    if (caster && !f->boss) {
        f->caster = true;
        f->range = 4;
        f->atk -= 1;
        f->hp = f->hp * 4 / 5;
        f->xp = f->xp * 3 / 2;
        f->coins = f->coins * 3 / 2;
        char n[40];
        snprintf(n, sizeof(n), "%s Arcano", f->name);
        strlcpy(f->name, n, sizeof(f->name));
    }
}

const char *foe_plural(int type, int realm)
{
    if (type == E_ALPHA) return g_realms[realm].alpha_name;
    if (type == E_ELEMENTAL) return g_realms[realm].elemental;
    return g_enemies[type].plural;
}

// ── Battles: random islands, the bosses' arenas drawn by hand ──

static const char *const den_h[12] = {
    "000000000000", "011111111110", "011222222110", "012233332210", "012345543210", "012345553210",
    "012344443210", "011233332110", "011122221110", "011111111110", "011111111100", "000000000000",
};
static const char *const den_o[12] = {
    "............", "............", "..p......p..", "............", "..r......r..", "............",
    "............", "............", ".s........s.", "............", "............", "............",
};
static const char *const pool_h[12] = {
    "000000000000", "011111111110", "012222222210", "012111111210", "012100001210", "012100001210",
    "012100001210", "012111111210", "012222222210", "011111111110", "011111111110", "000000000000",
};
static const char *const pool_o[12] = {
    "............", ".r........r.", "............", "............", "............", "............",
    "............", "............", "............", "............", ".p........p.", "............",
};
static const char *const shrine_h[12] = {
    "000000000000", "011111111110", "011222222110", "012233332210", "012234432210", "012344443210",
    "012344443210", "012234432210", "012233332210", "011222222110", "011111111110", "000000000000",
};
static const char *const shrine_o[12] = {
    "............", "............", ".r........r.", "............", "............", "............",
    "............", "............", "............", ".r........r.", "............", "............",
};
// The arenas: the boss first, then its guards; the heroes' cells.
static const struct {
    const char *const *h, *const *o;
    uint8_t foes[6][2], heroes[3][2];
} s_arena[TIERS] = {
    { den_h, den_o, { { 5, 5 }, { 7, 4 }, { 4, 6 }, { 3, 3 } }, { { 2, 9 }, { 3, 9 }, { 2, 8 } } },
    { pool_h, pool_o, { { 8, 2 }, { 6, 2 }, { 9, 4 }, { 3, 2 }, { 9, 7 } }, { { 2, 9 }, { 3, 9 }, { 1, 8 } } },
    { shrine_h, shrine_o, { { 5, 5 }, { 4, 3 }, { 7, 4 }, { 3, 6 }, { 6, 7 } }, { { 2, 10 }, { 3, 10 }, { 1, 9 } } },
};
static const uint8_t s_count[TIERS][9] = { { 2, 3, 2 }, { 3, 3, 4, 4, 4, 3 }, { 4, 4, 4, 5, 5, 5, 5, 5, 3 } };
static const char *const s_boss_about[TIERS] = { "O chefe da fase espera no alto.", "O Rei Slime guarda o lago.",
                                                 "O Elemental do reino desperta." };

void battle_make(int realm, int tier, int index, battle_def_t *b)
{
    const realm_t *rm = &g_realms[realm];
    const int n = g_tier_len[tier];
    memset(b, 0, sizeof(*b));
    b->realm = realm;
    b->tier = tier;
    b->index = index;
    b->theme = rm->theme;
    const int level = s_tier_lo[tier] + (s_tier_hi[tier] - s_tier_lo[tier]) * index / (n - 1);
    b->boss = index == n - 1;
    const uint32_t mix = (uint32_t)(realm * 7 + tier * 31 + index * 131 + 17) * 2654435761u;
    if (b->boss) {
        static const uint8_t boss[TIERS] = { E_ALPHA, E_KING, E_ELEMENTAL };
        if (tier == 0) snprintf(b->name, sizeof(b->name), "Covil: %s", rm->alpha_name);
        else if (tier == 1) snprintf(b->name, sizeof(b->name), "Lago do Rei Slime");
        else snprintf(b->name, sizeof(b->name), "Santuário de %s", rm->elemental);
        b->about = s_boss_about[tier];
        b->map = MAP_FIXED;
        b->cols = b->rows = 12;
        b->heights = s_arena[tier].h;
        b->objects = s_arena[tier].o;
        memcpy(b->heroes, s_arena[tier].heroes, sizeof(b->heroes));
        b->n = 1 + s_count[tier][index];
        if (b->n > 5) b->n = 5;
        for (int k = 0; k < b->n; k++) {
            b->foes[k].type = k ? rm->foes[(mix >> (k * 3)) % rm->nfoes] : boss[tier];
            b->foes[k].level = level;
            b->foes[k].c = s_arena[tier].foes[k][0];
            b->foes[k].r = s_arena[tier].foes[k][1];
        }
        if (tier >= 1) b->foes[1].caster = true;                 // one of the boss's guards casts
        return;
    }
    strlcpy(b->name, rm->places[(index + tier * 3) % 9], sizeof(b->name));
    static const char *const about[TIERS] = { "As criaturas barram o caminho.", "O reino fica mais hostil.",
                                              "Perto do Elemental, tudo é feroz." };
    b->about = about[tier];
    b->map = MAP_RANDOM;
    b->cols = b->rows = 11 + (index + tier) % 2 + (tier == 2);
    b->seed = (uint16_t)(1 + realm * 1000 + tier * 100 + index);
    b->n = s_count[tier][index];
    for (int k = 0; k < b->n; k++) {
        b->foes[k].type = rm->foes[(mix >> (k * 4)) % rm->nfoes];
        b->foes[k].level = level - (k >= 3);            // the bigger packs: some a level below
        if (b->foes[k].level < 1) b->foes[k].level = 1;
    }
    if (tier >= 1) b->foes[(mix >> 20) % b->n].caster = true;   // from fase 2: one caster in each pack
}

battle_def_t g_battle;

// ── The roster, levels and saving ──

save_t g_save;
result_t g_result;

#define SAVE_VERSION 5                       // 2: the classes; 3: the chain; 4: the realms; 5: elemental armour

void game_new(void)
{
    static const struct { const char *name; uint8_t cls; } start[ROSTER] = {
        { "Artur", CLS_PALADIN }, { "Lia", CLS_NINJA }, { "Tomé", CLS_ARCHER }, { "Nara", CLS_MAGE },
    };
    goods_init();
    memset(&g_save, 0, sizeof(g_save));
    g_save.version = SAVE_VERSION;
    for (int i = 0; i < ROSTER; i++) {
        hero_t *h = &g_save.heroes[i];
        strlcpy(h->name, start[i].name, sizeof(h->name));
        h->cls = start[i].cls;
        h->level = 1;
        h->weapon = goods_find(KIND_WEAPON, g_classes[h->cls].weapons, 0, EL_NONE);
        h->armour = goods_find(KIND_ARMOUR, g_classes[h->cls].armours, 0, EL_NONE);
        h->in_party = i < PARTY_MAX;
    }
    g_save.bag[item_named("Poção")] = 3;
    g_save.bag[item_named("Éter")] = 1;
    g_save.coins = 100;
}

// The save before the realms (version 3): the heroes' names, classes and levels, the coins and the potions come
// along; gear goes back to each class's first, and the old weapons and armour in the bag are sold.
typedef struct {
    char name[24];
    uint8_t cls, level;
    uint16_t xp;
    int16_t hp_max, mp_max, strength;
    uint8_t move, jump, weapon, armour;
    bool in_party;
} hero_v3_t;
typedef struct {
    uint32_t version;
    hero_v3_t heroes[ROSTER];
    uint16_t bag[32];
    uint32_t coins;
    uint8_t chain_won;
} save_v3_t;

// Version 4: the same with a bag of 96 goods (the new ones were added after them).
typedef struct {
    uint32_t version;
    hero_t heroes[ROSTER];
    uint8_t bag[96];
    uint32_t coins;
    uint8_t won[REALMS][TIERS];
    uint8_t realm, tier;
} save_v4_t;

static void game_upgrade_v4(const save_v4_t *o)
{
    game_new();
    memcpy(g_save.heroes, o->heroes, sizeof(g_save.heroes));
    memcpy(g_save.bag, o->bag, sizeof(o->bag));
    g_save.coins = o->coins;
    memcpy(g_save.won, o->won, sizeof(g_save.won));
    g_save.realm = o->realm;
    g_save.tier = o->tier;
}

static void game_upgrade(const save_v3_t *o)
{
    static const int16_t old_price[11] = { 100, 120, 150, 80, 220, 380, 20, 80, 140, 220, 420 };
    game_new();
    g_save.coins = o->coins;
    for (int i = 0; i < ROSTER; i++) {
        hero_t *h = &g_save.heroes[i];
        strlcpy(h->name, o->heroes[i].name, sizeof(h->name));
        h->cls = o->heroes[i].cls < CLS_COUNT ? o->heroes[i].cls : h->cls;
        h->level = o->heroes[i].level < 1 ? 1 : o->heroes[i].level > LEVEL_MAX ? LEVEL_MAX : o->heroes[i].level;
        h->in_party = o->heroes[i].in_party;
        h->weapon = goods_find(KIND_WEAPON, g_classes[h->cls].weapons, 0, EL_NONE);
        h->armour = goods_find(KIND_ARMOUR, g_classes[h->cls].armours, 0, EL_NONE);
    }
    for (int k = 0; k < 11; k++) g_save.coins += o->bag[k] * old_price[k] / 2;
    g_save.bag[item_named("Poção")] = o->bag[11] > 99 ? 99 : o->bag[11];
    g_save.bag[item_named("Éter")] = o->bag[12] > 99 ? 99 : o->bag[12];
    g_save.bag[item_named("Elixir")] = o->bag[13] > 99 ? 99 : o->bag[13];
}

// Whose progress is in memory: player 1's user when it was loaded ("" without users).
char g_owner[24];

bool game_owner_now(char *now, size_t n)
{
    now[0] = 0;
    fh1_user(0, now, n);
    return strcmp(now, g_owner) == 0;
}

void game_load(void)
{
    static union { save_t now; save_v4_t v4; save_v3_t v3; } buf;
    goods_init();
    g_owner[0] = 0;
    fh1_user(0, g_owner, sizeof(g_owner));
    const int n = fh1_state_load("progresso", &buf, sizeof(buf));
    if (n == (int)sizeof(save_t) && buf.now.version == SAVE_VERSION) g_save = buf.now;
    else if (n == (int)sizeof(save_v4_t) && buf.v4.version == 4) game_upgrade_v4(&buf.v4);
    else if (n == (int)sizeof(save_v3_t) && buf.v3.version == 3) {
        game_upgrade(&buf.v3);
        fh1_log("save: upgraded from version 3");
    } else game_new();
}

bool g_nosave;                                  // development: nothing is written (dev command "nosave")
// Saved into player 1's user's folder: only while that is still whose progress this is (another user who has just
// taken the controller over gets their own, loaded by the main loop).
void game_store(void)
{
    char now[24];
    if (!g_nosave && game_owner_now(now, sizeof(now))) fh1_state_save("progresso", &g_save, sizeof(g_save));
}

int xp_next(int level) { return 20 + 14 * level; }

bool hero_gain(hero_t *h, int xp)
{
    bool up = false;
    if (h->level >= LEVEL_MAX) return false;
    h->xp += xp;
    while (h->level < LEVEL_MAX && h->xp >= xp_next(h->level)) {
        h->xp -= xp_next(h->level);
        h->level++;
        up = true;
    }
    if (h->level >= LEVEL_MAX) h->xp = 0;
    return up;
}

int hero_hp_max(const hero_t *h) { return g_classes[h->cls].hp + g_classes[h->cls].hp_level * (h->level - 1); }
int hero_mp_max(const hero_t *h)
{
    return g_classes[h->cls].mp + g_classes[h->cls].mp_level * (h->level - 1) + g_goods[h->armour].extra;
}
int hero_defence(const hero_t *h) { return g_goods[h->armour].power; }

bool hero_can_use(const hero_t *h, int g, int *need)
{
    const goods_t *x = &g_goods[g];
    *need = x->kind == KIND_ITEM ? 0 : x->level * LEVEL_STEP;
    if (x->kind == KIND_WEAPON && x->family != g_classes[h->cls].weapons) return false;
    if (x->kind == KIND_ARMOUR && x->family != g_classes[h->cls].armours) return false;
    return h->level >= *need;
}

int party_count(void)
{
    int n = 0;
    for (int i = 0; i < ROSTER; i++) n += g_save.heroes[i].in_party;
    return n;
}

// ── How gear looks ──

const char *weapon_art(int g, int *rest, const uint16_t **shade)
{
    static const char *const art[4][GEAR_LEVELS] = {
        { "wpn.sword", "wpn.sword2", "wpn.sword3", "wpn.sword4" }, { "wpn.dagger", "wpn.blade", "wpn.dagger", "wpn.blade" },
        { "wpn.bow", "wpn.bow2", "wpn.bow", "wpn.bow2" }, { "wpn.staff", "wpn.staff", "wpn.staff", "wpn.staff" },
    };
    // The same art again at a higher level: a metal of its own (steel, gold, a violet myth-metal).
    static const int8_t metal[4][GEAR_LEVELS] = { { -1, -1, -1, -1 }, { -1, -1, METAL_GOLD, METAL_MYTH },
                                                  { -1, -1, METAL_GOLD, METAL_MYTH }, { -1, METAL_STEEL, METAL_GOLD, METAL_MYTH } };
    const goods_t *x = &g_goods[g];
    *shade = NULL;
    if (x->kind != KIND_WEAPON || x->family > FAM_STAFF) return NULL;
    *rest = x->family == FAM_SWORD || x->family == FAM_BLADE ? 300 : 80;
    if (x->element) *shade = el_shade(x->element);
    else if (metal[x->family][x->level] >= 0) *shade = s_metal[metal[x->family][x->level]];
    return art[x->family][x->level];
}

bool weapon_is_bow(int g) { return g_goods[g].kind == KIND_WEAPON && g_goods[g].family == FAM_BOW; }

const uint16_t *armour_look(int g, uint8_t *cover)
{
    static const uint16_t ramps[3][GEAR_LEVELS][4] = {
        { { C565(46, 50, 60), C565(96, 104, 118), C565(150, 158, 170), C565(206, 212, 222) },     // chain
          { C565(60, 66, 84), C565(124, 134, 160), C565(188, 198, 220), C565(246, 248, 255) },    // plate
          { C565(90, 60, 10), C565(170, 120, 30), C565(230, 180, 60), C565(255, 236, 150) },     // gold
          { C565(70, 90, 120), C565(150, 180, 220), C565(215, 230, 250), C565(255, 255, 255) } }, // holy
        { { C565(58, 34, 24), C565(104, 64, 36), C565(150, 98, 58), C565(196, 140, 90) },        // leather
          { C565(36, 30, 30), C565(70, 58, 54), C565(110, 96, 88), C565(160, 146, 132) },        // studded
          { C565(20, 50, 24), C565(40, 96, 46), C565(80, 150, 80), C565(150, 210, 140) },        // elven
          { C565(70, 14, 10), C565(130, 30, 22), C565(190, 60, 40), C565(240, 130, 90) } },      // scales
        { { C565(40, 22, 64), C565(78, 44, 116), C565(120, 76, 168), C565(170, 128, 210) },      // robe
          { C565(18, 30, 70), C565(36, 60, 130), C565(70, 104, 190), C565(140, 170, 235) },      // blue
          { C565(10, 50, 54), C565(24, 96, 100), C565(50, 150, 150), C565(130, 210, 200) },      // runes
          { C565(70, 10, 30), C565(130, 24, 50), C565(200, 60, 80), C565(250, 190, 120) } },     // arcane
    };
    static const uint8_t T = 1 << FART_PART_TORSO, A = 1 << FART_PART_ARMS, L = 1 << FART_PART_LEGS;
    static const uint8_t covers[3][GEAR_LEVELS] = { { T | A, T | A | L, T | A | L, T | A | L }, { T, T | A, T | A, T | A | L },
                                                     { T | L, T | L, T | L, T | A | L } };
    const goods_t *x = &g_goods[g];
    *cover = 0;
    if (x->kind != KIND_ARMOUR) return NULL;
    const int f = x->family - FAM_HEAVY;
    *cover = covers[f][x->level];
    return x->element ? s_el_ramp[x->element] : ramps[f][x->level];
}
