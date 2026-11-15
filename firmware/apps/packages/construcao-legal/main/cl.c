// Construção Legal: explore a world on foot and build in it, alone or with friends. Each user has their own world
// (their Mundo, saved with their data). The start menu plays it, finds the worlds friends are playing on the same
// network (they host theirs while they play it) or leaves. In a friend's world everything is theirs: their FH1 runs
// the game for everyone (SDK network play), so what a guest builds there is kept in the owner's world, on the
// owner's FH1. Each player has a colour, and each sees the world turned their own way, follows their own way round
// it, and sees the others where they are.
//
// A player walks freely in eight directions (the D-pad; B runs), blocked by water, trees, stones and buildings; the
// camera follows; things in front of them are cut away round them. X switches build mode: the cell they face is
// the target (green: they may, red: they may not), Y picks the tool, A uses it (a swing of the hammer). They build
// on clean grass outside the village, cut trees and clear stones outside it, and remove what was built. A into a
// building goes in. Start opens a menu.
#include "fh1_sdk.h"
#include "fart.h"
#include "ui.h"
#include "fx.h"
#include "world.h"
#include "interior.h"
#include "viewturn.h"
#include "netrep.h"
#include "sfxbank.h"
#include "portal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KEYS (FH1_KEY_MENU + 1)
#define WALK 70.0f                   // pixels a second on screen
#define RUN 130.0f
#define NP FH1_NET_PLAYERS           // players in a world, its owner's included (the owner is player 0)

static fart_t s_art;
static world_t s_world;
static const fart_anim_t *s_idle, *s_walk, *s_run, *s_swing;
static float s_camx, s_camy;
static uint32_t s_t, s_frame_ms, s_max_ms, s_max_shown, s_max_t;
static bool s_info, s_quit;
static ui_menu_t s_menu;
static bool s_paused;                // the owner's own menu open (their world goes on for the others)
static uint32_t s_down, s_edge;      // this FH1's keys (FH1_KEY_*): now, pressed this frame
static uint32_t s_pad_prev, s_pad_edge;

// ── The game's state: a world and the players in it ──

enum { MODE_START, MODE_FIND, MODE_OWN, MODE_GUEST };
static int s_mode = MODE_START;
static int s_me;                     // the player this FH1 is (0 in its own world; its seat in a friend's)

enum { T_ROAD, T_HOUSE, T_SMITH, T_MILL, T_WALL, T_TOWER, T_STAIR, T_TREE, T_FIELD, T_REMOVE };
// The tools (Y: the next), each with its variants (L1 / R1): a house's roof and walls, the
// smithy's and the mill's roof, a castle wall plain, with a window or a gate, a tower's style and colour, the tree's
// kind, wheat or corn. The bar shows each tool's chosen variant.
static const struct { const char *name; uint8_t kind, n; } s_tools[] = {
    { "Estrada", T_ROAD, 1 }, { "Casa", T_HOUSE, 8 }, { "Ferreiro", T_SMITH, 4 }, { "Moinho", T_MILL, 4 },
    { "Muralha", T_WALL, 3 }, { "Torre", T_TOWER, 36 }, { "Escada", T_STAIR, 1 }, { "Árvore", T_TREE, 4 },
    { "Plantação", T_FIELD, 2 },
    { "Remover", T_REMOVE, 1 },
};
#define TOOLS ((int)(sizeof(s_tools) / sizeof(s_tools[0])))
#define VARS 36

typedef struct {
    bool on;                         // in the world now
    float u, v, z;                   // where (cells; z: how high, pixels)
    int oct;                         // facing: an eighth on their screen
    bool moving, running;
    uint32_t t;                      // their animation's time
    int view;                        // how they see the world turned (0..3)
    bool build;                      // build mode
    int tool, rot;
    uint8_t var[TOOLS];              // each tool's chosen variant
    struct { bool on; int c, r, tool, var, rot; uint32_t t, len; } act;   // a swing under way, its change at the end
    // Holding A builds on: their facing (so the target's place beside them) stays as it was when A went down, and they
    // walk along one map axis only (the first they move on), kept on the middle of their row, so what they build
    // lines up. c, r: the cell built last (once each).
    struct { bool on; int oct, axis, c, r; float line; } lock;
    uint32_t refuse_t;               // "can't" shown until then
    uint32_t down, edge, pad, pad_edge;   // their keys (FH1_KEY_*) and controller buttons, now and pressed
    bool asking;                     // a guest's question "back to your world?" open (the owner's FH1 mirrors it)
    // Inside a building (the engine's interior.h)
    bool inside;
    interior_t *in;
    building_t bld;
    int door_c, door_r;              // the cell they went in by
    uint32_t step_t;                 // footsteps (this FH1's player)
    // Coming in through a portal: hidden while it opens, then walking out of it to (ex, ev) by themselves.
    bool arriving, hidden;
    uint32_t arrive_t;
    float ex, ev;
    uint8_t gate_axis;               // the way out of it: 0 along the world's columns (+u), 1 along its rows (+v)
    char name[24];
} player_t;
static player_t s_pl[NP];

static bool key(const player_t *p, int k) { return p->edge & (1u << k); }
static bool held(const player_t *p, int k) { return p->down & (1u << k); }

// ── Sounds (the rpg-sfx pool, through the engine's sfxbank.h) ──

enum { SN_GRASS, SN_ROCK, SN_WOOD, SN_SWING, SN_BUILD, SN_BREAK, SN_DOOR, SN_TOOL, SN_PICK, SN_NO, SN_OK, SN_BACK,
       SN_JOIN, SN_LEAVE, SN_TURN, SN_COUNT };
static const char *const s_snd_names[SN_COUNT] = {
    "step_grass_03", "step_rock_02", "step_wood_03", "attack_03", "hit_03", "earth_02", "use_item_01", "equip_10",
    "hover_01", "denied_03", "confirm_03", "decline_09", "encounter_02", "flee_02", "wind_01",
};
static sfxbank_t s_snd;
static void snd(int s, int volume) { sfxbank_play(&s_snd, s, (uint8_t)volume); }
static void ui_snd(int what)
{
    static const int8_t map[] = { [UI_SND_MOVE] = SN_PICK, [UI_SND_OK] = SN_OK, [UI_SND_BACK] = SN_BACK, [UI_SND_NO] = SN_NO };
    snd(map[what], what == UI_SND_MOVE ? 120 : 170);
}
// A sound of something done at (u, v): full beside this FH1's player, fainter away, none beyond a screen or two.
static void snd_at(int s, float u, float v, int volume)
{
    const player_t *me = &s_pl[s_me];
    if (me->inside) return;
    const float d = fabsf(u - me->u) + fabsf(v - me->v);
    if (d > 14) return;
    snd(s, d < 3 ? volume : (int)(volume * (14 - d) / 11));
}

// ── Notes: a line on screen for a while (who came, who went) ──

static char s_note[48];
static uint32_t s_note_t;
static void note(const char *t)
{
    snprintf(s_note, sizeof(s_note), "%s", t);
    s_note_t = s_t + 2600;
}

// ── Portals (the engine's portal.h, in a player's colour): a friend comes in through one and walks out of it, and
// one flashes open where a friend leaves. The pack's frame (two stones) stands round the whirling vortex. ──

#define GATES NP
#define ARRIVE_HIDE 520              // ms: the portal opening before the newcomer shows
#define ARRIVE_STAY 1500             // ms it stays open after that, then closes
#define GATE_FADE 300                // ms its stones take to show and to go
typedef struct {
    bool on;
    portal_t p;
    float u, v;                      // its cell's middle
    int axis;                        // the way out of it (as gate_axis)
    uint32_t t, close_at;
} gate_t;
static gate_t s_gate[GATES];
static portal_t s_portal_tpl;        // the vortex as loaded (copied for each portal)
static bool s_portal_ok;
static int s_portal_frame = -1, s_portal_front = -1, s_portal_glow = -1;

static void gate_open(float u, float v, int axis, int who, uint32_t stay)
{
    if (!s_portal_ok) return;
    gate_t *g = &s_gate[0];
    for (int i = 0; i < GATES; i++)
        if (!s_gate[i].on) {
            g = &s_gate[i];
            break;
        }
    g->on = true;
    g->p = s_portal_tpl;
    portal_colour(&g->p, ui_player_colour(who));
    portal_glow(&g->p, s_portal_glow);
    portal_open(&g->p);
    g->u = u, g->v = v, g->axis = axis;
    g->t = 0;
    g->close_at = stay;
}

static void gates_update(uint32_t dt)
{
    for (int i = 0; i < GATES; i++) {
        gate_t *g = &s_gate[i];
        if (!g->on) continue;
        g->t += dt;
        portal_update(&g->p, dt);
        if (g->t >= g->close_at && g->p.state == PORTAL_OPEN) portal_close(&g->p);
        if (g->t >= g->close_at && !portal_shown(&g->p) && g->t >= g->close_at + g->p.open_ms + GATE_FADE) g->on = false;
    }
}

// A portal at its place on this FH1's screen: its stones (fading in and out), the vortex, the near stone over it.
// Turned so its way out runs as it does in the world: the pack's picture opens toward the lower left; mirrored, the
// lower right.
static void gate_draw(const gate_t *g, uint16_t *px, int w, int h, int camx, int camy)
{
    float su, sv, tu, tv;
    world_to_screen(g->u, g->v, &su, &sv);
    world_to_screen(g->u + (g->axis ? 0 : 1), g->v + (g->axis ? 1 : 0), &tu, &tv);
    const bool mirror = fabsf(tu - su) > fabsf(tv - sv);
    float fx, fy;
    world_px(g->u, g->v, &fx, &fy);
    const int x = (int)floorf(fx + 0.5f) - camx, y = (int)floorf(fy + 0.5f) - camy;
    if (x < -60 || x > w + 60 || y < -10 || y > h + 70) return;
    const uint32_t end = g->close_at + g->p.open_ms;
    int level = (int)(g->t * 16 / GATE_FADE);
    if (g->t > end) level = 16 - (int)((g->t - end) * 16 / GATE_FADE);
    level = level < 0 ? 0 : level > 16 ? 16 : level;
    const fart_clip_t clip = { 0, 0, w, h };
    if (s_portal_frame >= 0) fart_draw_dither(px, w, &clip, &s_art.img[s_portal_frame], x, y, mirror, level);
    portal_draw(&g->p, &s_art, px, w, &clip, x, y, mirror);
    if (s_portal_front >= 0) fart_draw_dither(px, w, &clip, &s_art.img[s_portal_front], x, y, mirror, level);
}

// ── Build mode ──

static const char *const s_roof[4] = { "blue", "green", "red", "wood" };
static const char *const s_roof_pt[4] = { "azul", "verde", "vermelho", "de madeira" };
static const char *const s_tower_pt[4] = { "azul", "verde", "vermelha", "de madeira" };
static int s_icon[TOOLS][VARS];      // their pictures in the bar (-1: none; remove draws a red cross)

static void load_icons(void)
{
    char n[32];
    for (int t = 0; t < TOOLS; t++)
        for (int v = 0; v < VARS; v++) {
            n[0] = 0;
            switch (s_tools[t].kind) {
            case T_ROAD: snprintf(n, sizeof(n), "road.10"); break;     // a piece of road seen through the slot
            case T_HOUSE: snprintf(n, sizeof(n), "ic.house.%s.%s", v >> 2 ? "white" : "wood", s_roof[v & 3]); break;
            case T_SMITH: snprintf(n, sizeof(n), "ic.smith.%s", s_roof[v & 3]); break;
            case T_MILL: snprintf(n, sizeof(n), "ic.mill.%s", s_roof[v & 3]); break;
            case T_WALL: snprintf(n, sizeof(n), "ic.wall.%s", v == WALL_GATE ? "gate" : v == WALL_WINDOW ? "win" : "plain"); break;
            case T_TOWER: snprintf(n, sizeof(n), "ic.tower.%s.%d", s_roof[v / 9 % 4], 1 + v % 9); break;
            case T_STAIR: snprintf(n, sizeof(n), "ic.stair"); break;
            case T_TREE: snprintf(n, sizeof(n), "ic.tree.%d", 1 + v % 4); break;
            case T_FIELD: snprintf(n, sizeof(n), v & 1 ? "g.corn.1" : "g.wheat.1"); break;
            }
            s_icon[t][v] = n[0] ? fart_image(&s_art, n) : -1;
        }
}

// The chosen tool's name with its variant ("Casa branca, azul").
static void tool_name(const player_t *p, char *out, size_t n)
{
    const int t = p->tool, v = p->var[t];
    switch (s_tools[t].kind) {
    case T_HOUSE: snprintf(out, n, "Casa %s, %s", v >> 2 ? "branca" : "de madeira", s_roof_pt[v & 3]); break;
    case T_SMITH: case T_MILL: snprintf(out, n, "%s, %s", s_tools[t].name, s_roof_pt[v & 3]); break;
    case T_WALL: snprintf(out, n, v == WALL_GATE ? "Portão" : v == WALL_WINDOW ? "Muralha com janela" : "Muralha"); break;
    case T_TOWER: snprintf(out, n, "Torre %d, %s", 1 + v % 9, s_tower_pt[v / 9 % 4]); break;
    case T_TREE: snprintf(out, n, "Árvore %d", 1 + v); break;
    case T_FIELD: snprintf(out, n, v ? "Milho" : "Trigo"); break;
    default: snprintf(out, n, "%s", s_tools[t].name); break;
    }
}

static int s_since_save;
// The cell in front of a player for each eighth they face (E, SE, S, SW, W, NW, N, NE on their screen).
static const int8_t s_fc[8] = { 1, 1, 1, 0, -1, -1, -1, 0 }, s_fr[8] = { -1, 0, 1, 1, 1, 0, -1, -1 };

// (Their facing is on their screen: the cell in front of them is found there, then turned back to the world's.)
static void target(const player_t *p, int *c, int *r)
{
    float su, sv, u, v;
    world_to_screen_v(p->view, p->u, p->v, &su, &sv);
    world_from_screen_v(p->view, floorf(su) + s_fc[p->oct] + 0.5f, floorf(sv) + s_fr[p->oct] + 0.5f, &u, &v);
    *c = (int)floorf(u);
    *r = (int)floorf(v);
}

// Does a player's body (their feet's square) reach into the cell? Nothing solid goes up on anyone.
#define FEET 0.13f                   // a player's feet's half width (cells)
static bool on_someone(int c, int r)
{
    const float m = FEET;
    for (int i = 0; i < NP; i++) {
        const player_t *p = &s_pl[i];
        if (p->on && !p->inside && p->u + m > c && p->u - m < c + 1 && p->v + m > r && p->v - m < r + 1) return true;
    }
    return false;
}

// Can a wall or a tower go on top of the wall there (built, a level free, no tower on it yet)?
// (On a keep, only a tower: a wall on it would undo it.)
static bool stacks_on(int c, int r, int kind, int v)
{
    const cell_t *x = world_cell(&s_world, c, r);
    return x->obj == O_WALL && (x->flags & F_BUILT) && !STACK_TOWER(x) && STACK_LEVELS(x) < STACK_MAX &&
           (kind == T_TOWER || (kind == T_WALL && v != WALL_GATE && !world_is_keep(&s_world, c, r)));
}

// Stairs at (c, r): the side (0 N, 1 E, 2 S, 3 W) of a wall they climb to (a walkway: one level, nothing on it), the
// one straight ahead of the player first; -1 if none.
static int stair_way(const player_t *p, int c, int r)
{
    static const int8_t dc[4] = { 0, 1, 0, -1 }, dr[4] = { -1, 0, 1, 0 };
    const int hc = (int)floorf(p->u), hr = (int)floorf(p->v);
    const int ahead = c > hc ? 1 : c < hc ? 3 : r > hr ? 2 : 0;
    for (int i = 0; i < 4; i++) {
        const int d = (ahead + i) & 3;
        const float h = world_height(&s_world, c + dc[d] + 0.5f, r + dr[d] + 0.5f, WALK_H);
        if (world_inside(c + dc[d], r + dr[d]) && world_cell(&s_world, c + dc[d], r + dr[d])->obj == O_WALL && h == WALK_H)
            return d;
    }
    return -1;
}

// An upper storey can go on a built 2 x 2 house there (one at most).
static bool storey_on(int c, int r)
{
    const cell_t *x = world_cell(&s_world, c, r);
    return x->obj == O_HOUSE && (x->flags & F_BUILT) && !(x->var & UPPER) && world_is_big(&s_world, c, r);
}

static bool allowed(const player_t *p, int c, int r, int tool)
{
    if (!world_inside(c, r)) return false;
    const cell_t *x = world_cell(&s_world, c, r);
    const int kind = s_tools[tool].kind;
    if (x->flags & F_TOWN) return false;                          // the village is nobody's to change
    if (p->z > 1) return false;                                  // building is done standing on the ground
    if (kind == T_REMOVE)
        return (x->flags & F_BUILT) ? (x->obj != O_NONE && x->obj != O_TUFT) || x->ground != G_GRASS
                                    : x->obj == O_TREE || x->obj == O_STONE;
    if (stacks_on(c, r, kind, p->var[tool])) return true;
    if (kind == T_STAIR && stair_way(p, c, r) < 0) return false;  // stairs: against a wall's walkway
    if (kind == T_HOUSE && storey_on(c, r)) return true;
    if (kind == T_WALL && p->var[tool] == WALL_GATE && x->ground == G_ROAD && (x->obj == O_NONE || x->obj == O_TUFT))
        return world_gate_way(&s_world, c, r) >= 0 && !on_someone(c, r);   // a gate across a road, the road through it
    if (x->ground != G_GRASS || (x->obj != O_NONE && x->obj != O_TUFT)) return false;   // a clean field only
    return kind == T_ROAD || kind == T_FIELD || !on_someone(c, r);
}

// The world as the owner keeps it: their changes in their own data (fh1_state_save: player 1's user's folder).
static void save_world(void)
{
    if (s_mode != MODE_OWN) return;                               // a guest keeps nothing of a friend's world
    static struct { uint32_t magic; uint16_t n; edit_t e[EDITS_MAX]; } s;
    s.magic = 0x434c5731u;                                        // "CLW1"
    s.n = (uint16_t)s_world.nedits;
    memcpy(s.e, s_world.edits, sizeof(edit_t) * s.n);
    fh1_state_save("mundo", &s, 6 + sizeof(edit_t) * s.n);
    s_since_save = 0;
}

static void load_world(void)
{
    static struct { uint32_t magic; uint16_t n; edit_t e[EDITS_MAX]; } s;
    const int got = fh1_state_load("mundo", &s, sizeof(s));
    if (got < 6 || s.magic != 0x434c5731u || s.n > EDITS_MAX || got < (int)(6 + sizeof(edit_t) * s.n)) return;
    world_apply(&s_world, s.e, s.n);
}

// What a tool makes of a cell: its ground and variant, the thing on it and its variant (built: a player's).
typedef struct { int ground, var, obj, ovar; bool built; } plan_t;

static plan_t plan(const player_t *p, int c, int r, int tool, int v, int rot)
{
    const cell_t *x = world_cell(&s_world, c, r);
    // A building's door toward the player (the side they stand on, along the cell's main axis to them), turned by rot.
    const int dc = (int)floorf(p->u) - c, dr = (int)floorf(p->v) - r;
    const int face = ((abs(dc) >= abs(dr) ? (dc > 0 ? 1 : 3) : (dr > 0 ? 2 : 0)) + rot) % 4;
    switch (s_tools[tool].kind) {
    case T_ROAD: return (plan_t){ G_ROAD, 0, O_NONE, 0, true };
    case T_FIELD: return (plan_t){ v ? G_CORN : G_WHEAT, c, O_NONE, 0, true };          // in rows
    case T_TREE: return (plan_t){ G_GRASS, 0, O_TREE, v, true };
    case T_HOUSE:
        if (storey_on(c, r)) return (plan_t){ x->ground, x->var | UPPER, O_HOUSE, x->ovar, true };   // on its 2 x 2
        return (plan_t){ G_GRASS, 0, O_HOUSE, face | (v & 3) << 2 | (v >> 2) << 4, true };
    case T_SMITH: return (plan_t){ G_GRASS, 0, O_SMITH, face | (v & 3) << 2, true };
    case T_MILL: return (plan_t){ G_GRASS, 0, O_MILL, (v & 3) | (face <= 1) << 2, true };   // its sails' side
    case T_WALL:
        if (stacks_on(c, r, T_WALL, v))                                  // a level on top (windows if it has them)
            return (plan_t){ x->ground, ((x->var & 0x7c) + STACK_UP(x) + 1) | (v == WALL_WINDOW) << 7, O_WALL, x->ovar, true };
        if (x->ground == G_ROAD && v == WALL_GATE)                 // a gate across the road, the road kept
            return (plan_t){ G_ROAD, 0, O_WALL, WALL_GATE | (world_gate_way(&s_world, c, r) == 1) << 2, true };
        return (plan_t){ G_GRASS, 0, O_WALL, v | (face & 1) << 2, true };                      // alone: its face to them
    case T_STAIR: return (plan_t){ G_GRASS, 0, O_STAIR, stair_way(p, c, r) < 0 ? 0 : stair_way(p, c, r), true };
    case T_TOWER:
        if (stacks_on(c, r, T_TOWER, v))                                 // on top of the wall
            return (plan_t){ x->ground, (x->var & 0x83) | 4 | (v % 9) << 3, O_WALL, (x->ovar & 0xe7) | (v / 9 % 4) << 3, true };
        return (plan_t){ G_GRASS, 0, O_TOWER, v % 9 | (v / 9) << 4, true };
    case T_REMOVE:
        if (x->obj == O_HOUSE && (x->var & UPPER) && world_is_big(&s_world, c, r))   // an upper storey first
            return (plan_t){ x->ground, x->var & ~UPPER, O_HOUSE, x->ovar, true };
        if (x->obj == O_WALL && STACK_LEVELS(x) > 1)               // the top level first
            return (plan_t){ x->ground, STACK_TOWER(x) ? x->var & 0x83 : x->var - 1, O_WALL, x->ovar, true };
        if (x->ground == G_ROAD && x->obj != O_NONE && x->obj != O_TUFT)   // a gate off the road: the road stays
            return (plan_t){ G_ROAD, 0, O_NONE, 0, true };
        return (plan_t){ G_GRASS, 0, O_NONE, 0, false };           // back to plain grass, nobody's
    default: return (plan_t){ G_GRASS, 0, O_NONE, 0, false };    // back to plain grass, nobody's
    }
}

// The change a finished swing makes.
static void apply(const player_t *p, int c, int r, int tool, int v, int rot)
{
    const plan_t pl = plan(p, c, r, tool, v, rot);
    if (pl.obj != world_cell(&s_world, c, r)->obj) world_unjoin(&s_world, c, r);   // a piece taken off: the rest keep theirs
    world_set(&s_world, c, r, pl.ground, pl.var, pl.obj, pl.ovar, pl.built);
    world_merge(&s_world, c, r);
    world_spread(&s_world, c, r);                                  // a tower on a keep, a storey: on all four cells
    float fx, fy;
    world_px(c + 0.5f, r + 0.5f, &fx, &fy);
    fx_burst(fx, fy - 4, 0xce59, 0x9cd3, 26, 45);                   // dust
    snd_at(s_tools[tool].kind == T_REMOVE ? SN_BREAK : SN_BUILD, c + 0.5f, r + 0.5f, 190);
    if (++s_since_save >= 8) save_world();
}

// Starts a swing at the player's target if the tool may be used there (false: not).
static bool start_swing(player_t *p)
{
    int c, r;
    target(p, &c, &r);
    if (!allowed(p, c, r, p->tool)) return false;
    p->act.on = true;
    p->act.c = p->lock.c = c;
    p->act.r = p->lock.r = r;
    p->act.tool = p->tool;
    p->act.var = p->var[p->tool];
    p->act.rot = p->rot;
    p->act.t = 0;
    p->act.len = s_swing ? (uint32_t)s_swing->frames * s_swing->ms : 500;
    if (p == &s_pl[s_me]) snd(SN_SWING, 110);
    return true;
}

static void build_input(player_t *p)
{
    const bool mine = p == &s_pl[s_me];
    if (key(p, FH1_KEY_Y)) {
        p->tool = (p->tool + 1) % TOOLS;
        if (mine) snd(SN_TOOL, 140);
    }
    const uint32_t pe = p->pad_edge;
    const int n = s_tools[p->tool].n;
    if (pe & FH1_PAD_BTN_R1) p->var[p->tool] = (uint8_t)((p->var[p->tool] + 1) % n);
    if (pe & FH1_PAD_BTN_L1) p->var[p->tool] = (uint8_t)((p->var[p->tool] + n - 1) % n);
    if ((pe & FH1_PAD_BTN_R2) || key(p, FH1_KEY_SELECT)) p->rot = (p->rot + 1) % 4;
    if (pe & FH1_PAD_BTN_L2) p->rot = (p->rot + 3) % 4;
    if (mine && ((pe & (FH1_PAD_BTN_R1 | FH1_PAD_BTN_L1 | FH1_PAD_BTN_R2 | FH1_PAD_BTN_L2)) || key(p, FH1_KEY_SELECT)))
        snd(SN_PICK, 110);
    if (key(p, FH1_KEY_A)) {
        p->lock.c = p->lock.r = -1;
        p->lock.on = true;
        p->lock.oct = p->oct;
        p->lock.axis = -1;
        if (!p->act.on && !start_swing(p)) {
            p->refuse_t = s_t + 900;
            if (mine) snd(SN_NO, 150);
        }
    } else if (p->lock.on && held(p, FH1_KEY_A) && !p->act.on) {
        int c, r;
        target(p, &c, &r);
        if (c != p->lock.c || r != p->lock.r) start_swing(p);     // the next cell, once they have moved on to it
    }
    if (!held(p, FH1_KEY_A)) p->lock.on = false;
}

// What this FH1's player's tool would make at their target, see-through (red where it may not): the ground as it
// would join, the building turned as it would stand. Remove shows none.
static void draw_ghost(const player_t *p, uint16_t *px, int w, int h, int camx, int camy)
{
    if (p->act.on || s_tools[p->tool].kind == T_REMOVE) return;
    int c, r;
    target(p, &c, &r);
    const bool ok = allowed(p, c, r, p->tool);
    const plan_t pl = plan(p, c, r, p->tool, p->var[p->tool], p->rot);
    world_ghost(&s_world, px, w, h, camx, camy, c, r, pl.ground, pl.var, pl.obj, pl.ovar, ok ? 150 : 110, 0xf9a6, ok ? 0 : 150);
}

// Is the target in front of the player (drawn after them)?
static bool target_in_front(const player_t *p)
{
    int c, r;
    target(p, &c, &r);
    float tu, tv, su, sv;
    world_to_screen(c + 0.5f, r + 0.5f, &tu, &tv);
    world_to_screen(p->u, p->v, &su, &sv);
    return tu + tv > su + sv;
}

// The target cell's top face outlined: green where they may, red where they may not.
static void draw_target(const player_t *p, uint16_t *px, int w, int h, int camx, int camy)
{
    int c, r;
    target(p, &c, &r);
    const uint16_t col = allowed(p, c, r, p->tool) ? 0x87f0 : 0xf9a6;
    float fx, fy;
    world_px(c + 0.5f, r + 0.5f, &fx, &fy);
    const int x = (int)fx - camx, y = (int)fy - camy;
    for (int k = 0; k <= TW / 2; k++) {                          // the four edges, two pixels thick
        const int dy = k * TH / TW;
        const int pts[8][2] = { { x - TW / 2 + k, y - dy }, { x + k, y - TH / 2 + dy }, { x - TW / 2 + k, y + dy },
                                { x + k, y + TH / 2 - dy }, { x - TW / 2 + k, y - dy + 1 }, { x + k, y - TH / 2 + dy + 1 },
                                { x - TW / 2 + k, y + dy - 1 }, { x + k, y + TH / 2 - dy - 1 } };
        for (int i = 0; i < 8; i++)
            if (pts[i][0] >= 0 && pts[i][0] < w && pts[i][1] >= 0 && pts[i][1] < h) px[pts[i][1] * w + pts[i][0]] = col;
    }
}

static void fill(const ui_t *u, int x, int y, int bw, int bh, uint16_t c)
{
    for (int j = y < 0 ? 0 : y; j < y + bh && j < u->h; j++)
        for (int i = x < 0 ? 0 : x; i < x + bw && i < u->w; i++) u->px[j * u->w + i] = c;
}

// The bar: a slot per tool with its chosen variant's picture, the chosen tool framed in yellow; its name, how many
// variants it has and which, the keys.
#define SLOT 28
static void draw_build_bar(const ui_t *u, const player_t *p)
{
    const int gap = 3, bw = TOOLS * (SLOT + gap) - gap + 16, y0 = u->h - 70, x0 = (u->w - bw) / 2 + 8;
    ui_box(u, x0 - 8, y0, bw, 66);
    for (int i = 0; i < TOOLS; i++) {
        const int x = x0 + i * (SLOT + gap), y = y0 + 6, ic = s_icon[i][p->var[i] % VARS];
        const bool sel = i == p->tool;
        if (sel) fill(u, x - 2, y - 2, SLOT + 4, SLOT + 4, 0xfee0);
        fill(u, x, y, SLOT, SLOT, sel ? 0x4a8a : 0x2945);
        const fart_clip_t slot = { x, y, x + SLOT, y + SLOT };
        if (ic >= 0) {
            fart_draw(u->px, u->w, &slot, &s_art.img[ic], x + SLOT / 2, y + SLOT / 2, false);
        } else {                                                       // remove: a red cross
            for (int k = 7; k < SLOT - 7; k++)
                for (int t = -1; t <= 1; t++) {
                    u->px[(y + k) * u->w + x + k + t] = 0xf9a6;
                    u->px[(y + k) * u->w + x + SLOT - 1 - k + t] = 0xf9a6;
                }
        }
        if (s_tools[i].n > 1 && s_tools[i].n <= 8)                    // a few variants: a dot per one, the chosen lit
            for (int v = 0; v < s_tools[i].n; v++)
                fill(u, x + SLOT / 2 - s_tools[i].n * 2 + v * 4 + 1, y + SLOT - 3, 2, 2, v == p->var[i] ? 0xffff : 0x6b4d);
    }
    char l[48];
    tool_name(p, l, sizeof(l));
    const bool refused = s_t < p->refuse_t;
    const int ty = y0 + SLOT + 14, right = x0 + bw - 16;
    ui_text(u, x0, ty, l, refused ? 0xff8070 : 0xf8d870);
    if (refused) ui_text(u, right - 64, ty, "aqui não", 0xff8070);
    else if (s_tools[p->tool].n > 1) {
        snprintf(l, sizeof(l), "{LR} %d/%d", p->var[p->tool] + 1, s_tools[p->tool].n);
        ui_hints(u, right - ui_hints_width(l), ty, l, 0xe0e0e8);
    }
    ui_hints(u, x0, ty + 13, "{Y} trocar {s} girar {A} fazer {X} sair", 0xe0e0e8);
}

// The owner's place, facing (on their screen) and view; "CL01" saves had no view (the first one).
typedef struct { uint32_t magic; float u, v; uint8_t oct, view, z; } save_t;   // z: how high (on a wall)
#define SAVE_MAGIC 0x434c3032u       // "CL02"
#define SAVE_MAGIC1 0x434c3031u      // "CL01"

static void save(void)
{
    if (s_mode != MODE_OWN) return;
    const player_t *p = &s_pl[0];
    const save_t s = { SAVE_MAGIC, p->u, p->v, (uint8_t)p->oct, (uint8_t)p->view, (uint8_t)p->z };
    fh1_state_save("lugar", &s, sizeof(s));
}

// Turning the view of this FH1's player (+1 a quarter clockwise, -1 back): they keep facing the same way in the
// world. The next frame shows the old view once more, kept for the turn (the engine's viewturn.h), then the new one
// turning in. Another player's view turns at once (their own FH1 shows the turn).
static viewturn_t s_turn;
static int s_turn_to;                                          // turns asked for: the quarters, 0 none
static void turn_player(player_t *p, int d)
{
    if (p == &s_pl[s_me]) {
        s_turn_to = ((s_turn_to + d) % 4 + 4) % 4;             // those asked during a turn: once it ends
        return;
    }
    p->view = ((p->view + d) % 4 + 4) % 4;
    p->oct = ((p->oct + 2 * d) % 8 + 8) % 8;
}

// The drawn view turned (this FH1's player's): the world's pieces chosen again, the camera kept on them.
static void apply_turn(int d)
{
    world_view(&s_world, (s_world.view + d) % 4);
    player_t *me = &s_pl[s_me];
    if (s_mode != MODE_GUEST) {                                // a guest's player turns at its owner's FH1
        me->view = s_world.view;
        me->oct = (me->oct + 2 * d) % 8;
    }
    float fx, fy;
    world_px(me->u, me->v, &fx, &fy);
    s_camx = fx - 160;
    s_camy = fy - 130;
}

// Can the player stand at (u, v)? Where they stand (their middle) and their feet's four corners all within a step of
// how high they are: up stairs, along a wall's walkway, but not off its edge. *h: how high they would be.
#define STEP_UP 7.0f
static bool fits(const player_t *p, float u, float v, float *h)
{
    const float hc = world_height(&s_world, u, v, p->z);
    if (hc < 0 || fabsf(hc - p->z) > STEP_UP) return false;
    const float m = FEET;
    const float cu[4] = { u - m, u + m, u - m, u + m }, cv[4] = { v - m, v - m, v + m, v + m };
    for (int k = 0; k < 4; k++) {
        const float hk = world_height(&s_world, cu[k], cv[k], hc);
        if (hk < 0 || fabsf(hk - hc) > STEP_UP) return false;
    }
    *h = hc;
    return true;
}

// One step along the D-pad's direction on the player's screen, turned into the map's axes (sliding along what
// blocks).
static void walk(player_t *p, uint32_t dt)
{
    float sx = (held(p, FH1_KEY_RIGHT) ? 1.0f : 0.0f) - (held(p, FH1_KEY_LEFT) ? 1.0f : 0.0f);
    float sy = (held(p, FH1_KEY_DOWN) ? 1.0f : 0.0f) - (held(p, FH1_KEY_UP) ? 1.0f : 0.0f);
    p->moving = sx != 0 || sy != 0;
    p->running = p->moving && held(p, FH1_KEY_B);
    if (!p->moving) return;
    // A diagonal on the D-pad is a map axis: along a row or a column of cells (24 across for 14 down on screen), so
    // they walk straight along a road and what they build lines up.
    if (sx != 0 && sy != 0) {
        sx *= TW / 2;
        sy *= TH / 2;
    }
    const float len = sqrtf(sx * sx + sy * sy);
    sx /= len;
    sy /= len;
    // Facing: the eighth of the direction on screen (clockwise from east, y down); held while building on.
    if (!p->lock.on) p->oct = ((int)floorf(atan2f(sy, sx) / 0.785398f + 0.5f) + 8) % 8;
    const float px = (p->running ? RUN : WALK) * dt / 1000.0f;
    // Screen pixels to the screen's cells: x = (u - v) * TW/2, y = (u + v) * TH/2; then turned to the world's.
    float du = (sx * px / (TW / 2) + sy * px / (TH / 2)) / 2, dv = (sy * px / (TH / 2) - sx * px / (TW / 2)) / 2;
    float su, sv;
    world_to_screen_v(p->view, p->u, p->v, &su, &sv);
    if (p->lock.on) {                                             // along one axis, drawn to the middle of the row
        if (p->lock.axis < 0) {
            p->lock.axis = fabsf(du) >= fabsf(dv) ? 0 : 1;
            p->lock.line = floorf(p->lock.axis == 0 ? sv : su) + 0.5f;
        }
        const float step = fabsf(du) + fabsf(dv);
        float *across = p->lock.axis == 0 ? &dv : &du;
        const float off = p->lock.line - (p->lock.axis == 0 ? sv : su);
        if (p->lock.axis == 0) du = du >= 0 ? step : -step; else dv = dv >= 0 ? step : -step;
        *across = fabsf(off) < step ? off : (off > 0 ? step : -step);
    }
    {
        float wu, wv;
        world_from_screen_v(p->view, su + du, sv + dv, &wu, &wv);
        du = wu - p->u;
        dv = wv - p->v;
    }
    const float nu = p->u + du, nv = p->v + dv;
    float h;
    #define FREE(U, V) fits(p, (U), (V), &h)
    if (FREE(nu, nv)) p->u = nu, p->v = nv;
    else if (fabsf(du) > 1e-4f && FREE(nu, p->v)) p->u = nu;               // slide along it
    else if (fabsf(dv) > 1e-4f && FREE(p->u, nv)) p->v = nv;
    else {                                                                  // a corner: eased sideways, into a gap
        const float n = sqrtf(du * du + dv * dv), pu = -dv / n * 0.04f, pv = du / n * 0.04f;
        if (FREE(nu + pu, nv + pv)) p->u = nu + pu, p->v = nv + pv;
        else if (FREE(nu - pu, nv - pv)) p->u = nu - pu, p->v = nv - pv;
        else return;
    }
    #undef FREE
    p->z = h;
}

// Footsteps of this FH1's player: on grass, on stone (roads, walls, stairs), on wood (bridges).
static void steps(player_t *p, uint32_t dt)
{
    if (!p->moving || p->act.on) {
        p->step_t = 0;
        return;
    }
    if (p->step_t > dt) {
        p->step_t -= dt;
        return;
    }
    p->step_t = p->running ? 260 : 380;
    int s = SN_ROCK;
    if (!p->inside && p->z < 1) {
        const int c = (int)floorf(p->u), r = (int)floorf(p->v);
        const cell_t *x = world_inside(c, r) ? world_cell(&s_world, c, r) : NULL;
        s = !x ? SN_GRASS : x->ground == G_BRIDGE ? SN_WOOD : x->ground == G_ROAD || x->obj == O_STAIR ? SN_ROCK : SN_GRASS;
    } else if (p->inside) s = SN_WOOD;
    snd(s, 55);
}

// ── Inside a building (the engine's interior.h) ──

static interior_art_t s_in_art;
static int8_t s_room[IN_GRID * IN_GRID];      // the ground floor's rooms: each one's cell in the building (-1: none)
static uint8_t s_busy[IN_ROWS][IN_COLS];      // tiles kept clear or taken while furnishing

static int in_img(const char *name) { return fart_image(&s_art, name); }

// The building a player faces (not building, on the ground), by any of its cells: the one ahead, or when they face
// a corner, either cell beside it. *c, *r: the cell.
static bool door_ahead(const player_t *p, int *c, int *r, building_t *b)
{
    if (p->build || p->z > 1) return false;
    int tc, tr;
    target(p, &tc, &tr);
    const int hc = (int)floorf(p->u), hr = (int)floorf(p->v);
    const int cc[3] = { tc, tc, hc }, rr[3] = { tr, hr, tr };
    for (int i = 0; i < 3; i++) {
        if (i > 0 && (tc == hc || tr == hr)) break;
        if (world_building(&s_world, cc[i], rr[i], b)) {
            *c = cc[i], *r = rr[i];
            return true;
        }
    }
    return false;
}

// The ground floor laid out like the building as the player saw it going in (in their view): a room for each cell
// where it is on their screen (at most IN_GRID x IN_GRID round the one they went in by, the entry room), a doorway
// wherever two cells join.
static void plan_ground(const player_t *p, interior_plan_t *pl)
{
    int sc[GROUP_MAX], sr[GROUP_MAX], g[GROUP_MAX], ec = 0, er = 0, c0 = 1 << 20, r0 = 1 << 20;
    const building_t *b = &p->bld;
    for (int i = 0; i < b->n; i++) {
        float su, sv;
        world_to_screen_v(p->view, b->c[i] + 0.5f, b->r[i] + 0.5f, &su, &sv);
        sc[i] = (int)floorf(su), sr[i] = (int)floorf(sv);
        if (sc[i] < c0) c0 = sc[i];
        if (sr[i] < r0) r0 = sr[i];
        if (b->c[i] == p->door_c && b->r[i] == p->door_r) ec = sc[i], er = sr[i];
    }
    if (ec - c0 >= IN_GRID) c0 = ec - IN_GRID + 1;
    if (er - r0 >= IN_GRID) r0 = er - IN_GRID + 1;
    memset(pl, 0, sizeof(*pl));
    memset(s_room, -1, sizeof(s_room));
    for (int i = 0; i < b->n; i++) {
        const int gx = sc[i] - c0, gy = sr[i] - r0;
        g[i] = gx >= 0 && gy >= 0 && gx < IN_GRID && gy < IN_GRID ? gx + IN_GRID * gy : -1;
        if (g[i] >= 0) pl->cells |= (uint16_t)(1u << g[i]), s_room[g[i]] = (int8_t)i;
    }
    static const int8_t dc[4] = { 0, 1, 0, -1 }, dr[4] = { -1, 0, 1, 0 };
    for (int i = 0; i < b->n; i++)
        for (int k = 0; k < 4; k++) {
            if (g[i] < 0 || !(b->joins[i] >> k & 1)) continue;
            for (int j = 0; j < b->n; j++) {
                if (g[j] < 0 || b->c[j] != b->c[i] + dc[k] || b->r[j] != b->r[i] + dr[k]) continue;
                if (sc[j] == sc[i] + 1) pl->join_r |= (uint16_t)(1u << g[i]);
                if (sr[j] == sr[i] + 1) pl->join_d |= (uint16_t)(1u << g[i]);
            }
        }
    pl->entry = (ec - c0) + IN_GRID * (er - r0);
    pl->open = b->open;
}

// A floor's plan: the ground as the building is, an upper floor one room (its stairs down where they come up).
static void plan_floor(const player_t *p, int f, interior_plan_t *pl)
{
    if (f == 0) {
        plan_ground(p, pl);
        return;
    }
    memset(pl, 0, sizeof(*pl));
    memset(s_room, -1, sizeof(s_room));
    pl->cells = 1;
    pl->entry = -1;
}

static void keep_clear(int x, int y, int w, int h)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            if (i >= 0 && j >= 0 && i < IN_COLS && j < IN_ROWS) s_busy[j][i] = 1;
}

// A piece of furniture in the room whose floor starts at (x0, top), w tiles across: its bottom `bottom` tiles below
// the room's top, as near `want` (tiles from its left) as it fits on floor not kept clear or taken; against the back
// wall (`back`): its bottom half a tile out from it. Rugs (depth 0) only need floor.
typedef struct { const char *name; float want, bottom, depth; bool back; } piece_t;

static void put_piece(interior_t *in, const piece_t *pc, int x0, int top, int w)
{
    const int img = in_img(pc->name);
    if (img < 0) return;
    const float iw = (float)s_art.img[img].w / IN_TILE, ih = (float)s_art.img[img].h / IN_TILE;
    const float y1 = top + (pc->back ? 0.5f : pc->bottom), depth = pc->back ? 0.5f : pc->depth;
    const int ty0 = (int)floorf(y1 - (depth > 0 ? depth : 0.5f) + 0.05f), ty1 = -(int)floorf(0.05f - y1) - 1;
    for (int k = 0; k <= 4 * w; k++) {
        const float x = pc->want + (float)((k + 1) / 2) * 0.5f * ((k & 1) ? -1 : 1);   // want, then either side
        if (x < 0 || x + iw > w + 0.01f) continue;
        const int tx0 = x0 + (int)floorf(x + 0.05f), tx1 = x0 + -(int)floorf(0.05f - x - iw) - 1;
        bool ok = true;
        for (int ty = ty0; ty <= ty1 && ok; ty++)
            for (int tx = tx0; tx <= tx1 && ok; tx++) {
                ok = in->tile[ty * IN_COLS + tx] == IT_FLOOR && (depth <= 0 || !s_busy[ty][tx]);
                if (pc->back) ok = ok && in->tile[(ty - 1) * IN_COLS + tx] == IT_WALL;
            }
        if (!ok) continue;
        if (depth > 0) keep_clear(tx0, ty0, tx1 - tx0 + 1, ty1 - ty0 + 1);
        interior_add(in, &s_art, img, x0 + x, y1 - ih, depth);
        return;
    }
}

// What each kind of room has (x in tiles from its left, bottoms from its top: its floor 8 x 5).
enum { R_HALL, R_BED, R_KITCHEN, R_STORE, R_FORGE, R_KEEP, R_STUDY, ROLES };
static const piece_t s_pieces[ROLES][8] = {
    [R_HALL] = { { "in.rug", 3.0f, 4.6f, 0 }, { "in.table", 2.8f, 4.6f, 0.9f }, { "in.chair", 1.9f, 4.4f, 0.6f },
                 { "in.chair2", 5.3f, 4.4f, 0.6f }, { "in.fire", 4.0f, 0, 0, true }, { "in.cabinet", 0.6f, 0, 0, true },
                 { "in.chest", 0.2f, 5.0f, 0.8f }, { "in.barrel", 6.8f, 5.0f, 0.8f } },
    [R_BED] = { { "in.rug", 1.2f, 4.4f, 0 }, { "in.bed", 5.2f, 5.0f, 1.4f }, { "in.dresser", 1.0f, 0, 0, true },
                { "in.chest", 0.2f, 5.0f, 0.8f } },
    [R_KITCHEN] = { { "in.pantry", 0.6f, 0, 0, true }, { "in.pots", 5.0f, 0, 0, true }, { "in.table", 2.6f, 4.6f, 0.9f },
                    { "in.chair", 1.7f, 4.4f, 0.6f }, { "in.barrel", 6.0f, 5.0f, 0.8f }, { "in.barrel", 7.0f, 4.9f, 0.8f } },
    [R_STORE] = { { "in.books", 0.6f, 0, 0, true }, { "in.cabinet", 5.6f, 0, 0, true }, { "in.chest", 0.2f, 5.0f, 0.8f },
                  { "in.chest", 5.8f, 5.0f, 0.8f }, { "in.barrel", 3.0f, 5.0f, 0.8f }, { "in.barrel", 4.0f, 4.9f, 0.8f } },
    [R_FORGE] = { { "in.oven", 0.6f, 0, 0, true }, { "in.cauldron", 4.6f, 4.4f, 1.0f }, { "in.barrel", 0.2f, 5.0f, 0.8f },
                  { "in.barrel", 1.2f, 4.9f, 0.8f }, { "in.bench", 2.6f, 5.0f, 0.8f }, { "in.chest", 6.0f, 5.0f, 0.8f } },
    [R_KEEP] = { { "in.carpet", 2.0f, 4.9f, 0 }, { "in.desk", 2.6f, 4.4f, 1.0f }, { "in.books", 4.0f, 0, 0, true },
                 { "in.pantry", 0.6f, 0, 0, true }, { "in.chest", 0.2f, 5.0f, 0.8f }, { "in.chest", 6.0f, 5.0f, 0.8f } },
    [R_STUDY] = { { "in.rug", 1.2f, 4.4f, 0 }, { "in.books", 0.6f, 0, 0, true }, { "in.bed", 5.2f, 5.0f, 1.4f },
                  { "in.chest", 0.2f, 5.0f, 0.8f } },
};

// The floor's furniture, room by room: the ways through each kept clear (a passage along it a tile out from its back
// wall, its door, the doorways), then what its kind of room has: the entry room a hall (a keep's great hall), a
// smithy's room its forge, the others bedroom, kitchen, store by turns; upstairs a bedroom (a keep's: a study).
static void furnish(player_t *p)
{
    interior_t *in = p->in;
    memset(s_busy, 0, sizeof(s_busy));
    const interior_plan_t *pl = &in->plan;
    for (int g = 0; g < IN_GRID * IN_GRID; g++) {
        int x, y, w, h;
        if (!interior_room(in, g % IN_GRID, g / IN_GRID, &x, &y, &w, &h)) continue;
        keep_clear(x, y + 1, w, 1);                                      // the passage
        if (in->door_x >= x && in->door_x < x + w) keep_clear(in->door_x - 1, y, 4, 1);
        if (g >= IN_GRID && (pl->join_d >> (g - IN_GRID) & 1)) keep_clear(x + IN_PASS, y, 2, 1);
        if (pl->join_d >> g & 1) keep_clear(x + IN_PASS, y + 1, 2, h);
        if (pl->join_r >> g & 1) keep_clear(x + w - 1, y + 1, 1, 2);
        if (g % IN_GRID && (pl->join_r >> (g - 1) & 1)) keep_clear(x, y + 1, 1, 2);
    }
    int others = 0;
    for (int g = 0; g < IN_GRID * IN_GRID; g++) {
        int x, y, w, h;
        if (!interior_room(in, g % IN_GRID, g / IN_GRID, &x, &y, &w, &h)) continue;
        const int i = s_room[g];
        const bool forge = i >= 0 && world_cell(&s_world, p->bld.c[i], p->bld.r[i])->obj == O_SMITH;
        const bool keep = p->bld.kind == B_KEEP;
        int role;
        if (in->floor > 0) role = keep ? R_STUDY : R_BED;
        else if (forge) role = R_FORGE;
        else if (g == pl->entry) role = keep ? R_KEEP : R_HALL;
        else role = (int[]){ R_BED, R_KITCHEN, R_STORE }[others++ % 3];
        for (int k = 0; k < 8 && s_pieces[role][k].name; k++) put_piece(in, &s_pieces[role][k], x, y, w);
    }
}

// The player's building laid out inside (into p->in), on floor f: the same for whoever goes in by the same door in the
// same view, on whichever FH1 does it.
static bool lay_out(player_t *p, int f)
{
    if (!p->in) p->in = calloc(1, sizeof(interior_t) < 4160 ? 4160 : sizeof(interior_t));
    if (!p->in) return false;
    // Its colours for what has no picture: a stone keep, a wooden or white house.
    static const uint16_t wood[IN_COLOURS] = { 0x7a69, 0x6207, 0xb48c, 0x3164, 0x8a86 };
    static const uint16_t stone[IN_COLOURS] = { 0x7bcf, 0x632c, 0x9cf3, 0x31a6, 0x6b4d };
    interior_plan_t pl;
    plan_floor(p, 0, &pl);
    interior_enter(p->in, p->bld.floors, &pl, p->bld.kind == B_KEEP ? stone : wood);
    if (f > 0) {
        plan_floor(p, f, &pl);
        interior_floor(p->in, f, &pl);
    }
    furnish(p);
    return true;
}

static void go_in(player_t *p, int c, int r, const building_t *b)
{
    p->bld = *b;
    p->door_c = c, p->door_r = r;
    if (!lay_out(p, 0)) return;
    p->inside = true;
    p->oct = FART_S8;
    if (p == &s_pl[s_me]) snd(SN_DOOR, 150);
}

static void go_floor(player_t *p, int f)
{
    interior_plan_t pl;
    plan_floor(p, f, &pl);
    interior_floor(p->in, f, &pl);
    furnish(p);
}

// Back out where they stood going in, facing away from the building.
static void go_out(player_t *p)
{
    p->inside = false;
    p->z = 0;
    float su, sv, tu, tv;
    world_to_screen_v(p->view, p->door_c + 0.5f, p->door_r + 0.5f, &tu, &tv);
    world_to_screen_v(p->view, p->u, p->v, &su, &sv);
    const float x = (su - tu) - (sv - tv), y = (su - tu) + (sv - tv);
    p->oct = ((int)floorf(atan2f(y * 14, x * 24) / 0.785398f + 0.5f) + 8) % 8;
    if (p == &s_pl[s_me]) snd(SN_DOOR, 150);
}

static void inside_input(player_t *p, uint32_t dt)
{
    float sx = (held(p, FH1_KEY_RIGHT) ? 1.0f : 0.0f) - (held(p, FH1_KEY_LEFT) ? 1.0f : 0.0f);
    float sy = (held(p, FH1_KEY_DOWN) ? 1.0f : 0.0f) - (held(p, FH1_KEY_UP) ? 1.0f : 0.0f);
    p->moving = sx != 0 || sy != 0;
    p->running = false;
    if (!p->moving) return;
    const float len = sqrtf(sx * sx + sy * sy), sp = (held(p, FH1_KEY_B) ? 7.0f : 4.0f) * dt / 1000.0f;
    p->oct = ((int)floorf(atan2f(sy, sx) / 0.785398f + 0.5f) + 8) % 8;
    const int ev = interior_move(p->in, &s_art, sx / len * sp, sy / len * sp);
    if (ev == IN_EXIT) go_out(p);
    else if (ev == IN_UP || ev == IN_DOWN) go_floor(p, p->in->floor + (ev == IN_UP ? 1 : -1));
}

// ── Drawing players ──

// A player's colour under their feet: a ring on the ground.
static void ring(uint16_t *px, int w, int h, int x, int y, uint32_t rgb)
{
    const uint16_t c = (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
    for (int a = 0; a < 64; a++) {
        const float t = a * 0.0981748f;                          // a full turn in 64 steps
        for (int k = 0; k < 2; k++) {
            const int rx = 10 - k, ry = 5 - k;
            const int xi = x + (int)floorf(cosf(t) * rx + 0.5f), yi = y + (int)floorf(sinf(t) * ry + 0.5f);
            if (xi >= 0 && yi >= 0 && xi < w && yi < h) px[yi * w + xi] = c;
        }
    }
}

// A player's sprite with its feet at (x, y): walking, running, swinging or standing, as they are.
static const fart_img_t *draw_sprite(const player_t *p, uint16_t *px, int w, int h, int x, int y, bool *mirror)
{
    const fart_anim_t *a = p->act.on && s_swing ? s_swing : p->moving ? (p->running && s_run ? s_run : s_walk) : s_idle;
    const int i = fart_frame8(a, p->oct, p->act.on ? p->act.t : p->t, mirror);
    if (i < 0) return NULL;
    const fart_clip_t clip = { 0, 0, w, h };
    fart_draw(px, w, &clip, &s_art.img[i], x, y, *mirror);
    return &s_art.img[i];
}

// Another player's name over their head, in their colour.
static void name_tag(uint16_t *px, int w, int h, int x, int y, int i)
{
    const player_t *p = &s_pl[i];
    if (!p->name[0]) return;
    const ui_t u = { px, w, h };
    const int tw = (int)strlen(p->name) * 6;
    ui_text(&u, x - tw / 2, y - 46, p->name, ui_player_colour(i));
}

// A player in the world: their ring, them, what stands in front of them drawn again over them and cut away round
// them, their name (the others').
static void draw_player(int i, uint16_t *px, int w, int h, int camx, int camy)
{
    const player_t *p = &s_pl[i];
    float fx, fy;
    world_px(p->u, p->v, &fx, &fy);
    const int x = (int)floorf(fx + 0.5f) - camx, y = (int)floorf(fy - p->z + 0.5f) - camy;   // up as they climb
    if (x < -40 || x > w + 40 || y < -20 || y > h + 60) return;
    if (s_mode != MODE_START && (s_mode == MODE_GUEST || s_pl[1].on || s_pl[2].on || s_pl[3].on))
        ring(px, w, h, x, y, ui_player_colour(i));             // colours when there are others
    bool mirror;
    const fart_img_t *im = draw_sprite(p, px, w, h, x, y, &mirror);
    if (!im) return;
    const fart_clip_t box = { x + (mirror ? -im->ox - im->w : im->ox), y + im->oy,
                              x + (mirror ? -im->ox : im->ox + im->w), y + im->oy + im->h };
    world_cover(&s_world, px, w, h, camx, camy, p->u, p->v, &box);
    if (i != s_me) name_tag(px, w, h, x, y, i);
}

static void draw_walker(uint16_t *px, int w, int h, int x, int y, void *user)
{
    (void)user;
    const player_t *me = &s_pl[s_me];
    if (s_mode == MODE_GUEST || s_pl[1].on || s_pl[2].on || s_pl[3].on) ring(px, w, h, x, y, ui_player_colour(s_me));
    bool mirror;
    draw_sprite(me, px, w, h, x, y, &mirror);
}

static void draw_other_walker(uint16_t *px, int w, int h, int x, int y, int i, void *user)
{
    const int *who = user;
    ring(px, w, h, x, y, ui_player_colour(who[i]));
    bool mirror;
    draw_sprite(&s_pl[who[i]], px, w, h, x, y, &mirror);
    name_tag(px, w, h, x, y, who[i]);
}

// Inside: this FH1's player's floor, the others there with them (in by the same door, in the same view, on the same
// floor: the same rooms).
static void draw_inside(uint16_t *px, int w, int h)
{
    const player_t *me = &s_pl[s_me];
    float ox[NP], oy[NP];
    int who[NP], n = 0;
    for (int i = 0; i < NP; i++) {
        const player_t *q = &s_pl[i];
        if (i == s_me || !q->on || !q->inside || !q->in || q->door_c != me->door_c || q->door_r != me->door_r ||
            q->view != me->view || q->in->floor != me->in->floor)
            continue;
        ox[n] = q->in->x, oy[n] = q->in->y, who[n] = i;
        n++;
    }
    interior_draw_with(me->in, &s_art, &s_in_art, px, w, h, draw_walker, who, ox, oy, n, draw_other_walker);
}

// ── Players coming and going (the owner's FH1) ──

static void player_reset(player_t *p)
{
    interior_t *in = p->in;
    memset(p, 0, sizeof(*p));
    p->in = in;
    p->oct = FART_SE8;
}

// A guest comes in: beside the owner (on the ground, where they can stand), turned as the owner sees the world.
static void join(int i)
{
    player_t *p = &s_pl[i], *o = &s_pl[0];
    player_reset(p);
    p->on = true;
    p->view = o->view;
    p->u = o->u, p->v = o->v;
    // A portal on open ground near the owner, with room to walk out of it (a cell beside it, along a map axis); they
    // step out of it once it has opened.
    static const int8_t dc[8] = { 1, -1, 0, 0, 1, -1, 1, -1 }, dr[8] = { 0, 0, 1, -1, 1, 1, -1, -1 };
    for (int rad = 1; rad <= 3; rad++)
        for (int k = 0; k < 8; k++)
            for (int axis = 0; axis < 2; axis++) {
                const float u = floorf(o->u) + dc[k] * rad + 0.5f, v = floorf(o->v) + dr[k] * rad + 0.5f;
                const float eu = u + (axis ? 0 : 1), ev = v + (axis ? 1 : 0);
                float hh;
                if (world_height(&s_world, u, v, 0) == 0 && fits(p, u, v, &hh) && world_height(&s_world, eu, ev, 0) == 0 &&
                    fits(p, eu, ev, &hh) && fits(p, (u + eu) / 2, (v + ev) / 2, &hh)) {
                    p->u = u, p->v = v;
                    p->ex = eu, p->ev = ev;
                    p->gate_axis = (uint8_t)axis;
                    p->arriving = p->hidden = s_portal_ok;
                    if (!s_portal_ok) p->u = eu, p->v = ev;
                    gate_open(u, v, axis, i, ARRIVE_HIDE + ARRIVE_STAY);
                    goto placed;
                }
            }
placed:
    fh1_net_player(i, p->name, sizeof(p->name));
    char l[48];
    snprintf(l, sizeof(l), "%s entrou no seu mundo", p->name[0] ? p->name : "Alguém");
    note(l);
    snd(SN_JOIN, 170);
}

static void leave(int i)
{
    player_t *p = &s_pl[i];
    char l[48];
    snprintf(l, sizeof(l), "%s saiu", p->name[0] ? p->name : "Alguém");
    note(l);
    snd(SN_LEAVE, 150);
    if (!p->inside) gate_open(floorf(p->u) + 0.5f, floorf(p->v) + 0.5f, 0, i, 500);   // gone through one
    p->on = false;
    p->inside = false;
}

// The guests' controllers, as keys (buttons by position: the FH1's A is on the right, B at the bottom) and buttons.
static void guest_input(player_t *p, int i)
{
    fh1_pad_t pad;
    uint32_t b = 0, d = 0;
    if (fh1_pad(i, &pad)) {
        b = pad.buttons;
        static const struct { uint32_t btn; uint8_t key; } m[] = {
            { FH1_PAD_BTN_UP, FH1_KEY_UP }, { FH1_PAD_BTN_DOWN, FH1_KEY_DOWN }, { FH1_PAD_BTN_LEFT, FH1_KEY_LEFT },
            { FH1_PAD_BTN_RIGHT, FH1_KEY_RIGHT }, { FH1_PAD_BTN_B, FH1_KEY_A }, { FH1_PAD_BTN_A, FH1_KEY_B },
            { FH1_PAD_BTN_Y, FH1_KEY_X }, { FH1_PAD_BTN_X, FH1_KEY_Y }, { FH1_PAD_BTN_START, FH1_KEY_START },
            { FH1_PAD_BTN_SELECT, FH1_KEY_SELECT }, { FH1_PAD_BTN_SYSTEM, FH1_KEY_MENU },
        };
        for (size_t k = 0; k < sizeof(m) / sizeof(m[0]); k++)
            if (b & m[k].btn) d |= 1u << m[k].key;
        if (pad.lx < -200) d |= 1u << FH1_KEY_LEFT;
        if (pad.lx > 200) d |= 1u << FH1_KEY_RIGHT;
        if (pad.ly < -200) d |= 1u << FH1_KEY_UP;
        if (pad.ly > 200) d |= 1u << FH1_KEY_DOWN;
    }
    p->edge = d & ~p->down;
    p->down = d;
    p->pad_edge = b & ~p->pad;
    p->pad = b;
}

// A newcomer's first moments: hidden while their portal opens, then walking out of it by themselves (their keys
// wait), facing the way they go on their own screen.
static void arrive(player_t *p, uint32_t dt)
{
    p->arrive_t += dt;
    p->t += dt;
    p->moving = false;
    p->hidden = p->arrive_t < ARRIVE_HIDE;
    if (p->hidden) return;
    const float du = p->ex - p->u, dv = p->ev - p->v, d = sqrtf(du * du + dv * dv), sp = 1.6f * dt / 1000.0f;
    float a, b, c, e;
    world_to_screen_v(p->view, p->u, p->v, &a, &b);
    world_to_screen_v(p->view, p->ex, p->ev, &c, &e);
    const float sx = ((c - a) - (e - b)) * TW / 2, sy = ((c - a) + (e - b)) * TH / 2;
    if (sx != 0 || sy != 0) p->oct = ((int)floorf(atan2f(sy, sx) / 0.785398f + 0.5f) + 8) % 8;
    if (d <= sp) {
        p->u = p->ex, p->v = p->ev;
        p->arriving = false;
        return;
    }
    p->u += du / d * sp, p->v += dv / d * sp;
    p->moving = true;
}

// A player's moment in the world (on the owner's FH1).
static void step(player_t *p, uint32_t dt)
{
    const bool mine = p == &s_pl[s_me];
    if (p->arriving) {
        arrive(p, dt);
        return;
    }
    if (!mine) {                                                   // a guest: Start asks whether to go back home
        if (p->asking) {
            if (key(p, FH1_KEY_B) || key(p, FH1_KEY_START)) p->asking = false;
            p->moving = false;
            return;
        }
        if (key(p, FH1_KEY_START) || key(p, FH1_KEY_MENU)) {
            p->asking = true;
            p->moving = false;
            return;
        }
    }
    int dc, dr;
    building_t b;
    if (p->inside) {
        inside_input(p, dt);
        p->t += dt;
        return;
    }
    if (key(p, FH1_KEY_A) && !p->act.on && door_ahead(p, &dc, &dr, &b)) {   // into the building they face
        go_in(p, dc, dr, &b);
        return;
    }
    if (key(p, FH1_KEY_X) && !p->act.on) p->build = !p->build;
    if (p->build) build_input(p);
    else if (!p->act.on) {                                         // the view: Select or R1 a quarter on, L1 back
        if (key(p, FH1_KEY_SELECT) || (p->pad_edge & FH1_PAD_BTN_R1)) turn_player(p, 1);
        if (p->pad_edge & FH1_PAD_BTN_L1) turn_player(p, -1);
    }
    if (p->act.on) {
        p->moving = false;
        if ((p->act.t += dt) >= p->act.len) {
            p->act.on = false;
            apply(p, p->act.c, p->act.r, p->act.tool, p->act.var, p->act.rot);
        }
    } else walk(p, dt);
    p->t += dt;
}

// ── The wire: what the owner's FH1 sends its guests, about 30 times a second ──
// The players (where they are, how they move, their build mode, inside or not) and the world's changes through the
// engine's netrep.h (the latest ones, and the whole list going round for those who join late).

#define WIRE_VER 1
enum { W_ON = 1, W_MOVING = 2, W_RUNNING = 4, W_BUILD = 8, W_ACT = 16, W_INSIDE = 32, W_ASKING = 64, W_REFUSE = 128 };
typedef struct __attribute__((packed)) {
    uint8_t flags;                   // W_*
    uint16_t u, v;                   // 1/256 cell
    uint8_t z, ov;                   // z (pixels); facing (bits 0-2), view (3-4), coming in (5), hidden (6), way out (7)
    uint8_t tool, rot;
    uint8_t var[TOOLS];
    uint16_t t, act_t;               // their animation's time, their swing's
    uint8_t door_c, door_r, floor;   // inside: the door they went in by, the floor
    uint16_t ix, iy;                 // inside: where (1/256 tile)
} wire_pl_t;

static netrep_t s_rep;
static void on_edit(const edit_t *e, void *user) { (void)user; netrep_changed(&s_rep, e); }

static void send_state(void)
{
    static uint8_t buf[FH1_NET_STATE_MAX];
    buf[0] = WIRE_VER;
    buf[1] = 0;
    wire_pl_t *w = (wire_pl_t *)(buf + 2);
    for (int i = 0; i < NP; i++, w++) {
        const player_t *p = &s_pl[i];
        memset(w, 0, sizeof(*w));
        if (!p->on) continue;
        w->flags = (uint8_t)(W_ON | (p->moving ? W_MOVING : 0) | (p->running ? W_RUNNING : 0) | (p->build ? W_BUILD : 0) |
                             (p->act.on ? W_ACT : 0) | (p->inside ? W_INSIDE : 0) | (p->asking ? W_ASKING : 0) |
                             (s_t < p->refuse_t ? W_REFUSE : 0));
        w->u = (uint16_t)(p->u * 256), w->v = (uint16_t)(p->v * 256);
        w->z = (uint8_t)p->z;
        w->ov = (uint8_t)((p->oct & 7) | (p->view & 3) << 3 | (p->arriving ? 32 : 0) | (p->hidden ? 64 : 0) |
                          (p->gate_axis ? 128 : 0));
        w->tool = (uint8_t)p->tool, w->rot = (uint8_t)p->rot;
        memcpy(w->var, p->var, TOOLS);
        w->t = (uint16_t)p->t, w->act_t = (uint16_t)p->act.t;
        if (p->inside && p->in) {
            w->door_c = (uint8_t)p->door_c, w->door_r = (uint8_t)p->door_r, w->floor = (uint8_t)p->in->floor;
            w->ix = (uint16_t)(p->in->x * 256), w->iy = (uint16_t)(p->in->y * 256);
        }
    }
    const int head = 2 + NP * (int)sizeof(wire_pl_t);
    const int n = netrep_pack(&s_rep, buf + head, (int)sizeof(buf) - head, 40, s_world.edits, s_world.nedits);
    fh1_net_state_send(buf, (size_t)(head + n));
}

// ── A guest's FH1: the friend's world as their FH1 sends it ──

static bool s_got;                   // a state has come
static uint8_t s_prev_flags[NP];

static void apply_remote(const void *rec, void *user)
{
    (void)user;
    const edit_t *e = rec;
    if (!world_inside(e->c, e->r)) return;
    const cell_t *x = world_cell(&s_world, e->c, e->r);
    if (x->ground == e->ground && x->var == e->var && x->obj == e->obj && x->ovar == e->ovar &&
        (x->flags & F_BUILT) == (e->flags & F_BUILT))
        return;                                                    // as it is already
    const bool removed = e->obj == O_NONE && (e->ground == G_GRASS || e->ground == G_ROAD);
    world_set(&s_world, e->c, e->r, e->ground, e->var, e->obj, e->ovar, e->flags & F_BUILT);
    if (s_rep.synced) {                                            // live (not catching up): dust and a sound
        float fx, fy;
        world_px(e->c + 0.5f, e->r + 0.5f, &fx, &fy);
        fx_burst(fx, fy - 4, 0xce59, 0x9cd3, 12, 30);
        snd_at(removed ? SN_BREAK : SN_BUILD, e->c + 0.5f, e->r + 0.5f, 170);
    }
}

static void take_state(void)
{
    static uint8_t buf[FH1_NET_STATE_MAX];
    const int n = fh1_net_state_recv(buf, sizeof(buf));
    const int head = 2 + NP * (int)sizeof(wire_pl_t);
    if (n < head || buf[0] != WIRE_VER) return;
    s_me = fh1_net_me();
    if (s_me < 0 || s_me >= NP) s_me = 1;
    const wire_pl_t *w = (const wire_pl_t *)(buf + 2);
    for (int i = 0; i < NP; i++, w++) {
        player_t *p = &s_pl[i];
        const bool was = p->on;
        p->on = w->flags & W_ON;
        if (!p->on) {
            if (was && s_got && i != s_me) {
                char l[48];
                snprintf(l, sizeof(l), "%s saiu", p->name[0] ? p->name : "Alguém");
                note(l);
                snd(SN_LEAVE, 150);
                if (!p->inside) gate_open(floorf(p->u) + 0.5f, floorf(p->v) + 0.5f, 0, i, 500);
            }
            p->inside = false;
            continue;
        }
        fh1_net_player(i, p->name, sizeof(p->name));
        if (!was && s_got && i != s_me) {
            char l[48];
            snprintf(l, sizeof(l), "%s entrou", p->name[0] ? p->name : "Alguém");
            note(l);
            snd(SN_JOIN, 170);
        }
        const uint8_t f = w->flags, pf = s_prev_flags[i];
        p->u = w->u / 256.0f, p->v = w->v / 256.0f, p->z = w->z;
        p->oct = w->ov & 7;
        p->view = (w->ov >> 3) & 3;
        // Coming in: their portal, opened here as it opened at the owner's (where they stand while hidden; else the
        // cell they are leaving).
        const bool arriving = w->ov & 32;
        p->hidden = w->ov & 64;
        p->gate_axis = w->ov >> 7;
        if (arriving && !p->arriving) {
            const float bu = p->hidden ? p->u : p->u - (p->gate_axis ? 0 : 0.5f), bv = p->hidden ? p->v : p->v - (p->gate_axis ? 0.5f : 0);
            gate_open(floorf(bu) + 0.5f, floorf(bv) + 0.5f, p->gate_axis, i, ARRIVE_HIDE + ARRIVE_STAY);
        }
        p->arriving = arriving;
        p->moving = f & W_MOVING, p->running = f & W_RUNNING, p->build = f & W_BUILD, p->asking = f & W_ASKING;
        p->act.on = f & W_ACT;
        p->refuse_t = (f & W_REFUSE) ? s_t + 100 : 0;
        if (i == s_me) {                                           // their own: what they did, heard
            if (p->tool != w->tool && s_got) snd(SN_TOOL, 140);
            else if (s_got && (memcmp(p->var, w->var, TOOLS) || p->rot != w->rot)) snd(SN_PICK, 110);
            if ((f & W_ACT) && !(pf & W_ACT) && s_got) snd(SN_SWING, 110);
            if ((f & W_REFUSE) && !(pf & W_REFUSE) && s_got) snd(SN_NO, 150);
            if (((f ^ pf) & W_INSIDE) && s_got) snd(SN_DOOR, 150);
        }
        p->tool = w->tool % TOOLS, p->rot = w->rot & 3;
        memcpy(p->var, w->var, TOOLS);
        p->t = w->t, p->act.t = w->act_t;
        // Inside: the same rooms laid out again here (their door, their view, their floor), where they are in them.
        const bool inside = f & W_INSIDE;
        if (inside) {
            building_t b;
            const bool fresh = !p->inside || p->door_c != w->door_c || p->door_r != w->door_r || !p->in ||
                               p->in->floor != w->floor || p->in->floors != p->bld.floors;
            if (fresh && world_building(&s_world, w->door_c, w->door_r, &b)) {
                p->bld = b;
                p->door_c = w->door_c, p->door_r = w->door_r;
                lay_out(p, w->floor);
            }
            if (p->in) p->in->x = w->ix / 256.0f, p->in->y = w->iy / 256.0f;
            p->inside = p->in != NULL;
        } else p->inside = false;
        s_prev_flags[i] = f;
    }
    netrep_unpack(&s_rep, buf + head, n - head, apply_remote, NULL);
    s_got = true;
}

// ── The start menu: play in your own world, find a friend's, leave ──

static fh1_net_game_t s_games[6];
static int s_ngames, s_pick;
static uint32_t s_find_at;
static float s_start_u, s_start_v;
static const char *s_status;         // a line on the find screen: what went wrong

static void start_menu(void)
{
    memset(&s_menu, 0, sizeof(s_menu));
    s_menu.title = "Construção Legal";
    s_menu.items[0] = "Meu mundo";
    s_menu.items[1] = "Mundos de amigos";
    s_menu.items[2] = "Sair";
    s_menu.n = 3;
    s_mode = MODE_START;
}

// Everyone gone, the fixed world again (a friend's world, or one's own, left behind).
static void world_clean(void)
{
    for (int i = 0; i < NP; i++) player_reset(&s_pl[i]);
    s_world.on_edit = NULL;
    world_reset(&s_world, &s_start_u, &s_start_v);
    fx_reset();
    memset(s_gate, 0, sizeof(s_gate));
    s_got = false;
    s_me = 0;
    memset(s_prev_flags, 0, sizeof(s_prev_flags));
    netrep_init(&s_rep, sizeof(edit_t));
}

// Into one's own world: its changes, its owner where they were, open to friends on the network.
static void play_own(void)
{
    player_t *p = &s_pl[0];
    player_reset(p);
    p->on = true;
    p->u = s_start_u, p->v = s_start_v;
    load_world();
    save_t s;
    memset(&s, 0, sizeof(s));
    const int got = fh1_state_load("lugar", &s, sizeof(s));
    if (s.magic == SAVE_MAGIC1) s.view = s.z = 0;
    const float hz = world_height(&s_world, s.u, s.v, s.z);
    if (((got == (int)sizeof(s) && s.magic == SAVE_MAGIC) || (got >= 13 && s.magic == SAVE_MAGIC1)) &&
        hz >= 0 && fabsf(hz - s.z) <= STEP_UP) {
        p->z = hz;
        p->u = s.u;
        p->v = s.v;
        p->oct = s.oct % 8;
        world_view(&s_world, s.view % 4);
    } else world_view(&s_world, 0);
    p->view = s_world.view;
    fh1_user(0, p->name, sizeof(p->name));
    s_world.on_edit = on_edit;                                    // its changes, for whoever comes in
    s_me = 0;
    s_mode = MODE_OWN;
    if (!fh1_net_host(1, NP - 1)) note("Sem rede: só você neste mundo");
    float fx, fy;
    world_px(p->u, p->v, &fx, &fy);
    s_camx = fx - 160;
    s_camy = fy - 130;
}

static void find_start(void)
{
    s_mode = MODE_FIND;
    s_pick = 0;
    s_ngames = 0;
    s_status = NULL;
    if (!fh1_net_find()) s_status = "Sem rede";
    s_find_at = s_t;
}

static void join_pick(int i)
{
    if (i < 0 || i >= s_ngames) return;
    if (!fh1_net_join(&s_games[i])) {
        s_status = "Não deu para entrar";
        return;
    }
    s_status = "Entrando...";
}

// Back to the start, from either world: one's own saved and closed to others, a friend's left.
static void back_to_start(void)
{
    if (s_mode == MODE_OWN) {
        save();
        save_world();
    }
    fh1_net_close();
    if (viewturn_active(&s_turn)) viewturn_free(&s_turn);
    s_turn_to = 0;
    s_paused = false;
    world_clean();
    start_menu();
}

static int menu_key(void)
{
    if (s_edge & (1u << FH1_KEY_UP)) return FH1_KEY_UP;
    if (s_edge & (1u << FH1_KEY_DOWN)) return FH1_KEY_DOWN;
    if (s_edge & (1u << FH1_KEY_A)) return FH1_KEY_A;
    if (s_edge & (1u << FH1_KEY_B)) return FH1_KEY_B;
    return -1;
}

static void open_menu(void)
{
    memset(&s_menu, 0, sizeof(s_menu));
    s_menu.title = "Construção Legal";
    s_menu.items[0] = "Continuar";
    s_menu.items[1] = s_info ? "Esconder tempos" : "Mostrar tempos";
    s_menu.items[2] = "Voltar ao início";
    s_menu.n = 3;
    s_paused = true;
}

bool dev_command(const char *line);

// The world round this FH1's player, the players in it (back to front), what they build, smoke and sparks; the camera
// easing toward its player (their feet a little below the middle).
static void draw_scene(uint16_t *px, int w, int h, uint32_t dt)
{
    const player_t *me = &s_pl[s_me];
    if (me->on) {                                              // (still coming in: the camera stays)
        float fx, fy;
        world_px(me->u, me->v, &fx, &fy);
        const float tx = fx - w / 2, ty = fy - h / 2 - 10;
        float k = dt / 140.0f;
        if (k > 1) k = 1;
        s_camx += (tx - s_camx) * k;
        s_camy += (ty - s_camy) * k;
    }
    const int camx = (int)floorf((s_camx) + 0.5f), camy = (int)floorf((s_camy) + 0.5f);
    world_tick(&s_world, dt);
    world_draw(&s_world, px, w, h, camx, camy);
    if (s_mode == MODE_START || s_mode == MODE_FIND) return;
    const bool building = me->on && me->build && !s_paused && !me->asking;
    if (building) draw_target(me, px, w, h, camx, camy);
    const bool ghost_after = building && target_in_front(me);
    if (building && !ghost_after) draw_ghost(me, px, w, h, camx, camy);
    // The players outside and the portals, back to front (by how far down the screen they stand).
    int order[NP + GATES], n = 0;
    float depth[NP + GATES];
    for (int i = 0; i < NP + GATES; i++) {
        float u, v;
        if (i < NP) {
            const player_t *p = &s_pl[i];
            if (!p->on || p->inside || p->hidden) continue;
            u = p->u, v = p->v;
        } else {
            const gate_t *g = &s_gate[i - NP];
            if (!g->on) continue;
            u = g->u, v = g->v;
        }
        float su, sv;
        world_to_screen(u, v, &su, &sv);
        depth[i] = su + sv;
        int j = n++;
        for (; j > 0 && depth[order[j - 1]] > depth[i]; j--) order[j] = order[j - 1];
        order[j] = i;
    }
    for (int k2 = 0; k2 < n; k2++) {
        if (order[k2] < NP) draw_player(order[k2], px, w, h, camx, camy);
        else gate_draw(&s_gate[order[k2] - NP], px, w, h, camx, camy);
    }
    if (ghost_after) draw_ghost(me, px, w, h, camx, camy);
    world_smoke(&s_world, px, w, h, camx, camy);
    fx_draw(&s_art, px, w, h, camx, camy);
}

// The start: the world behind, the menu over it; or the friends' worlds found.
static int start_frame(uint16_t *px, int w, int h, uint32_t dt)
{
    draw_scene(px, w, h, dt);
    const ui_t u = { px, w, h };
    if (s_mode == MODE_START) {
        const int r = ui_menu_key(&s_menu, menu_key());
        if (r == 0) play_own();
        else if (r == 1) find_start();
        else if (r == 2 || r == UI_BACK) s_quit = true;
        if (s_mode == MODE_START) {
            int bw, bh;
            ui_menu_size(&s_menu, &bw, &bh);
            ui_menu_draw(&u, &s_menu, (w - bw) / 2, (h - bh) / 2, s_t);
        }
        return s_quit ? FH1_FRAME_END : FH1_FRAME_SHOW;
    }
    // Finding: the worlds friends play now, asked again now and then.
    const int st = fh1_net_status();
    if (st == FH1_NET_GUEST) {
        s_mode = MODE_GUEST;
        world_clean();
        s_mode = MODE_GUEST;
        return FH1_FRAME_SHOW;
    }
    if (st == FH1_NET_LOST) {
        fh1_net_close();
        s_status = "Não deu para entrar";
        fh1_net_find();
        s_find_at = s_t;
    }
    if (s_edge & (1u << FH1_KEY_B)) {
        if (ui_sound) ui_sound(UI_SND_BACK);
        fh1_net_close();
        start_menu();
        return FH1_FRAME_SHOW;
    }
    if (st != FH1_NET_JOINING) {
        if (s_t - s_find_at > 2000) {
            fh1_net_find();
            s_find_at = s_t;
        }
        s_ngames = fh1_net_games(s_games, 6);
        if (s_pick >= s_ngames) s_pick = s_ngames ? s_ngames - 1 : 0;
        if ((s_edge & (1u << FH1_KEY_UP)) && s_pick > 0) s_pick--, snd(SN_PICK, 120);
        if ((s_edge & (1u << FH1_KEY_DOWN)) && s_pick + 1 < s_ngames) s_pick++, snd(SN_PICK, 120);
        if ((s_edge & (1u << FH1_KEY_A)) && s_ngames) {
            snd(SN_OK, 170);
            join_pick(s_pick);
        }
    }
    const int bw = 260, bh = 150, x = (w - bw) / 2, y = (h - bh) / 2;
    ui_box(&u, x, y, bw, bh);
    ui_text(&u, x + 10, y + 8, "Mundos de amigos", 0xf8d870);
    if (!s_ngames)
        ui_text(&u, x + 10, y + 32, s_status ? s_status : "Procurando...", 0xe0e0e8);
    for (int i = 0; i < s_ngames; i++) {
        char l[48];
        snprintf(l, sizeof(l), "Mundo de %.30s", s_games[i].name);
        const int ly = y + 30 + i * 14;
        if (i == s_pick) ui_text_wave(&u, x + 22, ly, l, 0xffffff, s_t);
        else ui_text(&u, x + 22, ly, l, 0xc8c8d0);
        if (i == s_pick) ui_text(&u, x + 10, ly, ">", 0xf8d870);
        snprintf(l, sizeof(l), "%d/%d", s_games[i].players, s_games[i].players + s_games[i].seats);
        ui_text(&u, x + bw - 40, ly, l, 0xa0a0b0);
    }
    if (s_ngames && s_status) ui_text(&u, x + 10, y + bh - 34, s_status, 0xe0e0e8);
    ui_hints(&u, x + 10, y + bh - 18, st == FH1_NET_JOINING ? "{B} voltar" : "{A} entrar  {B} voltar", 0xe0e0e8);
    return FH1_FRAME_SHOW;
}

static int frame(uint16_t *px, int w, int h, uint32_t dt, void *user)
{
    const uint32_t t0 = fh1_millis();
    s_t += dt;
    uint32_t down = 0;
    for (int k = 0; k < KEYS; k++)
        if (fh1_input_is_down(k)) down |= 1u << k;
    s_edge = down & ~s_down;
    s_down = down;
    fh1_pad_t pad;
    const uint32_t pb = fh1_pad(0, &pad) ? pad.buttons : 0;
    s_pad_edge = pb & ~s_pad_prev;
    s_pad_prev = pb;
    char dev[64];
    while (fh1_dev_command(dev, sizeof(dev)))
        if (!dev_command(dev)) fh1_log("dev: not understood here: %s", dev);
    if (s_mode == MODE_START || s_mode == MODE_FIND) {
        const int r = start_frame(px, w, h, dt);
        s_frame_ms = fh1_millis() - t0;
        return r;
    }
    player_t *me = &s_pl[s_me];
    if (s_mode == MODE_GUEST) {
        // A friend's world: what their FH1 sends; this FH1's keys go to it by themselves (the SDK).
        if (fh1_net_status() != FH1_NET_GUEST) {
            back_to_start();
            note("O mundo do seu amigo fechou");
            return FH1_FRAME_SHOW;
        }
        take_state();
        me = &s_pl[s_me];
        if (me->asking && (s_edge & (1u << FH1_KEY_A))) {           // back home
            snd(SN_BACK, 160);
            back_to_start();
            return FH1_FRAME_SHOW;
        }
        if (s_got && me->on && !s_rep.synced && me->view != s_world.view) {   // coming in: their view at once
            world_view(&s_world, me->view);
            float fx, fy;
            world_px(me->u, me->v, &fx, &fy);
            s_camx = fx - 160;
            s_camy = fy - 130;
        } else if (s_got && me->on && me->view != s_world.view && !viewturn_active(&s_turn) && !s_turn_to)
            s_turn_to = ((me->view - s_world.view) % 4 + 4) % 4;    // their view turned at the owner's: shown here
        if (s_got) steps(me, dt);
    } else {
        // One's own world: its owner here, guests over the network.
        for (int i = 1; i < NP; i++) {
            const bool here = fh1_net_status() == FH1_NET_HOSTING && fh1_net_remote(i);
            if (here && !s_pl[i].on) join(i);
            else if (!here && s_pl[i].on) leave(i);
        }
        me->edge = s_edge, me->down = s_down, me->pad_edge = s_pad_edge, me->pad = s_pad_prev;
        if (s_paused) {
            const int r = ui_menu_key(&s_menu, menu_key());
            if (r == 0 || r == UI_BACK) s_paused = false;
            else if (r == 1) {
                s_info = !s_info;
                s_paused = false;
            } else if (r == 2) {
                back_to_start();
                return FH1_FRAME_SHOW;
            }
            me->moving = false;
            me->edge = me->pad_edge = 0;
        } else if (key(me, FH1_KEY_START) || key(me, FH1_KEY_MENU)) {
            open_menu();
            me->moving = false;
        } else step(me, dt);
        for (int i = 1; i < NP; i++)
            if (s_pl[i].on) {
                guest_input(&s_pl[i], i);
                step(&s_pl[i], dt);
            }
        steps(me, dt);
        if (fh1_net_status() == FH1_NET_HOSTING) send_state();
    }
    fx_update(dt);
    gates_update(dt);
    const ui_t u = { px, w, h };
    if (s_mode == MODE_GUEST && (!s_got || !s_rep.synced || !me->on)) {   // still coming in
        draw_scene(px, w, h, dt);
        ui_banner(&u, "Entrando no mundo...", 0xf8d870);
        s_frame_ms = fh1_millis() - t0;
        return FH1_FRAME_SHOW;
    }
    if (me->inside && me->in) {                                // the room instead of the world
        draw_inside(px, w, h);
        char l[48];
        snprintf(l, sizeof(l), me->in->floors > 1 ? "andar %d de %d" : "", me->in->floor + 1, me->in->floors);
        if (l[0]) ui_text(&u, 6, 4, l, 0xe0e0e8);
        if (!s_paused && !me->asking) ui_hints(&u, 6, h - 14, me->in->floor ? "descer pela escada" : "sair pela porta", 0xe0e0e8);
    } else {
        if (s_turn_to && !viewturn_active(&s_turn)) {          // the old view once more, kept; then the new
            draw_scene(px, w, h, 0);
            viewturn_begin(&s_turn, px, w, h, s_turn_to == 3 ? -1 : 1, 340);
            apply_turn(s_turn_to);
            s_turn_to = 0;
            snd(SN_TURN, 70);
        }
        draw_scene(px, w, h, dt);
        viewturn_update(&s_turn, dt);
        viewturn_draw(&s_turn, px, w, h);
        if (me->build && !s_paused && !me->asking) draw_build_bar(&u, me);
        else if (!s_paused && !me->asking) {
            int dc, dr;
            building_t b;
            ui_hints(&u, 6, h - 14, door_ahead(me, &dc, &dr, &b) ? "{A} entrar  {X} construir" : "{X} construir", 0xe0e0e8);
        }
    }
    if (s_mode == MODE_GUEST && me->asking) {
        const int bw = 220, bh = 54, x = (w - bw) / 2, y = (h - bh) / 2;
        ui_box(&u, x, y, bw, bh);
        ui_text(&u, x + 10, y + 10, "Voltar ao seu mundo?", 0xf8d870);
        ui_hints(&u, x + 10, y + 32, "{A} sim  {B} não", 0xe0e0e8);
    }
    if (s_paused) {
        int bw, bh;
        ui_menu_size(&s_menu, &bw, &bh);
        ui_menu_draw(&u, &s_menu, (w - bw) / 2, (h - bh) / 2, s_t);
    }
    if (s_t < s_note_t) ui_text(&u, (w - (int)strlen(s_note) * 6) / 2, 6, s_note, 0xf8d870);
    if (s_info) {
        char l[40];
        snprintf(l, sizeof(l), "%u ms (max %u)  %d,%d", (unsigned)s_frame_ms, (unsigned)s_max_shown, (int)me->u, (int)me->v);
        ui_text(&u, 4, me->build ? 18 : h - 12, l, 0xffffff);            // above the bar while building
    }
    s_frame_ms = fh1_millis() - t0;
    if (s_frame_ms > s_max_ms) s_max_ms = s_frame_ms;            // the slowest frame of the last 2 s (strips drawn)
    if ((s_max_t += dt) >= 2000) {
        s_max_shown = s_max_ms;
        s_max_ms = s_max_t = 0;
    }
    if (s_quit) return FH1_FRAME_END;
    return FH1_FRAME_SHOW;
}

// Development commands (the FH1's console, the PC host's scripts): "tp U V" puts this FH1's player at a cell (in
// its own world), "info" toggles the frame time and their cell on screen; "net host", "net find", "net join N",
// "net close" (tests).
bool dev_command(const char *text)
{
    char line[64];                                                // (without the line's end)
    snprintf(line, sizeof(line), "%s", text);
    line[strcspn(line, "\r\n")] = 0;
    player_t *me = &s_pl[s_me];
    float a, b;
    int n;
    if (!strcmp(line, "net host")) {
        if (s_mode == MODE_START) play_own();
        return true;
    }
    if (!strcmp(line, "net find")) {
        if (s_mode == MODE_START) find_start();
        return true;
    }
    if (sscanf(line, "net join %d", &n) == 1) {
        if (s_mode == MODE_FIND) join_pick(n);
        return true;
    }
    if (!strcmp(line, "net close")) {
        if (s_mode == MODE_OWN || s_mode == MODE_GUEST) back_to_start();
        return true;
    }
    if (!strncmp(line, "info", 4)) {
        s_info = !s_info;
        return true;
    }
    if (s_mode != MODE_OWN) return false;
    if (sscanf(line, "tp %f %f", &a, &b) == 2) {
        me->u = a + 0.5f;
        me->v = b + 0.5f;
        float fx, fy;
        world_px(me->u, me->v, &fx, &fy);
        s_camx = fx - 160;
        s_camy = fy - 130;
        return true;
    }
    if (!strncmp(line, "log", 3)) {
        int c, r;
        target(me, &c, &r);
        // (hundredths as whole numbers: a float through printf needs double helpers the firmware does not export)
        fh1_log("hero %d,%d (1/100 cell) facing %d target %d,%d; %d change(s)", (int)(me->u * 100), (int)(me->v * 100),
                me->oct, c, r, s_world.nedits);
        for (int i = 0; i < s_world.nedits; i++)
            fh1_log("  %d,%d ground %d var %d obj %d ovar %d flags %d", s_world.edits[i].c, s_world.edits[i].r,
                    s_world.edits[i].ground, s_world.edits[i].var, s_world.edits[i].obj, s_world.edits[i].ovar,
                    s_world.edits[i].flags);
        return true;
    }
    if (!strncmp(line, "enter", 5)) {                             // in through the door they face (tests)
        int c, r;
        building_t bb;
        if (!me->inside && door_ahead(me, &c, &r, &bb)) go_in(me, c, r, &bb);
        else fh1_log("no door ahead");
        return true;
    }
    if (!strncmp(line, "leave", 5)) {
        if (me->inside) go_out(me);
        return true;
    }
    int fl;
    if (sscanf(line, "floor %d", &fl) == 1) {
        if (me->inside && fl >= 0 && fl < me->in->floors) go_floor(me, fl);
        return true;
    }
    char name[16];
    if (sscanf(line, "tool %15s", name) == 1) {                    // the build tool by name (tests)
        static const char *const ids[] = { "road", "house", "smithy", "mill", "wall", "tower", "stair", "tree", "field", "remove" };
        for (int i = 0; i < TOOLS && i < (int)(sizeof(ids) / sizeof(ids[0])); i++)
            if (!strcmp(name, ids[i])) me->tool = i;
        return true;
    }
    if (sscanf(line, "view %d", &n) == 1) {
        s_turn_to = ((n - s_world.view) % 4 + 4) % 4;
        return true;
    }
    return false;
}

static void closed(void *user)
{
    save();
    save_world();
    fh1_net_close();
    world_free(&s_world);
    viewturn_free(&s_turn);
    sfxbank_free(&s_snd);
    for (int i = 0; i < NP; i++) free(s_pl[i].in);
    fart_free(&s_art);
    fh1_app_exit();
}

int app_main(int argc, char *argv[])
{
    char path[96];
    snprintf(path, sizeof(path), "%s/assets/art.fart", fh1_app_dir());
    if (!fart_load(&s_art, path)) {
        fh1_ui_message("Construção Legal", "Faltam os gráficos do jogo (assets/art.fart).");
        return 0;
    }
    if (!world_init(&s_world, &s_art)) {
        fh1_ui_message("Construção Legal", "Sem memória para o mundo.");
        fart_free(&s_art);
        return 0;
    }
    load_icons();
    world_make(&s_world, &s_start_u, &s_start_v);
    fx_reset();
    netrep_init(&s_rep, sizeof(edit_t));
    sfxbank_load(&s_snd, "rpg-sfx", "rpg.fsfx", s_snd_names, SN_COUNT);
    ui_sound = ui_snd;
    s_in_art = (interior_art_t){ in_img("in.floor"), in_img("in.wall"), in_img("in.door"),
                                 { in_img("in.window"), in_img("in.window2") } };
    s_idle = fart_anim(&s_art, "hero.idle");
    s_walk = fart_anim(&s_art, "hero.walk");
    s_run = fart_anim(&s_art, "hero.run");
    s_swing = fart_anim(&s_art, "hero.swing");
    s_portal_ok = portal_load(&s_portal_tpl, &s_art, "portal.whirl");
    s_portal_frame = fart_image(&s_art, "portal.frame");
    s_portal_front = fart_image(&s_art, "portal.front");
    s_portal_glow = fart_image(&s_art, "portal.glow");
    for (int i = 0; i < NP; i++) player_reset(&s_pl[i]);
    s_pl[0].u = s_start_u, s_pl[0].v = s_start_v;
    float fx, fy;
    world_px(s_start_u, s_start_v, &fx, &fy);
    s_camx = fx - 160;
    s_camy = fy - 130;
    start_menu();
    for (int k = 0; k < KEYS; k++)
        if (fh1_input_is_down(k)) s_down |= 1u << k;            // a key held on entry is not a press
    if (!fh1_screen_open(33, frame, closed, NULL)) {
        fh1_ui_message("Construção Legal", "Não foi possível usar a tela.");
        world_free(&s_world);
        fart_free(&s_art);
    }
    return 0;
}
