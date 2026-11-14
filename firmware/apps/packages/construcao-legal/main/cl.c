// Construção Legal: explore a world on foot and build in it. The protagonist walks freely in eight directions
// (the D-pad; B runs), blocked by water, trees, stones and buildings; the camera follows; things in front of him are
// cut away round him. X switches build mode: the cell he faces is the target (green: he may, red: he may not), Y
// picks the tool, A uses it (a swing of the hammer). He builds on clean grass outside the village, cuts trees and
// clears stones outside it, and removes only what he built. Start opens a menu. His place and his changes are kept.
#include "fh1_sdk.h"
#include "fart.h"
#include "ui.h"
#include "fx.h"
#include "world.h"
#include "interior.h"
#include "viewturn.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KEYS (FH1_KEY_MENU + 1)
#define WALK 70.0f                   // pixels a second on screen
#define RUN 130.0f

static fart_t s_art;
static world_t s_world;
static struct { float u, v, z; int oct; bool moving, running; uint32_t t; } s_hero;   // z: how high (pixels)
static const fart_anim_t *s_idle, *s_walk, *s_run;
static float s_camx, s_camy;
static uint32_t s_down, s_edge, s_t, s_frame_ms, s_max_ms, s_max_shown, s_max_t;
static bool s_info, s_paused, s_quit;
static ui_menu_t s_menu;
static const fart_anim_t *s_swing;
static bool key(int k);
static bool held(int k);

// ── Build mode ──

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
static const char *const s_roof[4] = { "blue", "green", "red", "wood" };
static const char *const s_roof_pt[4] = { "azul", "verde", "vermelho", "de madeira" };
static const char *const s_tower_pt[4] = { "azul", "verde", "vermelha", "de madeira" };
static int s_var[TOOLS];             // each tool's chosen variant
static int s_icon[TOOLS][VARS];      // their pictures in the bar (-1: none; remove draws a red cross)
static uint32_t s_pad_prev, s_pad_edge;   // the controller's buttons last frame, and those pressed this one

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
static void tool_name(int t, char *out, size_t n)
{
    const int v = s_var[t];
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
static bool s_build;                 // build mode on
static int s_tool;
static struct { bool on; int c, r, tool, var, rot; uint32_t t, len; } s_act;   // a swing under way, its change at the end
static uint32_t s_refuse_t;          // "can't" shown until then
// Holding A builds on: his facing (so the target's place beside him) stays as it was when A went down, and he walks
// along one map axis only (the first he moves on), kept on the middle of his row, so what he builds lines up.
static struct { bool on; int oct, axis, c, r; float line; } s_lock;   // c, r: the cell built last (once each)
static int s_since_save;
// The cell in front of him for each eighth he faces (E, SE, S, SW, W, NW, N, NE on screen).
static const int8_t s_fc[8] = { 1, 1, 1, 0, -1, -1, -1, 0 }, s_fr[8] = { -1, 0, 1, 1, 1, 0, -1, -1 };

// (His facing is on screen: the cell in front of him is found there, then turned back to the world's.)
static void target(int *c, int *r)
{
    float su, sv, u, v;
    world_to_screen(s_hero.u, s_hero.v, &su, &sv);
    world_from_screen(floorf(su) + s_fc[s_hero.oct] + 0.5f, floorf(sv) + s_fr[s_hero.oct] + 0.5f, &u, &v);
    *c = (int)floorf(u);
    *r = (int)floorf(v);
}

// Does his body (his feet's square) reach into the cell? Nothing solid goes up on him.
#define FEET 0.13f                   // his feet's half width (cells)
static bool on_him(int c, int r)
{
    const float m = FEET;
    return s_hero.u + m > c && s_hero.u - m < c + 1 && s_hero.v + m > r && s_hero.v - m < r + 1;
}

// Can a wall or a tower go on top of the wall there (his own, a level free, no tower on it yet)?
// (On a keep, only a tower: a wall on it would undo it.)
static bool stacks_on(int c, int r, int kind, int v)
{
    const cell_t *x = world_cell(&s_world, c, r);
    return x->obj == O_WALL && (x->flags & F_BUILT) && !STACK_TOWER(x) && STACK_LEVELS(x) < STACK_MAX &&
           (kind == T_TOWER || (kind == T_WALL && v != WALL_GATE && !world_is_keep(&s_world, c, r)));
}

// Stairs at (c, r): the side (0 N, 1 E, 2 S, 3 W) of a wall they climb to (a walkway: one level, nothing on it), the
// one straight ahead of him first; -1 if none.
static int stair_way(int c, int r)
{
    static const int8_t dc[4] = { 0, 1, 0, -1 }, dr[4] = { -1, 0, 1, 0 };
    const int hc = (int)floorf(s_hero.u), hr = (int)floorf(s_hero.v);
    const int ahead = c > hc ? 1 : c < hc ? 3 : r > hr ? 2 : 0;
    for (int i = 0; i < 4; i++) {
        const int d = (ahead + i) & 3;
        const float h = world_height(&s_world, c + dc[d] + 0.5f, r + dr[d] + 0.5f, WALK_H);
        if (world_inside(c + dc[d], r + dr[d]) && world_cell(&s_world, c + dc[d], r + dr[d])->obj == O_WALL && h == WALK_H)
            return d;
    }
    return -1;
}

// An upper storey can go on the player's 2 x 2 house there (one at most).
static bool storey_on(int c, int r)
{
    const cell_t *x = world_cell(&s_world, c, r);
    return x->obj == O_HOUSE && (x->flags & F_BUILT) && !(x->var & UPPER) && world_is_big(&s_world, c, r);
}

static bool allowed(int c, int r, int tool)
{
    if (!world_inside(c, r)) return false;
    const cell_t *x = world_cell(&s_world, c, r);
    const int kind = s_tools[tool].kind;
    if (x->flags & F_TOWN) return false;                          // the village is nobody's to change
    if (s_hero.z > 1) return false;                              // he builds standing on the ground
    if (kind == T_REMOVE)
        return (x->flags & F_BUILT) ? (x->obj != O_NONE && x->obj != O_TUFT) || x->ground != G_GRASS
                                    : x->obj == O_TREE || x->obj == O_STONE;
    if (stacks_on(c, r, kind, s_var[tool])) return true;
    if (kind == T_STAIR && stair_way(c, r) < 0) return false;     // stairs: against a wall's walkway
    if (kind == T_HOUSE && storey_on(c, r)) return true;
    if (kind == T_WALL && s_var[tool] == WALL_GATE && x->ground == G_ROAD && (x->obj == O_NONE || x->obj == O_TUFT))
        return world_gate_way(&s_world, c, r) >= 0 && !on_him(c, r);   // a gate across a road, the road through it
    if (x->ground != G_GRASS || (x->obj != O_NONE && x->obj != O_TUFT)) return false;   // a clean field only
    return s_tools[tool].kind == T_ROAD || s_tools[tool].kind == T_FIELD || !on_him(c, r);
}

static void save_world(void)
{
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

// What a tool makes of a cell: its ground and variant, the thing on it and its variant (built: the player's).
typedef struct { int ground, var, obj, ovar; bool built; } plan_t;
static int s_rot;                    // the building's turn (Select, L2 / R2): 0 its door toward him, then clockwise

static plan_t plan(int c, int r, int tool, int v, int rot)
{
    const cell_t *x = world_cell(&s_world, c, r);
    // A building's door toward him (the side he stands on, along the cell's main axis to him), turned by rot.
    const int dc = (int)floorf(s_hero.u) - c, dr = (int)floorf(s_hero.v) - r;
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
        return (plan_t){ G_GRASS, 0, O_WALL, v | (face & 1) << 2, true };                      // alone: its face to him
    case T_STAIR: return (plan_t){ G_GRASS, 0, O_STAIR, stair_way(c, r) < 0 ? 0 : stair_way(c, r), true };
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
static void apply(int c, int r, int tool, int v, int rot)
{
    const plan_t p = plan(c, r, tool, v, rot);
    if (p.obj != world_cell(&s_world, c, r)->obj) world_unjoin(&s_world, c, r);   // a piece taken off: the rest keep theirs
    world_set(&s_world, c, r, p.ground, p.var, p.obj, p.ovar, p.built);
    world_merge(&s_world, c, r);
    world_spread(&s_world, c, r);                                  // a tower on a keep, a storey: on all four cells
    float fx, fy;
    world_px(c + 0.5f, r + 0.5f, &fx, &fy);
    fx_burst(fx, fy - 4, 0xce59, 0x9cd3, 26, 45);                   // dust
    if (++s_since_save >= 8) save_world();
}

// Starts a swing at the target if the tool may be used there (false: not).
static bool start_swing(void)
{
    int c, r;
    target(&c, &r);
    if (!allowed(c, r, s_tool)) return false;
    s_act.on = true;
    s_act.c = s_lock.c = c;
    s_act.r = s_lock.r = r;
    s_act.tool = s_tool;
    s_act.var = s_var[s_tool];
    s_act.rot = s_rot;
    s_act.t = 0;
    s_act.len = s_swing ? (uint32_t)s_swing->frames * s_swing->ms : 500;
    return true;
}

static void build_input(void)
{
    if (key(FH1_KEY_Y)) s_tool = (s_tool + 1) % TOOLS;
    const uint32_t pe = s_pad_edge;
    const int n = s_tools[s_tool].n;
    if (pe & FH1_PAD_BTN_R1) s_var[s_tool] = (s_var[s_tool] + 1) % n;
    if (pe & FH1_PAD_BTN_L1) s_var[s_tool] = (s_var[s_tool] + n - 1) % n;
    if ((pe & FH1_PAD_BTN_R2) || key(FH1_KEY_SELECT)) s_rot = (s_rot + 1) % 4;
    if (pe & FH1_PAD_BTN_L2) s_rot = (s_rot + 3) % 4;
    if (key(FH1_KEY_A)) {
        s_lock.c = s_lock.r = -1;
        s_lock.on = true;
        s_lock.oct = s_hero.oct;
        s_lock.axis = -1;
        if (!s_act.on && !start_swing()) s_refuse_t = s_t + 900;
    } else if (s_lock.on && held(FH1_KEY_A) && !s_act.on) {
        int c, r;
        target(&c, &r);
        if (c != s_lock.c || r != s_lock.r) start_swing();        // the next cell, once he has moved on to it
    }
    if (!held(FH1_KEY_A)) s_lock.on = false;
}

// What the tool would make at the target, see-through (red where it may not): the ground as it would join, the
// building turned as it would stand. Remove shows none.
static void draw_ghost(uint16_t *px, int w, int h, int camx, int camy)
{
    if (s_act.on || s_tools[s_tool].kind == T_REMOVE) return;
    int c, r;
    target(&c, &r);
    const bool ok = allowed(c, r, s_tool);
    const plan_t p = plan(c, r, s_tool, s_var[s_tool], s_rot);
    world_ghost(&s_world, px, w, h, camx, camy, c, r, p.ground, p.var, p.obj, p.ovar, ok ? 150 : 110, 0xf9a6, ok ? 0 : 150);
}

// Is the target in front of him (drawn after him)?
static bool target_in_front(void)
{
    int c, r;
    target(&c, &r);
    float tu, tv, su, sv;
    world_to_screen(c + 0.5f, r + 0.5f, &tu, &tv);
    world_to_screen(s_hero.u, s_hero.v, &su, &sv);
    return tu + tv > su + sv;
}

// The target cell's top face outlined: green where he may, red where he may not.
static void draw_target(uint16_t *px, int w, int h, int camx, int camy)
{
    int c, r;
    target(&c, &r);
    const uint16_t col = allowed(c, r, s_tool) ? 0x87f0 : 0xf9a6;
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
static void draw_build_bar(const ui_t *u)
{
    const int gap = 3, bw = TOOLS * (SLOT + gap) - gap + 16, y0 = u->h - 70, x0 = (u->w - bw) / 2 + 8;
    ui_box(u, x0 - 8, y0, bw, 66);
    for (int i = 0; i < TOOLS; i++) {
        const int x = x0 + i * (SLOT + gap), y = y0 + 6, ic = s_icon[i][s_var[i]];
        const bool sel = i == s_tool;
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
                fill(u, x + SLOT / 2 - s_tools[i].n * 2 + v * 4 + 1, y + SLOT - 3, 2, 2, v == s_var[i] ? 0xffff : 0x6b4d);
    }
    char l[48];
    tool_name(s_tool, l, sizeof(l));
    const int ty = y0 + SLOT + 14, right = x0 + bw - 16;
    ui_text(u, x0, ty, l, s_t < s_refuse_t ? 0xff8070 : 0xf8d870);
    if (s_t < s_refuse_t) ui_text(u, right - 64, ty, "aqui não", 0xff8070);
    else if (s_tools[s_tool].n > 1) {
        snprintf(l, sizeof(l), "{LR} %d/%d", s_var[s_tool] + 1, s_tools[s_tool].n);
        ui_hints(u, right - ui_hints_width(l), ty, l, 0xe0e0e8);
    }
    ui_hints(u, x0, ty + 13, "{Y} trocar {s} girar {A} fazer {X} sair", 0xe0e0e8);
}

static bool key(int k) { return s_edge & (1u << k); }
static bool held(int k) { return s_down & (1u << k); }

// His place, his facing (on screen) and the view; "CL01" saves had no view (the first one).
typedef struct { uint32_t magic; float u, v; uint8_t oct, view, z; } save_t;   // z: how high (on a wall)
#define SAVE_MAGIC 0x434c3032u       // "CL02"
#define SAVE_MAGIC1 0x434c3031u      // "CL01"

static void save(void)
{
    const save_t s = { SAVE_MAGIC, s_hero.u, s_hero.v, (uint8_t)s_hero.oct, (uint8_t)s_world.view, (uint8_t)s_hero.z };
    fh1_state_save("lugar", &s, sizeof(s));
}

// Turns the view a quarter (+1 clockwise, -1 back): he keeps facing the same way in the world. The next frame
// shows the old view once more, kept for the turn (the engine's viewturn.h), then the new one turning in.
static viewturn_t s_turn;
static int s_turn_to;                                          // turns asked for: the quarters, 0 none
static void turn_view(int d)
{
    s_turn_to = ((s_turn_to + d) % 4 + 4) % 4;                 // those asked during a turn: once it ends
}

static void apply_turn(int d)
{
    world_view(&s_world, (s_world.view + d) % 4);
    s_hero.oct = (s_hero.oct + 2 * d) % 8;
    float fx, fy;
    world_px(s_hero.u, s_hero.v, &fx, &fy);
    s_camx = fx - 160;
    s_camy = fy - 130;
}

// Can he stand at (u, v)? Where he stands (his middle) and his feet's four corners all within a step of how high he
// is: up stairs, along a wall's walkway, but not off its edge. *h: how high he would be.
#define STEP_UP 7.0f
static bool fits(float u, float v, float *h)
{
    const float hc = world_height(&s_world, u, v, s_hero.z);
    if (hc < 0 || fabsf(hc - s_hero.z) > STEP_UP) return false;
    const float m = FEET;
    const float cu[4] = { u - m, u + m, u - m, u + m }, cv[4] = { v - m, v - m, v + m, v + m };
    for (int k = 0; k < 4; k++) {
        const float hk = world_height(&s_world, cu[k], cv[k], hc);
        if (hk < 0 || fabsf(hk - hc) > STEP_UP) return false;
    }
    *h = hc;
    return true;
}

// One step along the D-pad's direction on screen, turned into the map's axes (sliding along what blocks).
static void walk(uint32_t dt)
{
    float sx = (held(FH1_KEY_RIGHT) ? 1.0f : 0.0f) - (held(FH1_KEY_LEFT) ? 1.0f : 0.0f);
    float sy = (held(FH1_KEY_DOWN) ? 1.0f : 0.0f) - (held(FH1_KEY_UP) ? 1.0f : 0.0f);
    s_hero.moving = sx != 0 || sy != 0;
    s_hero.running = s_hero.moving && held(FH1_KEY_B);
    if (!s_hero.moving) return;
    // A diagonal on the D-pad is a map axis: along a row or a column of cells (24 across for 14 down on screen), so
    // he walks straight along a road and what he builds lines up.
    if (sx != 0 && sy != 0) {
        sx *= TW / 2;
        sy *= TH / 2;
    }
    const float len = sqrtf(sx * sx + sy * sy);
    sx /= len;
    sy /= len;
    // Facing: the eighth of the direction on screen (clockwise from east, y down); held while building on.
    if (!s_lock.on) s_hero.oct = ((int)floorf(atan2f(sy, sx) / 0.785398f + 0.5f) + 8) % 8;
    const float px = (s_hero.running ? RUN : WALK) * dt / 1000.0f;
    // Screen pixels to the screen's cells: x = (u - v) * TW/2, y = (u + v) * TH/2; then turned to the world's.
    float du = (sx * px / (TW / 2) + sy * px / (TH / 2)) / 2, dv = (sy * px / (TH / 2) - sx * px / (TW / 2)) / 2;
    float su, sv;
    world_to_screen(s_hero.u, s_hero.v, &su, &sv);
    if (s_lock.on) {                                              // along one axis, drawn to the middle of the row
        if (s_lock.axis < 0) {
            s_lock.axis = fabsf(du) >= fabsf(dv) ? 0 : 1;
            s_lock.line = floorf(s_lock.axis == 0 ? sv : su) + 0.5f;
        }
        const float step = fabsf(du) + fabsf(dv);
        float *across = s_lock.axis == 0 ? &dv : &du;
        const float off = s_lock.line - (s_lock.axis == 0 ? sv : su);
        if (s_lock.axis == 0) du = du >= 0 ? step : -step; else dv = dv >= 0 ? step : -step;
        *across = fabsf(off) < step ? off : (off > 0 ? step : -step);
    }
    {
        float wu, wv;
        world_from_screen(su + du, sv + dv, &wu, &wv);
        du = wu - s_hero.u;
        dv = wv - s_hero.v;
    }
    const float nu = s_hero.u + du, nv = s_hero.v + dv;
    float h;
    #define FREE(U, V) fits((U), (V), &h)
    if (FREE(nu, nv)) s_hero.u = nu, s_hero.v = nv;
    else if (fabsf(du) > 1e-4f && FREE(nu, s_hero.v)) s_hero.u = nu;        // slide along it
    else if (fabsf(dv) > 1e-4f && FREE(s_hero.u, nv)) s_hero.v = nv;
    else {                                                                  // a corner: eased sideways, into a gap
        const float n = sqrtf(du * du + dv * dv), pu = -dv / n * 0.04f, pv = du / n * 0.04f;
        if (FREE(nu + pu, nv + pv)) s_hero.u = nu + pu, s_hero.v = nv + pv;
        else if (FREE(nu - pu, nv - pv)) s_hero.u = nu - pu, s_hero.v = nv - pv;
        else return;
    }
    s_hero.z = h;
}

// ── Inside a building (the engine's interior.h) ──

static bool s_inside;
static interior_t s_in;
static interior_art_t s_in_art;
static building_t s_bld;
static int s_door_c, s_door_r;                // the cell he went in by
static int8_t s_room[IN_GRID * IN_GRID];      // the ground floor's rooms: each one's cell in s_bld (-1: none)
static uint8_t s_busy[IN_ROWS][IN_COLS];      // tiles kept clear or taken while furnishing

static int in_img(const char *name) { return fart_image(&s_art, name); }

// The building he faces (not building, on the ground), by any of its cells: the one ahead, or when he faces a
// corner, either cell beside it. *c, *r: the cell.
static bool door_ahead(int *c, int *r, building_t *b)
{
    if (s_build || s_hero.z > 1) return false;
    int tc, tr;
    target(&tc, &tr);
    const int hc = (int)floorf(s_hero.u), hr = (int)floorf(s_hero.v);
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

// The ground floor laid out like the building as he saw it going in: a room for each cell where it is on screen
// (at most IN_GRID x IN_GRID round the one he went in by, his entry room), a doorway wherever two cells join.
static void plan_ground(interior_plan_t *p)
{
    int sc[GROUP_MAX], sr[GROUP_MAX], g[GROUP_MAX], ec = 0, er = 0, c0 = 1 << 20, r0 = 1 << 20;
    for (int i = 0; i < s_bld.n; i++) {
        float su, sv;
        world_to_screen(s_bld.c[i] + 0.5f, s_bld.r[i] + 0.5f, &su, &sv);
        sc[i] = (int)floorf(su), sr[i] = (int)floorf(sv);
        if (sc[i] < c0) c0 = sc[i];
        if (sr[i] < r0) r0 = sr[i];
        if (s_bld.c[i] == s_door_c && s_bld.r[i] == s_door_r) ec = sc[i], er = sr[i];
    }
    if (ec - c0 >= IN_GRID) c0 = ec - IN_GRID + 1;
    if (er - r0 >= IN_GRID) r0 = er - IN_GRID + 1;
    memset(p, 0, sizeof(*p));
    memset(s_room, -1, sizeof(s_room));
    for (int i = 0; i < s_bld.n; i++) {
        const int gx = sc[i] - c0, gy = sr[i] - r0;
        g[i] = gx >= 0 && gy >= 0 && gx < IN_GRID && gy < IN_GRID ? gx + IN_GRID * gy : -1;
        if (g[i] >= 0) p->cells |= (uint16_t)(1u << g[i]), s_room[g[i]] = (int8_t)i;
    }
    static const int8_t dc[4] = { 0, 1, 0, -1 }, dr[4] = { -1, 0, 1, 0 };
    for (int i = 0; i < s_bld.n; i++)
        for (int k = 0; k < 4; k++) {
            if (g[i] < 0 || !(s_bld.joins[i] >> k & 1)) continue;
            for (int j = 0; j < s_bld.n; j++) {
                if (g[j] < 0 || s_bld.c[j] != s_bld.c[i] + dc[k] || s_bld.r[j] != s_bld.r[i] + dr[k]) continue;
                if (sc[j] == sc[i] + 1) p->join_r |= (uint16_t)(1u << g[i]);
                if (sr[j] == sr[i] + 1) p->join_d |= (uint16_t)(1u << g[i]);
            }
        }
    p->entry = (ec - c0) + IN_GRID * (er - r0);
    p->open = s_bld.open;
}

// A floor's plan: the ground as the building is, an upper floor one room (its stairs down where they come up).
static void plan_floor(int f, interior_plan_t *p)
{
    if (f == 0) {
        plan_ground(p);
        return;
    }
    memset(p, 0, sizeof(*p));
    memset(s_room, -1, sizeof(s_room));
    p->cells = 1;
    p->entry = -1;
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

static void put_piece(const piece_t *pc, int x0, int top, int w)
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
                ok = s_in.tile[ty * IN_COLS + tx] == IT_FLOOR && (depth <= 0 || !s_busy[ty][tx]);
                if (pc->back) ok = ok && s_in.tile[(ty - 1) * IN_COLS + tx] == IT_WALL;
            }
        if (!ok) continue;
        if (depth > 0) keep_clear(tx0, ty0, tx1 - tx0 + 1, ty1 - ty0 + 1);
        interior_add(&s_in, &s_art, img, x0 + x, y1 - ih, depth);
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
static void furnish(void)
{
    memset(s_busy, 0, sizeof(s_busy));
    const interior_plan_t *p = &s_in.plan;
    for (int g = 0; g < IN_GRID * IN_GRID; g++) {
        int x, y, w, h;
        if (!interior_room(&s_in, g % IN_GRID, g / IN_GRID, &x, &y, &w, &h)) continue;
        keep_clear(x, y + 1, w, 1);                                      // the passage
        if (s_in.door_x >= x && s_in.door_x < x + w) keep_clear(s_in.door_x - 1, y, 4, 1);
        if (g >= IN_GRID && (p->join_d >> (g - IN_GRID) & 1)) keep_clear(x + IN_PASS, y, 2, 1);
        if (p->join_d >> g & 1) keep_clear(x + IN_PASS, y + 1, 2, h);
        if (p->join_r >> g & 1) keep_clear(x + w - 1, y + 1, 1, 2);
        if (g % IN_GRID && (p->join_r >> (g - 1) & 1)) keep_clear(x, y + 1, 1, 2);
    }
    int others = 0;
    for (int g = 0; g < IN_GRID * IN_GRID; g++) {
        int x, y, w, h;
        if (!interior_room(&s_in, g % IN_GRID, g / IN_GRID, &x, &y, &w, &h)) continue;
        const int i = s_room[g];
        const bool forge = i >= 0 && world_cell(&s_world, s_bld.c[i], s_bld.r[i])->obj == O_SMITH;
        const bool keep = s_bld.kind == B_KEEP;
        int role;
        if (s_in.floor > 0) role = keep ? R_STUDY : R_BED;
        else if (forge) role = R_FORGE;
        else if (g == p->entry) role = keep ? R_KEEP : R_HALL;
        else role = (int[]){ R_BED, R_KITCHEN, R_STORE }[others++ % 3];
        for (int k = 0; k < 8 && s_pieces[role][k].name; k++) put_piece(&s_pieces[role][k], x, y, w);
    }
}

static void go_in(int c, int r, const building_t *b)
{
    // Its colours for what has no picture: a stone keep, a wooden or white house.
    static const uint16_t wood[IN_COLOURS] = { 0x7a69, 0x6207, 0xb48c, 0x3164, 0x8a86 };
    static const uint16_t stone[IN_COLOURS] = { 0x7bcf, 0x632c, 0x9cf3, 0x31a6, 0x6b4d };
    s_bld = *b;
    s_door_c = c, s_door_r = r;
    interior_plan_t p;
    plan_floor(0, &p);
    interior_enter(&s_in, b->floors, &p, b->kind == B_KEEP ? stone : wood);
    s_in_art = (interior_art_t){ in_img("in.floor"), in_img("in.wall"), in_img("in.door"),
                                 { in_img("in.window"), in_img("in.window2") } };
    furnish();
    s_inside = true;
    s_hero.oct = FART_S8;
}

static void go_floor(int f)
{
    interior_plan_t p;
    plan_floor(f, &p);
    interior_floor(&s_in, f, &p);
    furnish();
}

// Back out where he stood going in, facing away from the building.
static void go_out(void)
{
    s_inside = false;
    s_hero.z = 0;
    float su, sv, tu, tv;
    world_to_screen(s_door_c + 0.5f, s_door_r + 0.5f, &tu, &tv);
    world_to_screen(s_hero.u, s_hero.v, &su, &sv);
    const float x = (su - tu) - (sv - tv), y = (su - tu) + (sv - tv);
    s_hero.oct = ((int)floorf(atan2f(y * 14, x * 24) / 0.785398f + 0.5f) + 8) % 8;
}

static void inside_input(uint32_t dt)
{
    float sx = (held(FH1_KEY_RIGHT) ? 1.0f : 0.0f) - (held(FH1_KEY_LEFT) ? 1.0f : 0.0f);
    float sy = (held(FH1_KEY_DOWN) ? 1.0f : 0.0f) - (held(FH1_KEY_UP) ? 1.0f : 0.0f);
    s_hero.moving = sx != 0 || sy != 0;
    if (!s_hero.moving) return;
    const float len = sqrtf(sx * sx + sy * sy), sp = (held(FH1_KEY_B) ? 7.0f : 4.0f) * dt / 1000.0f;
    s_hero.oct = ((int)floorf(atan2f(sy, sx) / 0.785398f + 0.5f) + 8) % 8;
    const int ev = interior_move(&s_in, &s_art, sx / len * sp, sy / len * sp);
    if (ev == IN_EXIT) go_out();
    else if (ev == IN_UP || ev == IN_DOWN) go_floor(s_in.floor + (ev == IN_UP ? 1 : -1));
}

static void draw_walker(uint16_t *px, int w, int h, int x, int y, void *user)
{
    (void)user;
    const fart_anim_t *a = s_hero.moving ? s_walk : s_idle;
    bool mirror;
    const int i = fart_frame8(a, s_hero.oct, s_hero.t, &mirror);
    if (i < 0) return;
    const fart_clip_t clip = { 0, 0, w, h };
    fart_draw(px, w, &clip, &s_art.img[i], x, y, mirror);
}

static void draw_hero(uint16_t *px, int w, int h, int camx, int camy)
{
    float fx, fy;
    world_px(s_hero.u, s_hero.v, &fx, &fy);
    const int x = (int)floorf((fx) + 0.5f) - camx, y = (int)floorf((fy - s_hero.z) + 0.5f) - camy;   // up as he climbs
    const fart_anim_t *a = s_act.on && s_swing ? s_swing : s_hero.moving ? (s_hero.running && s_run ? s_run : s_walk) : s_idle;
    bool mirror;
    const int i = fart_frame8(a, s_hero.oct, s_act.on ? s_act.t : s_hero.t, &mirror);
    if (i < 0) return;
    const fart_img_t *im = &s_art.img[i];
    const fart_clip_t screen = { 0, 0, w, h };
    fart_draw(px, w, &screen, im, x, y, mirror);
    // What stands in front of him, drawn again over him, and cut away round him.
    const fart_clip_t box = { x + (mirror ? -im->ox - im->w : im->ox), y + im->oy,
                              x + (mirror ? -im->ox : im->ox + im->w), y + im->oy + im->h };
    world_cover(&s_world, px, w, h, camx, camy, s_hero.u, s_hero.v, &box);
}

static void open_menu(void)
{
    memset(&s_menu, 0, sizeof(s_menu));
    s_menu.title = "Construção Legal";
    s_menu.items[0] = "Continuar";
    s_menu.items[1] = s_info ? "Esconder tempos" : "Mostrar tempos";
    s_menu.items[2] = "Sair";
    s_menu.n = 3;
    s_paused = true;
}

static int menu_key(void)
{
    if (key(FH1_KEY_UP)) return FH1_KEY_UP;
    if (key(FH1_KEY_DOWN)) return FH1_KEY_DOWN;
    if (key(FH1_KEY_A)) return FH1_KEY_A;
    if (key(FH1_KEY_B)) return FH1_KEY_B;
    return -1;
}

bool dev_command(const char *line);

// The world, him, what he builds, smoke and sparks; the camera easing toward him (his feet a little below the
// middle).
static void draw_scene(uint16_t *px, int w, int h, uint32_t dt)
{
    float fx, fy;
    world_px(s_hero.u, s_hero.v, &fx, &fy);
    const float tx = fx - w / 2, ty = fy - h / 2 - 10;
    float k = dt / 140.0f;
    if (k > 1) k = 1;
    s_camx += (tx - s_camx) * k;
    s_camy += (ty - s_camy) * k;
    const int camx = (int)floorf((s_camx) + 0.5f), camy = (int)floorf((s_camy) + 0.5f);
    world_tick(&s_world, dt);
    world_draw(&s_world, px, w, h, camx, camy);
    if (s_build) draw_target(px, w, h, camx, camy);
    const bool ghost_after = s_build && !s_paused && target_in_front();
    if (s_build && !s_paused && !ghost_after) draw_ghost(px, w, h, camx, camy);
    draw_hero(px, w, h, camx, camy);
    if (ghost_after) draw_ghost(px, w, h, camx, camy);
    world_smoke(&s_world, px, w, h, camx, camy);
    fx_draw(&s_art, px, w, h, camx, camy);
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
    char dev[64];
    while (fh1_dev_command(dev, sizeof(dev)))
        if (!dev_command(dev)) fh1_log("dev: not understood here: %s", dev);
    if (s_paused) {
        const int r = ui_menu_key(&s_menu, menu_key());
        if (r == 0 || r == UI_BACK) s_paused = false;
        else if (r == 1) {
            s_info = !s_info;
            s_paused = false;
        } else if (r == 2) s_quit = true;
    } else {
        if (key(FH1_KEY_START) || key(FH1_KEY_MENU)) open_menu();
        fh1_pad_t pad;
        const uint32_t pb = fh1_pad(0, &pad) ? pad.buttons : 0;
        s_pad_edge = pb & ~s_pad_prev;
        s_pad_prev = pb;
        int dc, dr;
        building_t b;
        if (s_inside) {
            inside_input(dt);
            s_hero.t += dt;
            goto drawn;
        }
        if (key(FH1_KEY_A) && !s_act.on && door_ahead(&dc, &dr, &b)) {   // into the building he faces
            go_in(dc, dr, &b);
            goto drawn;
        }
        if (key(FH1_KEY_X) && !s_act.on) s_build = !s_build;
        if (s_build) build_input();
        else if (!s_act.on) {                                     // the view: Select or R1 a quarter on, L1 back
            if (key(FH1_KEY_SELECT) || (s_pad_edge & FH1_PAD_BTN_R1)) turn_view(1);
            if (s_pad_edge & FH1_PAD_BTN_L1) turn_view(-1);
        }
        if (s_act.on) {
            s_hero.moving = false;
            if ((s_act.t += dt) >= s_act.len) {
                s_act.on = false;
                apply(s_act.c, s_act.r, s_act.tool, s_act.var, s_act.rot);
            }
        } else walk(dt);
    }
    fx_update(dt);
    s_hero.t += dt;
drawn:
    if (s_inside) {                                            // the room instead of the world
        const ui_t ui = { px, w, h };
        interior_draw(&s_in, &s_art, &s_in_art, px, w, h, draw_walker, NULL);
        char l[48];
        snprintf(l, sizeof(l), s_in.floors > 1 ? "andar %d de %d" : "", s_in.floor + 1, s_in.floors);
        if (l[0]) ui_text(&ui, 6, 4, l, 0xe0e0e8);
        if (!s_paused) ui_hints(&ui, 6, h - 14, s_in.floor ? "descer pela escada" : "sair pela porta", 0xe0e0e8);
        else {
            int bw, bh;
            ui_menu_size(&s_menu, &bw, &bh);
            ui_menu_draw(&ui, &s_menu, (w - bw) / 2, (h - bh) / 2, s_t);
        }
        s_frame_ms = fh1_millis() - t0;
        return s_quit ? FH1_FRAME_END : FH1_FRAME_SHOW;
    }
    if (s_turn_to && !viewturn_active(&s_turn)) {              // the old view once more, kept; then the new
        draw_scene(px, w, h, 0);
        viewturn_begin(&s_turn, px, w, h, s_turn_to == 3 ? -1 : 1, 340);
        apply_turn(s_turn_to);
        s_turn_to = 0;
    }
    draw_scene(px, w, h, dt);
    viewturn_update(&s_turn, dt);
    viewturn_draw(&s_turn, px, w, h);
    const ui_t u = { px, w, h };
    if (s_build && !s_paused) draw_build_bar(&u);
    else if (!s_paused) {
        int dc, dr;
        building_t b;
        ui_hints(&u, 6, h - 14, door_ahead(&dc, &dr, &b) ? "{A} entrar  {X} construir" : "{X} construir", 0xe0e0e8);
    }
    if (s_paused) {
        int bw, bh;
        ui_menu_size(&s_menu, &bw, &bh);
        ui_menu_draw(&u, &s_menu, (w - bw) / 2, (h - bh) / 2, s_t);
    }
    if (s_info) {
        char l[40];
        snprintf(l, sizeof(l), "%u ms (max %u)  %d,%d", (unsigned)s_frame_ms, (unsigned)s_max_shown, (int)s_hero.u, (int)s_hero.v);
        ui_text(&u, 4, s_build ? 4 : h - 12, l, 0xffffff);            // above the bar while building
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

// Development commands (the FH1's console, the PC host's scripts): "tp U V" puts him at a cell, "info" toggles the
// frame time and his cell on screen.
bool dev_command(const char *line)
{
    float a, b;
    if (sscanf(line, "tp %f %f", &a, &b) == 2) {
        s_hero.u = a + 0.5f;
        s_hero.v = b + 0.5f;
        float fx, fy;
        world_px(s_hero.u, s_hero.v, &fx, &fy);
        s_camx = fx - 160;
        s_camy = fy - 130;
        return true;
    }
    if (!strncmp(line, "log", 3)) {
        int c, r;
        target(&c, &r);
        // (hundredths as whole numbers: a float through printf needs double helpers the firmware does not export)
        fh1_log("hero %d,%d (1/100 cell) facing %d target %d,%d; %d change(s)", (int)(s_hero.u * 100), (int)(s_hero.v * 100),
                s_hero.oct, c, r, s_world.nedits);
        for (int i = 0; i < s_world.nedits; i++)
            fh1_log("  %d,%d ground %d var %d obj %d ovar %d flags %d", s_world.edits[i].c, s_world.edits[i].r,
                    s_world.edits[i].ground, s_world.edits[i].var, s_world.edits[i].obj, s_world.edits[i].ovar,
                    s_world.edits[i].flags);
        return true;
    }
    if (!strncmp(line, "enter", 5)) {                             // in through the door he faces (tests)
        int c, r;
        building_t b;
        if (!s_inside && door_ahead(&c, &r, &b)) go_in(c, r, &b);
        else fh1_log("no door ahead");
        return true;
    }
    if (!strncmp(line, "leave", 5)) {
        if (s_inside) go_out();
        return true;
    }
    int fl;
    if (sscanf(line, "floor %d", &fl) == 1) {
        if (s_inside && fl >= 0 && fl < s_in.floors) go_floor(fl);
        return true;
    }
    char name[16];
    if (sscanf(line, "tool %15s", name) == 1) {                    // the build tool by name (tests)
        static const char *const ids[] = { "road", "house", "smithy", "mill", "wall", "tower", "stair", "tree", "field", "remove" };
        for (int i = 0; i < TOOLS && i < (int)(sizeof(ids) / sizeof(ids[0])); i++)
            if (!strcmp(name, ids[i])) s_tool = i;
        return true;
    }
    int n;
    if (sscanf(line, "view %d", &n) == 1) {
        s_turn_to = ((n - s_world.view) % 4 + 4) % 4;
        return true;
    }
    if (!strncmp(line, "info", 4)) {
        s_info = !s_info;
        return true;
    }
    return false;
}

static void closed(void *user)
{
    save();
    save_world();
    world_free(&s_world);
    viewturn_free(&s_turn);
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
    world_make(&s_world, &s_hero.u, &s_hero.v);
    load_world();
    fx_reset();
    s_hero.oct = FART_SE8;
    save_t s;
    memset(&s, 0, sizeof(s));
    const int got = fh1_state_load("lugar", &s, sizeof(s));
    if (s.magic == SAVE_MAGIC1) s.view = s.z = 0;
    const float hz = world_height(&s_world, s.u, s.v, s.z);
    if (((got == (int)sizeof(s) && s.magic == SAVE_MAGIC) || (got >= 13 && s.magic == SAVE_MAGIC1)) &&
        hz >= 0 && fabsf(hz - s.z) <= STEP_UP) {
        s_hero.z = hz;
        s_hero.u = s.u;
        s_hero.v = s.v;
        s_hero.oct = s.oct % 8;
        if (s.view) world_view(&s_world, s.view);
    }
    s_idle = fart_anim(&s_art, "hero.idle");
    s_walk = fart_anim(&s_art, "hero.walk");
    s_run = fart_anim(&s_art, "hero.run");
    s_swing = fart_anim(&s_art, "hero.swing");
    float fx, fy;
    world_px(s_hero.u, s_hero.v, &fx, &fy);
    s_camx = fx - 160;
    s_camy = fy - 130;
    for (int k = 0; k < KEYS; k++)
        if (fh1_input_is_down(k)) s_down |= 1u << k;            // a key held on entry is not a press
    if (!fh1_screen_open(33, frame, closed, NULL)) {
        fh1_ui_message("Construção Legal", "Não foi possível usar a tela.");
        world_free(&s_world);
        fart_free(&s_art);
    }
    return 0;
}
