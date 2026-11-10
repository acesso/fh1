// A battle in a realm: the party (up to three heroes) against the battle's creatures, turn by turn. Damage: the
// hero's level plus its weapon's roll (doubled against the element its weapon beats), less the defence.
// Choosing: D-pad cursor, X / Y next / previous hero, A a hero's menu, Start the battle menu.
#include "game.h"
#include "fx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef PC_HOST
void battle_debug(void);
#endif
#define MAXCELLS (16 * 16)
#define COLS (s_map.cols)
#define ROWS (s_map.rows)

typedef struct {
    const char *name;
    bool hero;
    hero_t *h;                       // heroes: their roster entry (stats, gear)
    int type, level;                 // enemies: E_*, its level
    foe_t foe;                       // enemies: name, art, stats
    int hp, hp_max, mp, mp_max;
    bool moved, acted, done, down;
    const fart_anim_t *attack, *death, *hit;
    int slot;                        // heroes: the party slot (result_t arrays)
    int range;                       // enemies: how far they attack (1: next to them)
} fighter_t;

typedef enum { ST_CHOOSE, ST_MENU, ST_MOVE, ST_TARGET, ST_LIST, ST_BUSY, ST_ENEMY, ST_BATTLE, ST_OVER, ST_SUMMARY } state_t;
enum { ACT_ATTACK, ACT_SPELL, ACT_ITEM };
enum { M_MOVE, M_ATTACK, M_MAGIC, M_ITEMS, M_EQUIP, M_COUNTER, M_WAIT, M_COUNT };
enum { L_MAGIC, L_ITEMS, L_EQUIP, L_WEAPON, L_ARMOUR };

static iso_map_t s_map;
static fighter_t s_f[ISO_UNITS];
static int s_cc, s_cr;
static state_t s_st, s_after_busy;
static int s_sel;
static ui_menu_t s_menu, s_list, s_battle;
static int s_list_kind, s_act, s_act_what;
static int s_list_ids[UI_ITEMS];               // what each list line stands for
static int s_tlist[ISO_UNITS], s_tn, s_ti;      // the units an action can go to, the one chosen
static uint8_t s_dist[MAXCELLS], s_from[MAXCELLS];
static uint32_t s_summary_t;
static char s_banner[32];
static uint32_t s_banner_until;
static struct { int unit; char text[16]; uint32_t t; uint32_t rgb; int dy; } s_pop[6];
static int s_enemy_i;
static uint32_t s_enemy_wait;
static bool s_leave, s_ended;
static bool s_cam_fixed;                        // the PC host holds the camera still
uint32_t summary_length(void);
void summary_draw(const ui_t *u, uint32_t t);

static int unit_index(const iso_unit_t *u) { return u ? (int)(u - s_map.units) : -1; }
static void banner(const char *s, uint32_t ms) { strlcpy(s_banner, s, sizeof(s_banner)); s_banner_until = g_t + ms; }
static void clear_marks(void) { memset(s_map.mark, 0, COLS * ROWS); }

static void popup_at(int unit, const char *text, uint32_t rgb, int dy)
{
    for (int i = 0; i < 6; i++)
        if (!s_pop[i].text[0] || g_t - s_pop[i].t > 900) {
            s_pop[i].unit = unit;
            strlcpy(s_pop[i].text, text, sizeof(s_pop[i].text));
            s_pop[i].t = g_t;
            s_pop[i].rgb = rgb;
            s_pop[i].dy = dy;
            return;
        }
}
static void popup(int unit, const char *text, uint32_t rgb) { popup_at(unit, text, rgb, 0); }

// ── Setting up ──

static fighter_t *add_unit(const char *art, bool walk_is_run, int c, int r, int dir)
{
    char wn[24], in[24];
    snprintf(wn, sizeof(wn), "%s.%s", art, walk_is_run ? "run" : "walk");
    snprintf(in, sizeof(in), "%s.idle", art);
    iso_unit_t *u = iso_add_unit(&s_map, fart_anim(&g_art, wn), fart_anim(&g_art, in), c, r, dir);
    if (!u) return NULL;
    iso_cell(&s_map, c, r)->obj = ISO_NONE;
    fighter_t *f = &s_f[unit_index(u)];
    memset(f, 0, sizeof(*f));
    return f;
}

// What a hero wears and holds, onto its unit.
static void dress(int i)
{
    iso_unit_t *u = &s_map.units[i];
    const fighter_t *f = &s_f[i];
    u->wpn = -1;
    u->wpn_shade = NULL;
    u->cover = 0;
    u->ramp = NULL;
    if (!f->hero) return;
    int rest = 0;
    const char *art = weapon_art(f->h->weapon, &rest, &u->wpn_shade);
    if (art) {
        char n[24];
        snprintf(n, sizeof(n), "%s.0", art);
        u->wpn = fart_image(&g_art, n);
        u->wpn_rest = rest;
    }
    u->ramp = armour_look(f->h->armour, &u->cover);
}

// A step: on the ground the battle is on, a jump when the height changes.
static void footstep(const iso_map_t *m, const iso_unit_t *u, int from_h, int to_h)
{
    if (from_h != to_h) snd_play(from_h < to_h ? S_JUMP : S_LANDING, 150);
    else snd_play(g_battle.theme == THEME_ROCKS ? S_STEP_ROCK : S_STEP_GRASS, 150);
}

bool battle_enter(void)
{
    const battle_def_t *b = &g_battle;
    if (!iso_init(&s_map, &g_art, b->cols, b->rows)) return false;
    uint8_t spots[3][2], foes[6][2];
    map_build(&s_map, b, spots, foes);
    memset(&g_result, 0, sizeof(g_result));
    int slot = 0;
    for (int i = 0; i < ROSTER && slot < PARTY_MAX; i++) {
        hero_t *h = &g_save.heroes[i];
        if (!h->in_party) continue;
        fighter_t *f = add_unit(g_classes[h->cls].art, false, spots[slot][0], spots[slot][1], FART_NE);
        if (!f) break;
        f->name = h->name;
        f->hero = true;
        f->h = h;
        f->hp = f->hp_max = hero_hp_max(h);
        f->mp = f->mp_max = hero_mp_max(h);
        f->slot = slot;
        dress(unit_index(&s_map.units[s_map.nunits - 1]));
        g_result.hero[slot] = i;
        slot++;
    }
    g_result.nparty = slot;
    for (int k = 0; k < b->n; k++) {
        foe_t foe;
        foe_make(b->foes[k].type, b->realm, b->foes[k].level, b->foes[k].caster, &foe);
        fighter_t *f = add_unit(foe.art, true, foes[k][0], foes[k][1], FART_SW);
        if (!f) break;
        iso_unit_t *u = &s_map.units[s_map.nunits - 1];
        char an[32];
        f->foe = foe;
        f->name = f->foe.name;
        f->type = b->foes[k].type;
        f->level = b->foes[k].level;
        f->hp = f->hp_max = foe.hp;
        f->range = foe.range;
        u->shade = foe.shade;
        u->zoom = (uint8_t)foe.zoom;
        snprintf(an, sizeof(an), "%s.bite", foe.art);
        f->attack = fart_anim(&g_art, an);
        snprintf(an, sizeof(an), "%s.death", foe.art);
        f->death = fart_anim(&g_art, an);
        snprintf(an, sizeof(an), "%s.hit", foe.art);
        f->hit = fart_anim(&g_art, an);
    }
    if (!iso_build_layer(&s_map)) return false;
    map_grade(&s_map, b->realm);
    memset(s_pop, 0, sizeof(s_pop));
    fx_reset();
    s_sel = -1;
    s_leave = s_ended = false;
    s_cc = s_map.units[0].c;
    s_cr = s_map.units[0].r;
    for (int i = 0; i < s_map.nunits; i++) {
        s_f[i].moved = s_f[i].acted = s_f[i].done = false;
    }
    s_st = ST_CHOOSE;
    banner("Sua vez", 1200);
    s_map.on_step = footstep;
    snd_play(S_ENCOUNTER, 200);
    return true;
}

void battle_leave(void) { iso_free(&s_map); }

// ── Rules ──

static int move_of(const fighter_t *f) { return f->hero ? g_classes[f->h->cls].move : f->foe.move; }
static int jump_of(const fighter_t *f) { return f->hero ? g_classes[f->h->cls].jump : f->foe.jump; }
static int element_of(const fighter_t *f) { return f->hero ? EL_NONE : f->foe.element; }

static int range_of(const fighter_t *f, int act, int what)
{
    if (act == ACT_ATTACK) return f->hero ? g_goods[f->h->weapon].extra : f->range > 1 ? f->range : 1;
    return act == ACT_SPELL ? g_spells[what].range : 4;            // items are thrown
}

static bool shoots(const fighter_t *f) { return f->hero ? weapon_is_bow(f->h->weapon) : f->range > 1; }

static int distance(int a, int b)
{
    return abs(s_map.units[a].c - s_map.units[b].c) + abs(s_map.units[a].r - s_map.units[b].r);
}

static int face_toward(const iso_unit_t *u, int c, int r)
{
    const int dc = c - u->c, dr = r - u->r;
    return abs(dc) >= abs(dr) ? (dc >= 0 ? FART_SE : FART_NW) : (dr >= 0 ? FART_SW : FART_NE);
}

// A creature down: its coins and XP for the party (the XP is shared out when the battle ends).
static void reward(int by, int target)
{
    const fighter_t *t = &s_f[target];
    if (t->hero || g_result.nkills >= 8) return;
    g_result.kills[g_result.nkills].type = (uint8_t)t->type;
    g_result.kills[g_result.nkills].level = (uint8_t)t->level;
    g_result.kills[g_result.nkills].coins = (uint16_t)t->foe.coins;
    g_result.kills[g_result.nkills].xp = (uint16_t)t->foe.xp;
    g_result.nkills++;
    g_result.coins += t->foe.coins;
}

static void hurt(int by, int target, int amount)
{
    char t[16];
    fighter_t *f = &s_f[target];
    if (amount < 0) {
        f->hp = f->hp - amount > f->hp_max ? f->hp_max : f->hp - amount;
        snprintf(t, sizeof(t), "+%d", -amount);
        popup(target, t, 0x80ff90);
        return;
    }
    f->hp -= amount;
    s_map.units[target].flash = 360;
    snprintf(t, sizeof(t), "%d", amount);
    popup(target, t, 0xffffff);
    if (f->hp <= 0) {
        f->hp = 0;
        f->down = true;
        snd_play(S_DEATH, 220);
        // Its own death if it has one (wolves, slimes), else it falls over; either way it stays down.
        if (f->death) iso_play(&s_map.units[target], f->death, true);
        else iso_fall(&s_map.units[target]);
        s_map.units[target].dim = true;
        s_map.units[target].still = true;                      // no more breathing: it lies still
        reward(by, target);
    } else if (f->hit) iso_play(&s_map.units[target], f->hit, false);
}

// ── Actions, played out: a pose, maybe something flying, then the effect at the moment of impact ──

enum { K_MELEE, K_BITE, K_SHOT, K_CAST, K_THROW };
typedef struct {
    int actor, target, act, what, kind, amount, mp;
    uint32_t t, impact, len;
    int proj;
    bool landed, counter, weak, resist, strong;
    int element;                     // the attack's (an elemental weapon, a spell, a realm's creature)
} action_t;
#define QUEUE 4
static action_t s_q[QUEUE];
static int s_nq;

// What an action would do to a unit: damage lo..hi (> 0) or healing (< 0), PM restored, and whether it hits a
// weakness (an element that beats the target's: the weapon's or the spell's addition counts twice).
typedef struct { int lo, hi, mp, element; bool weak, resist, strong; } effect_t;   // strong: the target resists it

// The weapon's (or spell's) addition against a target of element `target`: twice against the element it beats
// ("Fraco!" over the target), half against its own (the target "Resiste").
static void element_mod(effect_t *e, int target, int *lo, int *hi)
{
    if (!e->element || !target) return;
    if (el_beats(e->element) == target) {
        e->weak = true;
        *lo *= 2;
        *hi *= 2;
    } else if (e->element == target) {
        e->strong = true;
        *lo /= 2;
        *hi /= 2;
    }
}

static bool predict(int a, int act, int what, int b, effect_t *e)
{
    const fighter_t *f = &s_f[a];
    memset(e, 0, sizeof(*e));
    if (b < 0) return false;
    const bool revive = act == ACT_ITEM && g_goods[what].revive;
    if (s_f[b].down && !revive) return false;
    // No friendly fire: attacks and harmful spells only on the other side, healing and items only on one's own.
    const bool foe = s_f[b].hero != f->hero;
    const int guard = s_f[b].hero ? hero_defence(s_f[b].h) : s_f[b].foe.def;
    if (act == ACT_ATTACK) {
        if (!foe) return false;
        if (f->hero) {
            int wlo, whi;
            weapon_roll(f->h->weapon, &wlo, &whi);
            e->element = g_goods[f->h->weapon].element;
            element_mod(e, element_of(&s_f[b]), &wlo, &whi);
            e->lo = f->h->level + wlo - guard;
            e->hi = f->h->level + whi - guard;
        } else {
            e->element = f->foe.element;
            e->lo = f->foe.atk - guard;
            e->hi = e->lo + 1;
        }
    } else if (act == ACT_SPELL) {
        const spell_t *sp = &g_spells[what];
        if (sp->heal ? foe : !foe) return false;
        e->element = sp->element;
        int wlo, whi;
        spell_roll(f->h, what, &wlo, &whi);
        if (sp->heal) e->lo = e->hi = -(8 + 2 * f->h->level + wlo);
        else {
            element_mod(e, element_of(&s_f[b]), &wlo, &whi);
            e->lo = f->h->level + wlo - guard;
            e->hi = f->h->level + whi - guard;
        }
    } else {
        if (foe) return false;
        e->lo = e->hi = -g_goods[what].power;
        e->mp = g_goods[what].extra;
    }
    // An elemental armour: half the damage of its element.
    if (e->hi > 0 && s_f[b].hero && e->element && g_goods[s_f[b].h->armour].element == e->element) {
        e->resist = true;
        e->lo /= 2;
        e->hi /= 2;
    }
    if (e->hi > 0) {                                                // a hit does at least 1
        if (e->lo < 1) e->lo = 1;
        if (e->hi < 1) e->hi = 1;
    }
    return true;
}

static void impact(action_t *q);

// Starts an action of unit a on the unit at (c, r): false if it cannot go there.
static bool start_action(int a, int act, int what, int c, int r, bool counter)
{
    const int b = unit_index(iso_unit_at(&s_map, c, r));
    fighter_t *f = &s_f[a];
    iso_unit_t *u = &s_map.units[a];
    effect_t e;
    if (s_nq >= QUEUE || !predict(a, act, what, b, &e)) return false;
    if (!counter && abs(c - u->c) + abs(r - u->r) > range_of(f, act, what)) return false;
    action_t *q = &s_q[s_nq++];
    const int amount = e.lo + (e.hi > e.lo ? rand() % (e.hi - e.lo + 1) : 0);
    *q = (action_t){ .actor = a, .target = b, .act = act, .what = what, .amount = amount, .mp = e.mp, .proj = -1,
                     .counter = counter, .weak = e.weak, .resist = e.resist, .strong = e.strong, .element = e.element };
    if (b != a) u->dir = face_toward(u, c, r);
    if (act == ACT_ATTACK) {
        if (shoots(f)) {                                         // an archer: an arrow, a counter too
            q->kind = K_SHOT;
            q->impact = 0xFFFFFFFF;                              // when the arrow arrives
            q->len = 0xFFFFFFFF;
        } else if (!f->hero) {                                   // a bite: the animal's own animation, or a lunge
            q->kind = K_BITE;
            q->len = f->attack ? (uint32_t)f->attack->frames * f->attack->ms : 520;
            q->impact = q->len * 2 / 5;
            if (f->attack) iso_play(u, f->attack, false);
            snd_play(rand() & 1 ? S_BITE : S_CLAW, 220);
        } else {
            q->kind = K_MELEE;
            q->impact = 260;
            q->len = 460;
        }
    } else if (act == ACT_SPELL) {
        q->kind = K_CAST;
        f->mp -= g_spells[what].mp;
        q->impact = 780;
        q->len = 1250;
    } else {
        q->kind = K_THROW;
        g_save.bag[what]--;
        q->impact = 0xFFFFFFFF;
        q->len = 0xFFFFFFFF;
    }
    return true;
}

static void rest_pose(iso_unit_t *u)
{
    u->wpn_angle = ISO_POSE_REST;
    u->hand_dx = u->hand_dy = 0;
    u->lunge_x = u->lunge_y = 0;
}

// The point a projectile aims at: the target's chest.
static void chest(int i, float *x, float *y)
{
    int fx, fy;
    iso_feet(&s_map, &s_map.units[i], &fx, &fy);
    *x = fx;
    *y = fy - 16;
}

// An element's sparks: its burst on a hit, its kind of aura.
static void el_burst(int el, float x, float y, int n)
{
    if (el) fx_burst(x, y, el_565(el, false), el_565(el, true), n, 50);
}
static int el_aura_kind(int el) { return el == EL_FIRE ? FX_RISE : el == EL_WATER ? FX_DRIP : el == EL_EARTH ? FX_DUST : FX_SWIRL; }

static void play_action(action_t *q, uint32_t dt)
{
    iso_unit_t *u = &s_map.units[q->actor];
    q->t += dt;
    const bool right = u->dir == FART_SE || u->dir == FART_NE;
    int hx, hy;
    float tx, ty;
    chest(q->target, &tx, &ty);
    switch (q->kind) {
    case K_MELEE: {                                              // raise, swing through, step in, back
        const int up = right ? 125 : 55, down = right ? -35 : 215;
        if (q->t >= 150 && q->t - dt < 150) snd_play(S_SLASH, 220);
        if (q->element && q->t >= 120 && q->t < 320) {           // an elemental blade leaves sparks along the swing
            iso_hand(&s_map, u, &hx, &hy);
            fx_aura(hx, hy - 6, 6, el_565(q->element, false), el_565(q->element, true), el_aura_kind(q->element));
        }
        if (q->t < 150) u->wpn_angle = up;
        else if (q->t < 300) u->wpn_angle = up + (down - up) * (int)(q->t - 150) / 150;
        else u->wpn_angle = down;
        const int push = q->t < 180 ? 0 : q->t < 300 ? (int)(q->t - 180) / 20 : q->t < 380 ? 6 : 0;
        const iso_unit_t *v = &s_map.units[q->target];
        u->lunge_x = (int8_t)((v->c - u->c) - (v->r - u->r) > 0 ? push : -push);
        u->lunge_y = (int8_t)(((v->c - u->c) + (v->r - u->r)) > 0 ? push / 2 : -push / 2);
        break;
    }
    case K_SHOT: {
        // Aim, loose, wait for it to land. An archer's arrow takes its bow's element: fire burns along its flight,
        // water flies as an ice needle, earth makes a rock spike burst under the target, air is a tempest arrow (wind
        // wound round it, after-images) ending in a whirlwind. A creature casts its realm's element: a fireball, an
        // ice needle, a rock spike, lightning.
        iso_hand(&s_map, u, &hx, &hy);
        const bool cast = !s_f[q->actor].hero;
        const int el = q->element;
        int feet_x, feet_y;
        iso_feet(&s_map, &s_map.units[q->target], &feet_x, &feet_y);
        const float fx_ = feet_x, fy_ = feet_y;
        if (cast) {
            hy -= 8 * (u->zoom > 1);
            if (q->t < 300) fx_aura(hx, hy, 8, el_565(el, false), el_565(el, true), el_aura_kind(el));
        } else u->wpn_angle = (int)(atan2f(-(ty - hy), tx - hx) * 57.2958f) + 45;
        if (q->t >= 300 && q->proj < 0 && !q->landed) {
            static const snd_t sound[EL_COUNT] = { S_WHOOSH, S_FIRE, S_WATER, S_EARTH, S_THUNDER };
            snd_play(cast ? sound[el] : S_WHOOSH, 220);
            if (cast && (el == EL_EARTH || el == EL_AIR)) {     // no flight: it strikes where the target stands
                if (el == EL_EARTH) fx_spike(fx_, fy_);
                else fx_bolt(tx, ty + 10);
                q->landed = true;
                q->impact = q->t + 140;
                q->len = q->t + 520;
            } else {
                const float d = sqrtf((tx - hx) * (tx - hx) + (ty - hy) * (ty - hy));
                const bool needle = el == EL_WATER;
                const float arc = needle ? d * 0.04f : cast ? d * 0.1f : d * 0.18f;
                const uint32_t ms = 150 + (uint32_t)(d * (needle ? 1.4f : cast ? 3.0f : el == EL_AIR ? 3.2f : 2.2f));
                q->proj = fx_shoot(cast && !needle ? -1 : fart_image(&g_art, "arrow.0"), true, false, hx, hy, tx, ty, arc, ms);
                if (needle) fx_style(q->proj, FX_NEEDLE);
                if (el == EL_AIR) fx_style(q->proj, FX_TEMPEST);
                if (el && el != EL_AIR) fx_trail(q->proj, el_565(el, false), el_565(el, true));
            }
        }
        if (q->proj >= 0 && fx_landed(q->proj) && !q->landed) {
            q->landed = true;
            q->impact = q->t;
            q->len = q->t + 200;
            if (el == EL_EARTH) {                                // the arrow strikes; the ground answers
                fx_spike(fx_, fy_);
                q->len = q->t + 520;
            } else if (el == EL_AIR) {
                fx_vortex(fx_, fy_);
                q->len = q->t + 900;
            } else if (el == EL_WATER) fx_burst(tx, ty, 0xffff, 0x9f1f, 18, 50);   // the needle shatters
            else el_burst(el, tx, ty, cast ? 26 : 14);
        }
        break;
    }
    case K_BITE:                                                 // no bite of its own: a lunge at the target
        if (!s_f[q->actor].attack) {
            const iso_unit_t *v = &s_map.units[q->target];
            const int push = q->t < 140 ? 0 : q->t < 220 ? (int)(q->t - 140) / 10 : q->t < 320 ? 8 - (int)(q->t - 220) / 13 : 0;
            u->lunge_x = (int8_t)((v->c - u->c) - (v->r - u->r) > 0 ? push : -push);
            u->lunge_y = (int8_t)(((v->c - u->c) + (v->r - u->r)) > 0 ? push / 2 : -push / 2);
        }
        break;
    case K_CAST: {                                               // the weapon held high, sparks gathering
        u->hand_dy = -14;
        u->hand_dx = right ? -2 : 2;
        u->wpn_angle = 90;
        iso_hand(&s_map, u, &hx, &hy);
        const int el = g_spells[q->what].element;
        if (q->t < 700 && (q->t / 30) % 2) fx_glow(hx, hy - 12, el ? el_565(el, false) : 0x87f0);
        if (q->t >= 650 && q->t - dt < 650) {
            static const snd_t sound[EL_COUNT] = { S_HEAL, S_FIRE, S_WATER, S_EARTH, S_THUNDER };
            snd_play(sound[el], 230);
            if (el == EL_FIRE) {
                fx_burst(tx, ty + 8, 0xfa20, 0xfd60, 30, 70);
                fx_burst(tx, ty, 0xf800, 0xffe0, 16, 40);
            } else if (el == EL_WATER) {                         // a wave: a splash, then drops falling
                fx_burst(tx, ty + 6, 0x3c9e, 0xaf9f, 34, 60);
                for (int k = 0; k < 14; k++) fx_aura(tx, ty, 14, 0x5d1f, 0xffff, FX_DRIP);
            } else if (el == EL_EARTH) {                         // rocks bursting out of the ground
                fx_burst(tx, ty + 10, 0x8a85, 0xd5ac, 30, 75);
                fx_burst(tx, ty + 12, 0x52aa, 0xa514, 14, 30);
            } else if (el == EL_AIR) fx_bolt(tx, ty + 10);
            else fx_sparkle(tx, ty + 10, 0x87f0, 26);
        }
        break;
    }
    case K_THROW: {                                              // up into the sky, down onto the target
        u->hand_dy = q->t < 260 ? -(int)q->t / 20 : -12;
        iso_hand(&s_map, u, &hx, &hy);
        if (q->t >= 260 && q->proj < 0 && !q->landed) {
            char n[16];
            snprintf(n, sizeof(n), "icon.%d", g_goods[q->what].icon);
            const float d = sqrtf((tx - hx) * (tx - hx) + (ty - hy) * (ty - hy));
            q->proj = fx_shoot(fart_image(&g_art, n), false, true, hx, hy - 6, tx, ty - 4, 60 + d * 0.3f, 520 + (uint32_t)(d * 1.5f));
            snd_play(S_USE_ITEM, 200);
        }
        if (q->proj >= 0 && fx_landed(q->proj) && !q->landed) {
            q->landed = true;
            q->impact = q->t;
            q->len = q->t + 450;
            static const uint16_t c[3] = { 0xf9a6, 0x5f3f, 0x87f0 };
            const int k = !strcmp(g_goods[q->what].name, "Éter") ? 1 : !strcmp(g_goods[q->what].name, "Elixir") ? 2 : 0;
            fx_sparkle(tx, ty + 10, c[k], 22);
            snd_play(g_goods[q->what].revive && s_f[q->target].down ? S_REVIVE : S_HEAL, 210);
        }
        break;
    }
    default:
        break;
    }
    if (q->t >= q->impact && q->impact != 0xFFFFFFFF) {
        impact(q);
        q->impact = 0xFFFFFFFF;
    }
}

static void impact(action_t *q)
{
    fighter_t *t = &s_f[q->target];
    if (q->act == ACT_ITEM && t->down && g_goods[q->what].revive) {
        t->down = false;
        s_map.units[q->target].once = NULL;
        s_map.units[q->target].dim = false;
        s_map.units[q->target].still = false;
        iso_rise(&s_map.units[q->target]);
    }
    if (q->act == ACT_ATTACK) {
        snd_play(S_HIT, 230);
        if (q->kind == K_MELEE && q->element) {
            float x, y;
            chest(q->target, &x, &y);
            el_burst(q->element, x, y, 18);
        }
    }
    if (q->weak) popup_at(q->target, "Fraco!", 0xffb040, -11);
    if (q->resist || q->strong) popup_at(q->target, "Resiste", 0x9fd8ff, -11);
    if (q->amount) hurt(q->actor, q->target, q->amount);
    if (q->mp) {
        char m[16];
        t->mp = t->mp + q->mp > t->mp_max ? t->mp_max : t->mp + q->mp;
        snprintf(m, sizeof(m), "+%d PM", q->mp);
        popup(q->target, m, 0x9fd8ff);
    }
    // A counter, once: attacked while ready and still standing. In close combat it strikes back; an archer shoots
    // back at any attacker its bow reaches, near or far.
    iso_unit_t *tu = &s_map.units[q->target];
    const int d = distance(q->actor, q->target);
    const bool reach = shoots(t) ? d >= 1 && d <= range_of(t, ACT_ATTACK, 0) : d == 1;
    if (q->act == ACT_ATTACK && !q->counter && tu->counter && !t->down && !s_f[q->actor].down && reach) {
        tu->counter = false;
        popup(q->target, "Reage!", 0xffe060);
        start_action(q->target, ACT_ATTACK, 0, s_map.units[q->actor].c, s_map.units[q->actor].r, true);
    }
}

static bool actions_update(uint32_t dt)
{
    if (!s_nq) return false;
    action_t *q = &s_q[0];
    play_action(q, dt);
    if (q->t >= q->len) {
        rest_pose(&s_map.units[q->actor]);
        memmove(&s_q[0], &s_q[1], (size_t)(s_nq - 1) * sizeof(action_t));
        s_nq--;
    }
    return true;
}

static bool side_left(bool heroes)
{
    for (int i = 0; i < s_map.nunits; i++)
        if (s_f[i].hero == heroes && !s_f[i].down) return true;
    return false;
}

static void finish(bool won)
{
    g_result.won = won;
    if (!won) g_result.coins /= 2;                    // a defeat keeps half of what was won
    g_save.coins += g_result.coins;
    // The next battle of the fase opens; the fase's last one clears it (the next fase opens, the shop grows).
    uint8_t *done = &g_save.won[g_battle.realm][g_battle.tier];
    if (won && g_battle.index == *done) {
        (*done)++;
        g_result.cleared = *done >= g_tier_len[g_battle.tier];
    }
    // Every hero in the party learns from every creature down; less from creatures below its level (a quarter less
    // a level, down to a quarter), and half after a defeat.
    for (int s = 0; s < g_result.nparty; s++) {
        hero_t *h = &g_save.heroes[g_result.hero[s]];
        int xp = g_result.xp[s];                                  // (0, or what a dev command added)
        for (int k = 0; k < g_result.nkills; k++) {
            const int gap = h->level - g_result.kills[k].level, quarters = gap <= 0 ? 4 : gap >= 3 ? 1 : 4 - gap;
            xp += g_result.kills[k].xp * quarters / 4;
        }
        g_result.xp[s] = won ? xp : xp / 2;
        g_result.level_before[s] = h->level;
        g_result.xp_before[s] = h->xp;
        g_result.levelled[s] = hero_gain(h, g_result.xp[s]);
    }
    game_store();
    s_st = ST_OVER;
    clear_marks();
    banner(won ? "Vitória!" : "Derrota", 100000);
}

// ── Heroes ──

static void cursor_to(int i)
{
    s_cc = s_map.units[i].c;
    s_cr = s_map.units[i].r;
}

static int next_hero(int step)
{
    const int here = unit_index(iso_unit_at(&s_map, s_cc, s_cr));
    for (int k = 1; k <= s_map.nunits; k++) {
        const int i = ((here < 0 ? (step > 0 ? -1 : 0) : here) + step * k + s_map.nunits * 2) % s_map.nunits;
        if (s_f[i].hero && !s_f[i].done && !s_f[i].down) return i;
    }
    return -1;
}

static void open_menu(int i)
{
    static const char *items[M_COUNT] = { "Mover", "Atacar", "Magia", "Itens", "Equipar", "Reagir", "Esperar" };
    s_sel = i;
    cursor_to(i);
    memset(&s_menu, 0, sizeof(s_menu));
    s_menu.title = s_f[i].name;
    s_menu.n = M_COUNT;
    for (int k = 0; k < M_COUNT; k++) s_menu.items[k] = items[k];
    s_menu.notes[M_COUNTER] = "1x";
    const fighter_t *f = &s_f[i];
    s_menu.off = (f->moved ? 1u << M_MOVE : 0) |
                 (f->acted ? (1u << M_ATTACK) | (1u << M_MAGIC) | (1u << M_ITEMS) | (1u << M_COUNTER) : 0);
    if (!g_classes[f->h->cls].spells) s_menu.off |= 1u << M_MAGIC;
    while (s_menu.off & (1u << s_menu.sel)) s_menu.sel++;
    s_st = ST_MENU;
}

static void open_list(int kind)
{
    static char notes[UI_ITEMS][16];
    const fighter_t *f = &s_f[s_sel];
    memset(&s_list, 0, sizeof(s_list));
    s_list_kind = kind;
    int n = 0;
    if (kind == L_MAGIC) {
        // Magic's level is the weapon's: the title says it; each spell in its element's colour, its cost and what the
        // weapon adds (gold when the staff is of the spell's element and doubles it).
        static char title[24];
        snprintf(title, sizeof(title), "Magia Nv %d", g_goods[f->h->weapon].level);
        s_list.title = title;
        for (int k = 0; k < g_nspells && n < UI_ITEMS; k++) {
            if (!(g_classes[f->h->cls].spells & (1u << k))) continue;
            int lo, hi;
            spell_roll(f->h, k, &lo, &hi);
            s_list.items[n] = g_spells[k].name;
            s_list.colors[n] = g_spells[k].element ? g_elements[g_spells[k].element].rgb : 0x80ff90;
            snprintf(notes[n], sizeof(notes[n]), "%dPM +%d~%d", g_spells[k].mp, lo, hi);
            s_list.notes[n] = notes[n];
            s_list.note_colors[n] = spell_attuned(f->h, k) ? 0xffd040 : 0x9fd8ff;
            if (f->mp < g_spells[k].mp) s_list.off |= 1u << n;
            s_list_ids[n++] = k;
        }
    } else if (kind == L_ITEMS) {
        s_list.title = "Itens";
        for (int k = 0; k < g_ngoods && n < UI_ITEMS; k++) {
            if (g_goods[k].kind != KIND_ITEM) continue;
            s_list.items[n] = g_goods[k].name;
            snprintf(notes[n], sizeof(notes[n]), "x%d", g_save.bag[k]);
            s_list.notes[n] = notes[n];
            if (!g_save.bag[k]) s_list.off |= 1u << n;
            s_list_ids[n++] = k;
        }
    } else if (kind == L_EQUIP) {
        s_list.title = "Equipar";
        s_list.items[0] = "Arma";
        s_list.notes[0] = g_goods[f->h->weapon].name;               // (names fit: the list is beside the menu)
        s_list.items[1] = "Armadura";
        s_list.notes[1] = g_goods[f->h->armour].name;
        n = 2;
    } else {
        // The class's own gear in the bag; what needs a higher level greyed, with the level it needs.
        const int want = kind == L_WEAPON ? KIND_WEAPON : KIND_ARMOUR;
        const int worn = kind == L_WEAPON ? f->h->weapon : f->h->armour;
        s_list.title = kind == L_WEAPON ? "Arma" : "Armadura";
        for (int k = 0; k < g_ngoods && n < UI_ITEMS; k++) {
            int need;
            if (g_goods[k].kind != want || (k != worn && !g_save.bag[k])) continue;
            const bool ok = hero_can_use(f->h, k, &need);
            if (!ok && f->h->level >= need) continue;               // another class's
            s_list.items[n] = g_goods[k].name;
            if (k == worn) snprintf(notes[n], sizeof(notes[n]), "usando");
            else if (!ok) snprintf(notes[n], sizeof(notes[n]), "Nv %d", need);
            else snprintf(notes[n], sizeof(notes[n]), "x%d", g_save.bag[k]);
            s_list.notes[n] = notes[n];
            if (!ok) s_list.off |= 1u << n;
            if (k == worn) s_list.sel = n;
            s_list_ids[n++] = k;
        }
    }
    s_list.n = n;
    while (s_list.sel < s_list.n - 1 && (s_list.off & (1u << s_list.sel))) s_list.sel++;
    s_st = ST_LIST;
}

static void wear(hero_t *h, int goods)
{
    uint8_t *slot = g_goods[goods].kind == KIND_WEAPON ? &h->weapon : &h->armour;
    if (*slot == goods) return;
    g_save.bag[*slot]++;
    g_save.bag[goods]--;
    *slot = goods;
}

static void start_move(void)
{
    const fighter_t *f = &s_f[s_sel];
    iso_reach(&s_map, &s_map.units[s_sel], move_of(f), jump_of(f), s_dist, s_from);
    clear_marks();
    for (int i = 0; i < COLS * ROWS; i++)
        if (s_dist[i] != 255 && s_dist[i] > 0) s_map.mark[i] = ISO_MARK_MOVE;
    s_st = ST_MOVE;
}

static void start_target(int act, int what)
{
    const iso_unit_t *u = &s_map.units[s_sel];
    const int range = range_of(&s_f[s_sel], act, what);
    s_act = act;
    s_act_what = what;
    clear_marks();
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            const int d = abs(c - u->c) + abs(r - u->r);
            if (d <= range && (d > 0 || act != ACT_ATTACK) && iso_cell(&s_map, c, r)->h > 0)
                s_map.mark[r * COLS + c] = ISO_MARK_ACT;
        }
    // The units it can go to, nearest first: Left and Right cycle through them (no tile cursor: nothing aims at a
    // tile yet).
    s_tn = 0;
    for (int i = 0; i < s_map.nunits; i++) {
        effect_t e;
        const iso_unit_t *v = &s_map.units[i];
        if (!s_map.mark[v->r * COLS + v->c] || !predict(s_sel, act, what, i, &e)) continue;
        if (act == ACT_ATTACK && i == s_sel) continue;
        int k = s_tn++;
        while (k > 0 && distance(s_sel, s_tlist[k - 1]) > distance(s_sel, i)) {
            s_tlist[k] = s_tlist[k - 1];
            k--;
        }
        s_tlist[k] = i;
    }
    if (!s_tn) {
        clear_marks();
        banner("Sem alvo ao alcance", 1100);
        return;                                                   // stays in its menu
    }
    s_ti = 0;
    cursor_to(s_tlist[0]);
    s_st = ST_TARGET;
}

static void enemy_phase(void)
{
    s_sel = -1;
    clear_marks();
    s_st = ST_ENEMY;
    s_enemy_i = 0;
    s_enemy_wait = 0;
    banner("Vez do inimigo", 1200);
}

static void hero_done(void)
{
    s_f[s_sel].done = true;
    s_map.units[s_sel].dim = true;
    s_sel = -1;
    clear_marks();
    const int n = next_hero(1);
    if (n >= 0) {
        cursor_to(n);
        s_st = ST_CHOOSE;
    } else enemy_phase();
}

static void new_round(void)
{
    for (int i = 0; i < s_map.nunits; i++) {
        if (s_f[i].hero) s_map.units[i].counter = false;
        s_f[i].moved = s_f[i].acted = s_f[i].done = false;
        if (!s_f[i].down) s_map.units[i].dim = false;
    }
    const int n = next_hero(1);
    if (n >= 0) cursor_to(n);
    s_st = ST_CHOOSE;
    banner("Sua vez", 1200);
}

// ── Enemies: bite the weakest hero within reach this turn; otherwise close in on the nearest ──

static int s_enemy_target = -1;

static void enemy_step(void)
{
    while (s_enemy_i < s_map.nunits && (s_f[s_enemy_i].hero || s_f[s_enemy_i].down)) s_enemy_i++;
    if (s_enemy_i >= s_map.nunits) {
        new_round();
        return;
    }
    const int e = s_enemy_i++;
    iso_unit_t *u = &s_map.units[e];
    cursor_to(e);
    // One that strikes from afar (an Elemental) stays where it is when a hero is already in its reach.
    if (s_f[e].range > 1)
        for (int i = 0; i < s_map.nunits; i++)
            if (s_f[i].hero && !s_f[i].down && distance(e, i) <= s_f[e].range) {
                s_enemy_target = i;
                s_f[e].acted = false;
                s_after_busy = ST_ENEMY;
                s_st = ST_BUSY;
                return;
            }
    iso_reach(&s_map, u, move_of(&s_f[e]), jump_of(&s_f[e]), s_dist, s_from);
    // The hero worth biting: reachable (a free cell next to it within the move), the lowest PV; its cell to bite from.
    int best = -1, best_hp = 1 << 30, from = -1;
    for (int i = 0; i < s_map.nunits; i++) {
        if (!s_f[i].hero || s_f[i].down) continue;
        static const int8_t dc[4] = { 1, 0, -1, 0 }, dr[4] = { 0, 1, 0, -1 };
        for (int d = 0; d < 4; d++) {
            const int c = s_map.units[i].c + dc[d], r = s_map.units[i].r + dr[d];
            if (!iso_inside(&s_map, c, r) || s_dist[r * COLS + c] == 255) continue;
            const int score = s_f[i].hp * 16 + s_dist[r * COLS + c];     // weakest first, then nearest
            if (score < best_hp) {
                best_hp = score;
                best = i;
                from = r * COLS + c;
            }
        }
    }
    if (best < 0) {                          // nobody in reach: walk toward the nearest hero
        int near = -1, nd = 1 << 30;
        for (int i = 0; i < s_map.nunits; i++)
            if (s_f[i].hero && !s_f[i].down && distance(e, i) < nd) {
                nd = distance(e, i);
                near = i;
            }
        if (near >= 0) {
            int bd = nd;
            for (int i = 0; i < COLS * ROWS; i++) {
                if (s_dist[i] == 255) continue;
                const int d = abs(i % COLS - s_map.units[near].c) + abs(i / COLS - s_map.units[near].r);
                if (d < bd) {
                    bd = d;
                    from = i;
                }
            }
        }
    }
    s_enemy_target = best;
    uint8_t path[ISO_PATH];
    const int n = from >= 0 ? iso_path_to(&s_map, u, s_dist, s_from, from % COLS, from / COLS, path) : 0;
    if (n) iso_walk(u, path, n);
    s_f[e].acted = false;
    s_after_busy = ST_ENEMY;
    s_st = ST_BUSY;
}

// ── Input ──

static void move_cursor(int dir)
{
    static const int8_t dc[4] = { 1, 0, -1, 0 }, dr[4] = { 0, 1, 0, -1 };
    const int c = s_cc + dc[dir], r = s_cr + dr[dir];
    if (iso_inside(&s_map, c, r)) {
        s_cc = c;
        s_cr = r;
    }
}

static void input(void)
{
    const int dir = g_keys.dir;
    switch (s_st) {
    case ST_CHOOSE: {
        if (dir >= 0) move_cursor(dir);
        const int n = key(FH1_KEY_X) ? next_hero(1) : key(FH1_KEY_Y) ? next_hero(-1) : -1;
        if (n >= 0) cursor_to(n);
        if (key(FH1_KEY_A)) {
            const int i = unit_index(iso_unit_at(&s_map, s_cc, s_cr));
            if (i >= 0 && s_f[i].hero && !s_f[i].done && !s_f[i].down) open_menu(i);
        }
        if (key(FH1_KEY_START) || key(FH1_KEY_MENU)) {
            memset(&s_battle, 0, sizeof(s_battle));
            s_battle.title = "Batalha";
            s_battle.items[0] = "Encerrar turno";
            s_battle.items[1] = g_info ? "Esconder tempos" : "Mostrar tempos";
            s_battle.items[2] = "Desistir";
            s_battle.items[3] = "Sair do jogo";
            s_battle.notes[3] = "sem salvar";
            s_battle.n = 4;
            s_st = ST_BATTLE;
            snd_play(S_PAUSE, 200);
        }
        break;
    }
    case ST_BATTLE: {
        const int r = ui_menu_key(&s_battle, menu_key());
        if (r == 0) {
            for (int i = 0; i < s_map.nunits; i++)
                if (s_f[i].hero) s_f[i].done = true;
            enemy_phase();
        } else if (r == 1) {
            g_info = !g_info;
            s_st = ST_CHOOSE;
        } else if (r == 2) finish(false);
        else if (r == 3) s_leave = true;
        else if (r == UI_BACK) {
            s_st = ST_CHOOSE;
            snd_play(S_UNPAUSE, 200);
        }
        break;
    }
    case ST_MENU: {
        if (key(FH1_KEY_Y) && s_menu.sel != M_WAIT) {        // the west button (Square, X on an Xbox pad): to Esperar
            s_menu.sel = M_WAIT;
            snd_play(S_HOVER, 160);
        }
        const int r = ui_menu_key(&s_menu, menu_key());
        if (r == UI_BACK) {
            s_st = ST_CHOOSE;
            s_sel = -1;
        } else if (r == M_MOVE) start_move();
        else if (r == M_ATTACK) start_target(ACT_ATTACK, 0);
        else if (r == M_MAGIC) open_list(L_MAGIC);
        else if (r == M_ITEMS) open_list(L_ITEMS);
        else if (r == M_EQUIP) open_list(L_EQUIP);
        else if (r == M_COUNTER) {
            s_map.units[s_sel].counter = true;
            s_f[s_sel].acted = true;
            hero_done();
        } else if (r == M_WAIT) hero_done();
        break;
    }
    case ST_LIST: {
        const int r = ui_menu_key(&s_list, menu_key());
        if (r == UI_BACK) {
            if (s_list_kind == L_WEAPON || s_list_kind == L_ARMOUR) open_list(L_EQUIP);
            else open_menu(s_sel);
        } else if (r >= 0) {
            if (s_list_kind == L_MAGIC) start_target(ACT_SPELL, s_list_ids[r]);
            else if (s_list_kind == L_ITEMS) start_target(ACT_ITEM, s_list_ids[r]);
            else if (s_list_kind == L_EQUIP) open_list(r == 0 ? L_WEAPON : L_ARMOUR);
            else {
                fighter_t *f = &s_f[s_sel];
                snd_play(S_EQUIP, 220);
                wear(f->h, s_list_ids[r]);
                dress(s_sel);
                f->mp_max = hero_mp_max(f->h);
                if (f->mp > f->mp_max) f->mp = f->mp_max;
                open_list(L_EQUIP);
            }
        }
        break;
    }
    case ST_MOVE:
        if (dir >= 0) move_cursor(dir);
        if (key(FH1_KEY_A)) {
            uint8_t path[ISO_PATH];
            const int n = iso_path_to(&s_map, &s_map.units[s_sel], s_dist, s_from, s_cc, s_cr, path);
            if (n) {
                iso_walk(&s_map.units[s_sel], path, n);
                s_f[s_sel].moved = true;
                clear_marks();
                s_after_busy = ST_MENU;
                s_st = ST_BUSY;
            }
        }
        if (key(FH1_KEY_B)) {
            clear_marks();
            open_menu(s_sel);
        }
        break;
    case ST_TARGET:
        if (dir == FART_SE || dir == FART_SW) s_ti = (s_ti + 1) % s_tn;      // Right / Down: the next
        if (dir == FART_NW || dir == FART_NE) s_ti = (s_ti + s_tn - 1) % s_tn;   // Left / Up: the previous
        if (dir >= 0) cursor_to(s_tlist[s_ti]);
        if (key(FH1_KEY_A) && start_action(s_sel, s_act, s_act_what, s_cc, s_cr, false)) {
            s_f[s_sel].acted = true;
            clear_marks();
            s_after_busy = ST_MENU;
            s_st = ST_BUSY;
        }
        if (key(FH1_KEY_B)) {
            clear_marks();
            if (s_act == ACT_ATTACK) open_menu(s_sel);
            else open_list(s_act == ACT_SPELL ? L_MAGIC : L_ITEMS);
        }
        break;
    case ST_OVER:
        if (key(FH1_KEY_A) && g_t > s_banner_until - 100000 + 800) {
            s_st = ST_SUMMARY;
            s_summary_t = 0;
        }
        break;
    case ST_SUMMARY:
        if (key(FH1_KEY_A)) {
            if (s_summary_t < summary_length()) s_summary_t = summary_length();   // A skips the counting
            else s_ended = true;
        }
        break;
    default:
        break;
    }
}

static void update(uint32_t dt)
{
    if (s_st == ST_BUSY) {
        bool busy = s_nq > 0 || fx_busy();
        for (int i = 0; i < s_map.nunits; i++) busy |= iso_busy(&s_map.units[i]);
        if (busy) return;
        if (!side_left(false)) {
            finish(true);
            return;
        }
        if (!side_left(true)) {
            finish(false);
            return;
        }
        if (s_after_busy == ST_MENU) {
            const fighter_t *f = &s_f[s_sel];
            if (f->moved && f->acted) hero_done();
            else open_menu(s_sel);
        } else if (s_after_busy == ST_ENEMY) {
            const int e = s_enemy_i - 1;
            if (!s_f[e].acted) {
                s_f[e].acted = true;
                // Its target if in reach now, else the weakest hero in reach.
                const int reach = s_f[e].range > 1 ? s_f[e].range : 1;
                int t = s_enemy_target >= 0 && !s_f[s_enemy_target].down && distance(e, s_enemy_target) <= reach ? s_enemy_target : -1;
                for (int i = 0; i < s_map.nunits; i++)
                    if (t < 0 && s_f[i].hero && !s_f[i].down && distance(e, i) <= reach) t = i;
                    else if (t >= 0 && t != s_enemy_target && s_f[i].hero && !s_f[i].down && distance(e, i) <= reach &&
                             s_f[i].hp < s_f[t].hp) t = i;
                if (t >= 0) start_action(e, ACT_ATTACK, 0, s_map.units[t].c, s_map.units[t].r, false);
                return;
            }
            s_st = ST_ENEMY;
            s_enemy_wait = 0;
        }
    } else if (s_st == ST_ENEMY && g_t > s_banner_until) {
        s_enemy_wait += dt;
        if (s_enemy_wait > 300) enemy_step();
    }
}

// ── Drawing ──

// Decoration: an Elemental's aura, the glow of elemental weapons.
static void ambient(uint32_t dt)
{
    static uint32_t acc;
    acc += dt;
    const bool tick = acc >= 90;
    if (tick) acc = 0;
    for (int i = 0; i < s_map.nunits; i++) {
        const fighter_t *f = &s_f[i];
        if (f->down) continue;
        int x, y;
        if (!f->hero && f->type == E_ELEMENTAL) {
            const int el = f->foe.element;
            iso_feet(&s_map, &s_map.units[i], &x, &y);
            for (int k = 0; k < 2; k++) fx_aura(x, y - 20, 22, el_565(el, false), el_565(el, true), el_aura_kind(el));
        } else if (!f->hero && f->foe.caster && tick) {         // a caster: its element flickering over it
            const int el = f->foe.element;
            iso_feet(&s_map, &s_map.units[i], &x, &y);
            fx_aura(x, y - 26, 5, el_565(el, false), el_565(el, true), el_aura_kind(el));
        } else if (f->hero && tick && g_goods[f->h->weapon].element) {
            const int el = g_goods[f->h->weapon].element;
            iso_hand(&s_map, &s_map.units[i], &x, &y);
            fx_aura(x, y - 5, 5, el_565(el, false), el_565(el, true), el_aura_kind(el));
        }
    }
}

static void draw_panel(const ui_t *u, int i, int x, int y)
{
    if (i < 0 || i >= s_map.nunits || (uintptr_t)s_f[i].name < 0x10000u) {
        static uint32_t told;
        if (g_t - told > 2000) {
            told = g_t;
            fh1_log("panel: unit %d of %d, name %p, sel %d, st %d, enemy %d, cursor %d,%d", i, s_map.nunits,
                    (i >= 0 && i < ISO_UNITS) ? (const void *)s_f[i].name : NULL, s_sel, s_st, s_enemy_i, s_cc, s_cr);
        }
        return;
    }
    const fighter_t *f = &s_f[i];
#ifdef PC_HOST
    if (!f->name || (uintptr_t)f->name < 4096) { printf("BAD NAME unit %d of %d, state %d, enemy_i %d\n", i, s_map.nunits, s_st, s_enemy_i); battle_debug(); abort(); }
#endif
    char line[40];
    ui_box(u, x, y, 140, f->hero ? 50 : 40);
    ui_text(u, x + 6, y + 5, f->name, f->hero ? 0x9fd8ff : f->foe.boss ? 0xffd060 : 0xff9c8a);
    if (f->hero) {
        if (f->down) ui_text(u, x + 92, y + 5, "caído", 0xff7060);
        else if (f->done) ui_text(u, x + 92, y + 5, "pronto", 0x909cb0);
        else {
            snprintf(line, sizeof(line), "Nv %d", f->h->level);
            ui_text(u, x + 92, y + 5, line, 0xc8c8d0);
        }
    } else {                                                     // its element (its colour) and level
        const int el = f->foe.element;
        ui_text(u, x + 6, y + 28, g_elements[el].name, g_elements[el].rgb);
        snprintf(line, sizeof(line), "Nv %d", f->level);
        ui_text(u, x + 6 + 8 * 6, y + 28, line, 0xc8c8d0);
        if (f->down) ui_text(u, x + 92, y + 28, "caído", 0xff7060);
        else if (f->foe.boss) ui_text(u, x + 92, y + 28, "chefe", 0xffd060);
        else if (f->foe.caster) ui_text(u, x + 100, y + 28, "mago", 0xd8a0ff);
    }
    snprintf(line, sizeof(line), "PV %3d", f->hp);
    ui_text(u, x + 6, y + 17, line, 0xf0f0f0);
    ui_bar(u, x + 62, y + 19, 70, f->hp, f->hp_max, 0x58d068);
    if (f->hero) {
        snprintf(line, sizeof(line), "PM %3d", f->mp);
        ui_text(u, x + 6, y + 27, line, 0xf0f0f0);
        ui_bar(u, x + 62, y + 29, 70, f->mp, f->mp_max, 0x5890f0);
        const int w = f->h->weapon;
        ui_text(u, x + 6, y + 38, g_goods[w].name, g_goods[w].element ? g_elements[g_goods[w].element].rgb : 0xc8c8d0);
    }
}

// A unit's card with an action's effect shown on its bars: the PV it would lose blinking red, the PV or PM it
// would gain blinking green, the number over the bar; "-" when the action cannot go there.
static void draw_preview(const ui_t *u, int i, int x, int y, bool ok, const effect_t *e)
{
    draw_panel(u, i, x, y);
    const fighter_t *f = &s_f[i];
    char l[16];
    if (!ok) {
        ui_text(u, x + 104, y + 17, "-", 0x8a96b0);
        return;
    }
    const bool blink = (g_t / 220) % 2;
    const int amount = e->hi, mp = e->mp;                          // the bar shows the most it can do
    if (amount) {
        const int after = f->hp - amount < 0 ? 0 : f->hp - amount > f->hp_max ? f->hp_max : f->hp - amount;
        const int lo = after < f->hp ? after : f->hp, hi = after < f->hp ? f->hp : after;
        const int x0 = x + 62 + lo * 70 / f->hp_max, x1 = x + 62 + hi * 70 / f->hp_max;
        const uint16_t c = amount > 0 ? (blink ? 0xf800 : 0xfd20) : (blink ? 0x87f0 : 0xbff7);
        for (int yy = y + 20; yy < y + 22; yy++)
            for (int xx = x0; xx < x1 + (x1 == x0); xx++)
                if (xx >= 0 && xx < u->w && yy < u->h) u->px[yy * u->w + xx] = c;
        if (amount > 0 && e->lo != e->hi) snprintf(l, sizeof(l), "%d~%d", e->lo, e->hi);
        else if (amount > 999 || amount < -999) snprintf(l, sizeof(l), "tudo");
        else snprintf(l, sizeof(l), amount > 0 ? "-%d" : "+%d", amount > 0 ? amount : -amount);
        ui_text(u, x + 136 - 8 * (int)strlen(l), y + 5, l, amount > 0 ? 0xff7060 : 0x80ff90);
        if (amount > 0 && f->hp - e->lo <= 0) ui_text(u, x + 62, y + 5, "cai", 0xff7060);
        else if (amount > 0 && after == 0) ui_text(u, x + 62, y + 5, "cai?", 0xff7060);
        if (e->weak) ui_text(u, x + 92, y + 28, "Fraco!", blink ? 0xffb040 : 0xffe080);
        else if (e->strong) ui_text(u, x + 84, y + 28, "Resiste", 0x9fd8ff);
    }
    if (mp && f->hero) {
        const int after = f->mp + mp > f->mp_max ? f->mp_max : f->mp + mp;
        const int x0 = x + 62 + f->mp * 70 / (f->mp_max ? f->mp_max : 1), x1 = x + 62 + after * 70 / (f->mp_max ? f->mp_max : 1);
        for (int yy = y + 30; yy < y + 32; yy++)
            for (int xx = x0; xx < x1; xx++)
                if (xx >= 0 && xx < u->w && yy < u->h) u->px[yy * u->w + xx] = blink ? 0x5f3f : 0xbfff;
        snprintf(l, sizeof(l), "+%d PM", mp);
        ui_text(u, x + 92, y + 27, l, 0x9fd8ff);
    }
}

screen_t battle_frame(const ui_t *u, uint32_t dt)
{
    input();
    if (s_ended) {
        battle_leave();
        return SCR_PARTY;                                         // back to the party (the shop is there)
    }
    if (s_leave) return SCR_QUIT;                                 // the loop's closing frees the battle
    if (s_st == ST_SUMMARY) s_summary_t += dt;
    actions_update(dt);
    ambient(dt);
    fx_update(dt);
    update(dt);
    iso_update(&s_map, dt);
    int cx, cy;
    iso_world(&s_map, s_cc, s_cr, &cx, &cy);
    if (!s_cam_fixed) iso_follow(&s_map, cx, cy, u->w, u->h, dt);
    const bool show_cursor = s_st == ST_CHOOSE || s_st == ST_MOVE;
    iso_draw(&s_map, u->px, u->w, u->h, show_cursor ? s_cc : -1, s_cr, g_t);
    fx_draw(&g_art, u->px, u->w, u->h, s_map.camx, s_map.camy);

    for (int i = 0; i < 6; i++) {
        if (!s_pop[i].text[0]) continue;
        const uint32_t age = g_t - s_pop[i].t;
        if (age > 900) {
            s_pop[i].text[0] = 0;
            continue;
        }
        int x, y;
        iso_world(&s_map, s_map.units[s_pop[i].unit].c, s_map.units[s_pop[i].unit].r, &x, &y);
        ui_text(u, x - s_map.camx - 6, y - s_map.camy - 44 + s_pop[i].dy - (int)age / 30, s_pop[i].text, s_pop[i].rgb);
    }
    if (s_st == ST_TARGET) {                                    // the chosen target: a pointer over it
        int x, y;
        iso_feet(&s_map, &s_map.units[s_tlist[s_ti]], &x, &y);
        ui_text(u, x - s_map.camx - 3, y - s_map.camy - 54 - (int)((g_t / 120) % 3), "v", 0xff6050);
        char l[16];
        snprintf(l, sizeof(l), "%d/%d", s_ti + 1, s_tn);
        ui_text(u, u->w - 40, 30, l, 0xc8c8d0);
    }
    if (s_sel >= 0 && s_st != ST_BUSY) {
        int x, y;
        iso_world(&s_map, s_map.units[s_sel].c, s_map.units[s_sel].r, &x, &y);
        ui_text(u, x - s_map.camx - 3, y - s_map.camy - 52 - (int)((g_t / 150) % 3), "v", 0xffe060);
    }
    const int shown = s_sel >= 0 ? s_sel : unit_index(iso_unit_at(&s_map, s_cc, s_cr));
    if (shown >= 0 && s_st != ST_OVER && s_st != ST_SUMMARY) draw_panel(u, shown, 4, u->h - 54);
    if (s_st == ST_TARGET) {                                    // the target's card, with what the action would do
        const int b = unit_index(iso_unit_at(&s_map, s_cc, s_cr));
        effect_t e;
        if (b >= 0 && b != s_sel) {
            const bool ok = predict(s_sel, s_act, s_act_what, b, &e) && s_map.mark[s_cr * COLS + s_cc];
            draw_preview(u, b, u->w - 144, u->h - 54, ok, &e);
        } else if (b == s_sel) {
            if (predict(s_sel, s_act, s_act_what, b, &e)) draw_preview(u, b, 4, u->h - 54, true, &e);
        }
    }
    if (s_st == ST_MENU || s_st == ST_LIST) {
        int bw, bh;
        ui_menu_size(&s_menu, &bw, &bh);
        ui_menu_draw(u, &s_menu, u->w - bw - 4, 4, g_t);
        if (s_st == ST_LIST) {
            int lw, lh;
            ui_menu_size(&s_list, &lw, &lh);
            ui_menu_draw(u, &s_list, u->w - bw - lw - 2, 22, g_t);
        }
    } else if (s_st == ST_BATTLE) {
        int bw, bh;
        ui_menu_size(&s_battle, &bw, &bh);
        ui_menu_draw(u, &s_battle, (u->w - bw) / 2, (u->h - bh) / 2, g_t);
    } else if (s_st == ST_MOVE || s_st == ST_TARGET) {
        ui_box(u, u->w - 172, 4, 168, 22);
        if (s_st == ST_MOVE) ui_hints(u, u->w - 166, 11, "Mover para onde? {A}", 0xf0f0f0);
        else ui_hints(u, u->w - 166, 11, "Alvo: {LR} {A} {B}", 0xf0f0f0);
    }
    if (s_banner[0] && (g_t < s_banner_until || s_st == ST_OVER) && s_st != ST_SUMMARY) ui_banner(u, s_banner, 0xf8d870);
    if (s_st == ST_OVER) {
        ui_box(u, (u->w - 120) / 2, u->h / 2, 120, 22);
        ui_hints(u, (u->w - 120) / 2 + 12, u->h / 2 + 7, "{A} continuar", 0xf0f0f0);
    }
    if (s_st == ST_SUMMARY) summary_draw(u, s_summary_t);
    return SCR_BATTLE;
}

#ifdef PC_HOST
void battle_debug(void)
{
    printf("state %d sel %d enemy_i %d |", s_st, s_sel, s_enemy_i);
    for (int i = 0; i < s_map.nunits; i++)
        printf(" %s(%d,%d) hp%d%s%s", s_f[i].name, s_map.units[i].c, s_map.units[i].r, s_f[i].hp, s_f[i].down ? " down" : "",
               iso_busy(&s_map.units[i]) ? " busy" : "");
    printf("\n");
}
#endif

// Development hooks (dev_command in isotest.c: the PC host's scripts and the FH1's console): stage a scene, play
// an action, end the battle, to look at it without playing to it.
void battle_host_place(int i, int c, int r, int dir)
{
    if (i < 0 || i >= s_map.nunits) return;
    s_map.units[i].c = s_map.units[i].fc = c;
    s_map.units[i].r = s_map.units[i].fr = r;
    s_map.units[i].dir = dir;
    s_cc = c;
    s_cr = r;
}
void battle_host_act(int a, int act, int what, int b)
{
    clear_marks();
    if (start_action(a, act, what, s_map.units[b].c, s_map.units[b].r, false)) {
        s_sel = s_f[a].hero ? a : -1;
        s_after_busy = s_f[a].hero ? ST_MENU : ST_ENEMY;
        s_st = ST_BUSY;
    }
}
void battle_host_counter(int i) { s_map.units[i].counter = true; }
void battle_host_hp(int i, int hp) { if (i >= 0 && i < s_map.nunits) s_f[i].hp = hp; }
void battle_host_log(void)
{
    char l[200];
    int n = snprintf(l, sizeof(l), "state %d:", s_st);
    for (int i = 0; i < s_map.nunits && n < (int)sizeof(l) - 24; i++)
        n += snprintf(l + n, sizeof(l) - n, " %d %s(%d,%d) %d%s", i, s_f[i].name, s_map.units[i].c, s_map.units[i].r, s_f[i].hp,
                      s_f[i].down ? "x" : "");
    fh1_log("%s", l);
}
// Ends the battle as if fought: won (every enemy knocked out by the first hero) or lost (every hero down).
void battle_host_end(bool won)
{
    for (int i = 0; i < s_map.nunits; i++)
        if (s_f[i].hero != won && !s_f[i].down) hurt(won ? 0 : i, i, 999);
    s_after_busy = ST_MENU;
    s_st = ST_BUSY;
}
void battle_host_ranged(int i, int range) { s_f[i].range = range; }
// XP the battle will give each hero on top of what its creatures give (shared out at the end, per creature down).
void battle_host_xp(int slot, int xp) { if (slot >= 0 && slot < PARTY_MAX) g_result.xp[slot] += xp; }
void battle_host_cam(int x, int y)
{
    s_cam_fixed = true;
    s_map.camx = x;
    s_map.camy = y;
}
void battle_host_target(int a, int act, int what, int c, int r)
{
    s_sel = a;
    start_target(act, what);                     // the real list of targets, then the one at (c, r)
    for (int k = 0; k < s_tn; k++)
        if (s_map.units[s_tlist[k]].c == c && s_map.units[s_tlist[k]].r == r) {
            s_ti = k;
            cursor_to(s_tlist[k]);
        }
}
void battle_host_gear(int i, int weapon, int armour)
{
    if (!s_f[i].hero) return;
    s_f[i].h->weapon = weapon;
    s_f[i].h->armour = armour;
    dress(i);
}

