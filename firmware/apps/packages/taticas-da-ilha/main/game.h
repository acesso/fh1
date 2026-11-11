// Táticas da Ilha: four elemental realms, each a path of fases (3, 6 and 9 battles, a boss at the end of each);
// title, party, realm, path, battle, summary, shop, and back to the party. Every text is in Portuguese. Progress
// (the roster, the bag, the coins, the battles won) is saved with fh1_state_save.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "fh1_sdk.h"
#include "fart.h"
#include "iso.h"
#include "ui.h"
#include "snd.h"

// ── Keys, read once per frame ──
typedef struct {
    uint32_t down, edge;             // bits 1 << FH1_KEY_*
    int dir;                         // a D-pad press or repeat this frame: FART_NE (Up), SE (Right), SW (Down), NW (Left); -1
    int turn;                        // turn the map's view this frame: +1 (Select, R1), -1 (L1), 0
} keys_t;
extern keys_t g_keys;
static inline bool key(int k) { return g_keys.edge & (1u << k); }
// Up/Down for menus from the D-pad (with repeat): FH1_KEY_UP / FH1_KEY_DOWN, or A / B pressed, or -1.
int menu_key(void);

// ── Elements: each realm's; an attack of an element hits the one it beats twice as hard ──
// Fire beats water, water beats earth, earth beats air, air beats fire.
enum { EL_NONE, EL_FIRE, EL_WATER, EL_EARTH, EL_AIR, EL_COUNT };
typedef struct {
    const char *name, *of;           // "Fogo", "de Fogo"
    uint32_t rgb, rgb2;              // its colour, a lighter one (sparks)
} element_t;
extern const element_t g_elements[EL_COUNT];
int el_beats(int el);                // the element el hits twice as hard
int el_weakness(int el);             // the element that hits el twice as hard
const uint16_t *el_shade(int el);    // 32 shades of its colour (fart_style_t), NULL for none
uint16_t el_565(int el, bool light);

// ── The catalogue: weapons and armour in four levels per class (0..3), elemental weapons and armour, items ──
// An elemental weapon's roll counts twice against the element it beats; an elemental armour halves the damage of
// its own element.
enum { KIND_WEAPON, KIND_ARMOUR, KIND_ITEM };
enum { FAM_SWORD, FAM_BLADE, FAM_BOW, FAM_STAFF, FAM_HEAVY, FAM_LIGHT, FAM_ROBE, FAM_NONE };
#define GEAR_LEVELS 4
#define LEVEL_STEP 5                 // a hero uses gear of level n from level n * 5
#define LEVEL_MAX 15
typedef struct {
    char name[24];
    uint8_t kind, family, level, element;
    int16_t power;                   // armour: defence; item: PV healed
    int16_t extra;                   // weapon: range; armour: PM bonus; item: PM restored
    int16_t price;
    bool revive;                     // an item that brings one back
    uint8_t icon;                    // icon.N (16 x 16, the bag icons)
    int8_t big;                      // sword.N (32 x 32) for the shop's detail, or -1
} goods_t;
#define GOODS_MAX 128
extern goods_t g_goods[GOODS_MAX];
extern int g_ngoods;
void goods_init(void);
// A weapon's addition to its hero's damage: lo..hi (level n: 2n + 1 .. 2n + 2).
void weapon_roll(int g, int *lo, int *hi);
void goods_icon(const ui_t *u, int g, int x, int y, bool big);
// How gear looks on a hero: a weapon's rotated images, its angle at rest and its colour (NULL: its own); an
// armour's shades (an elemental one's in its element's colours) and the parts it covers.
const char *weapon_art(int g, int *rest, const uint16_t **shade);
bool weapon_is_bow(int g);
const uint16_t *armour_look(int g, uint8_t *cover);
void coin_icon(const ui_t *u, int x, int y);
// In the shop: always for level 0; level n when any realm's fase n is cleared; an elemental weapon of level n
// when its own realm's fase n is.
bool goods_in_shop(int g);

typedef struct { const char *name; int mp, range, element; bool heal; } spell_t;
extern const spell_t g_spells[];
extern const int g_nspells;

// ── The roster ──
#define ROSTER 4
#define PARTY_MAX 3
typedef struct {
    char name[24];                   // UTF-8, up to 8 letters
    uint8_t cls;                     // CLS_*
    uint8_t level;                   // 1..LEVEL_MAX: also its damage before its weapon
    uint16_t xp;
    uint8_t weapon, armour;          // goods indexes
    bool in_party;
} hero_t;
enum { CLS_PALADIN, CLS_NINJA, CLS_ARCHER, CLS_MAGE, CLS_COUNT };   // art: knight, warrior, adventurer, mage
typedef struct {
    const char *name, *art;
    uint16_t spells;                 // bit i: g_spells[i]
    uint8_t weapons, armours;        // FAM_*
    int hp, hp_level, mp, mp_level, move, jump;
} class_t;
extern const class_t g_classes[CLS_COUNT];

#define REALMS 4
#define TIERS 3
typedef struct {
    uint32_t version;
    hero_t heroes[ROSTER];
    uint8_t bag[GOODS_MAX];          // goods owned, not worn
    uint32_t coins;
    uint8_t won[REALMS][TIERS];      // battles of each fase won, in order
    uint8_t realm, tier;             // the path last looked at
} save_t;
extern save_t g_save;
void game_new(void);
void game_load(void);
void game_store(void);
extern bool g_nosave;
int xp_next(int level);              // XP to reach the next level
bool hero_gain(hero_t *h, int xp);   // true if it levelled up
int hero_hp_max(const hero_t *h);
int hero_mp_max(const hero_t *h);
int hero_defence(const hero_t *h);
// Whether the hero can use the goods: its class's and its level's (*need: the level it needs).
bool hero_can_use(const hero_t *h, int g, int *need);
int party_count(void);
// A spell's addition from the hero's weapon (a staff: magic's level is the staff's): its roll + 1, twice when the
// staff is of the spell's element (attuned). Then, on a target, twice against the element it beats, half against
// its own.
void spell_roll(const hero_t *h, int spell, int *lo, int *hi);
bool spell_attuned(const hero_t *h, int spell);

// ── Realms, fases and battles ──
extern const int g_tier_len[TIERS];  // 3, 6, 9 battles; the last a boss (the third fase's: the Elemental)
enum { THEME_MEADOW, THEME_FOREST, THEME_ROCKS };
enum { E_WOLF, E_BOAR, E_BADGER, E_STAG, E_SLIME, E_ALPHA, E_KING, E_ELEMENTAL, E_COUNT };
typedef struct {
    const char *name, *key;          // "Fogo", "fogo" (art names: slime.fogo...)
    uint8_t element, theme;
    const char *tiers[TIERS];        // the fases' names
    const char *about[2];
    const char *places[9];
    uint8_t foes[4], nfoes;          // the creatures that live there
    uint8_t alpha;                   // the first boss: a big one of these (E_WOLF...)
    const char *alpha_name, *elemental;
    uint16_t sky;
} realm_t;
extern const realm_t g_realms[REALMS];
bool tier_open(int realm, int tier);
bool tier_cleared(int realm, int tier);

typedef struct {
    const char *name, *plural, *art; // art: a critter's ("wolf"), or a realm's own (slime: "slime.fogo")
    bool realm_art, tinted;          // art per realm; recoloured to the realm (critters)
    int hp, hp_level, atk, def, move, jump, range;
    int reward;                      // XP and coins times this (bosses)
    uint8_t zoom;
} enemy_t;
extern const enemy_t g_enemies[E_COUNT];
// An enemy as it stands in a battle: its name and art for its realm, its stats at its level.
typedef struct {
    char name[24], art[24];
    const uint16_t *shade;
    int hp, atk, def, move, jump, range, xp, coins, element, zoom;
    bool boss, caster;
} foe_t;
void foe_make(int type, int realm, int level, bool caster, foe_t *f);
const char *foe_plural(int type, int realm);

// A battle: its map made from a seed (MAP_RANDOM, foes placed far from the heroes) or drawn by hand (MAP_FIXED:
// a row of height digits per map row, '0' water; objects in a second set of rows: r rock, p plant, s stump).
enum { MAP_RANDOM, MAP_FIXED };
typedef struct {
    char name[32];
    const char *about;
    uint8_t realm, tier, index;
    uint8_t map, theme, cols, rows;
    uint16_t seed;
    const char *const *heights, *const *objects;
    int n;
    struct { uint8_t type, level, c, r; bool caster; } foes[6];   // fixed maps: where they stand (random maps: c, r
                                                                  // unused); caster: casts its realm's element
    uint8_t heroes[3][2];                            // fixed maps: where the heroes start
    bool boss;
} battle_def_t;
void battle_make(int realm, int tier, int index, battle_def_t *b);
// The test field in a realm's colours (its creatures, its first boss): the engine's tests and showcase.
void battle_test(int realm, battle_def_t *b);
extern battle_def_t g_battle;        // the battle chosen (the loading screen, the battle, its summary)
// Builds the battle's map into m (initialised to its size); spots: the heroes' starting cells, foes: the enemies'.
void map_build(iso_map_t *m, const battle_def_t *b, uint8_t spots[3][2], uint8_t foes[6][2]);
// The realm's colours over the built scenery (lava for water in the fire realm, a cold blue by the sea...).
// The realm's colours over the map's scenery and its sky (before iso_build_layer).
void map_grade(iso_map_t *m, int realm);

// What a battle leaves for the summary.
typedef struct {
    bool won;
    int nkills;
    struct { uint8_t type, level; uint16_t coins, xp; } kills[8];   // in order
    int coins;
    int xp[PARTY_MAX];               // per party member
    int hero[PARTY_MAX];             // roster index per party member
    bool levelled[PARTY_MAX];
    int level_before[PARTY_MAX], xp_before[PARTY_MAX];
    int nparty;
    bool cleared;                    // this win cleared its fase
} result_t;
extern result_t g_result;

// ── Shared ──
extern fart_t g_art;
extern uint32_t g_t;                 // ms since start
extern bool g_info;
void draw_back(const ui_t *u);       // the menu screens' background
void hero_sprite(const ui_t *u, int cls, int x, int y, int dir);   // a hero standing, feet at (x, y)
// A hero as it is equipped (armour over its body, its weapon in hand), feet at (x, y): walking in place or
// standing, facing dir.
void hero_draw(const ui_t *u, const hero_t *h, int x, int y, int dir, bool walking, uint32_t t);
// Walking in place, then turning to the next facing: the facing and whether it walks, at time t.
void hero_pose(uint32_t t, int *dir, bool *walking);
// Player 1's user changed while the game runs: their own progress is loaded (g_owner: whose it is).
extern char g_owner[24];
bool game_owner_now(char *now, size_t n);           // true while player 1's user is still g_owner
void party_notice(const char *who);

// ── Screens: enter, then a frame each; a frame returns the next screen ──
typedef enum { SCR_TITLE, SCR_PARTY, SCR_REALM, SCR_PICK, SCR_LOAD, SCR_BATTLE, SCR_SHOP, SCR_QUIT } screen_t;
void title_enter(void);
screen_t title_frame(const ui_t *u, uint32_t dt);
void party_enter(void);
screen_t party_frame(const ui_t *u, uint32_t dt);
void realm_enter(void);
screen_t realm_frame(const ui_t *u, uint32_t dt);
void load_enter(void);
screen_t load_frame(const ui_t *u, uint32_t dt);
void pick_enter(void);
screen_t pick_frame(const ui_t *u, uint32_t dt);
bool battle_enter(void);
screen_t battle_frame(const ui_t *u, uint32_t dt);
void battle_leave(void);
void shop_enter(void);
screen_t shop_frame(const ui_t *u, uint32_t dt);
