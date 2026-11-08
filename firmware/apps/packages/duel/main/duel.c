// Duelo: two sword fighters for the FH1's two players (SDK 0.3: gamepads with motion, the canvas).
//
// With a controller that has motion, the controller is the sword: turning it on its side (gravity across it)
// raises or lowers the blade, and a real swing (a burst of rotation) strikes, harder the faster it is.
// Defence: hold a shoulder button to guard (a blow at the blade's height is blocked); press it just before the
// blow lands to parry (the attacker is stunned); the right face button (Circle) dodges back, untouchable for a
// moment. Without motion: Up/Down aim, the bottom face button (Cross) strikes. The stick or the D-pad walks.
// With no controller for player 2, the FH1 fights. A match is the best of three rounds.
// Holding Select, or the FH1's B, leaves.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "fh1_sdk.h"
#include "fighter.h"

#define W 280                          // the arena, in units
#define H 150
#define GROUND 128
#define REACH 60                       // how far a blade reaches (between the fighters' middles)
#define MIN_GAP 34                     // the fighters' stances touch
#define EDGE_X 20                      // how near the arena's sides a fighter's middle goes
#define SWING_MS 260
#define COOLDOWN_MS 450
#define STUN_MS 650
#define GUARD_DEG 35.0f                // a guard blocks a blow this close to the blade's height
#define PARRY_MS 200                   // guard pressed this long before the blow: a parry
#define DODGE_MS 240
#define DODGE_COOLDOWN_MS 900
#define SWING_RADS 5.0f                // rad/s that make a swing
#define FRAME_MS 33
#define WINS 2                         // rounds to win the match
#define PI 3.14159265f
#define DEG (PI / 180)

enum { ST_INTRO, ST_READY, ST_FIGHT, ST_KO, ST_RESULT };

typedef struct {
    float x;
    int facing;                        // +1 right, -1 left
    int hp;
    float aim;                         // the blade's angle (radians, up positive)
    uint32_t swing_at;                 // 0: not swinging
    float swing_aim, swing_power;
    bool resolved;
    bool guard;
    uint32_t guard_at;                 // when the guard went up
    uint32_t dodge_until, dodge_ready;
    uint32_t cooldown_until, stun_until, flash_until;
    float walk;                        // the step's phase
    bool moving;
    uint32_t colour;
    ftr_look_t look;
    char name[24];
    bool pad, motion, cpu, playstation;
    bool strike_was, dodge_was;
    int wins;
} fighter_t;

typedef struct {
    float x, y;
    char text[16];
    uint32_t colour, at;
} popup_t;

static fighter_t s_f[2];
static lv_obj_t *s_canvas;
static fh1_win_t *s_win;
static int s_state = ST_INTRO, s_round = 1, s_round_winner = -1;
static uint32_t s_state_at, s_select_since, s_spark_at;
static float s_spark_x, s_spark_y;
static popup_t s_pop[4];
// Each fighter's sprite, drawn again only when its pose changes (the pose rounded to what shows in pixels).
static struct {
    uint32_t *px;                    // FTR_W x FTR_H, allocated at the start, freed when the window closes
    ftr_pose_t pose;
    bool valid;
    int grip_x, grip_y;
} s_spr[2];
static struct {
    float x, y, v;
} s_petal[16];

static uint32_t now_ms(void) { return fh1_millis(); }

static void set_state(int st)
{
    s_state = st;
    s_state_at = now_ms();
}

static uint32_t shade(uint32_t rgb, float k)
{
    int r = (int)((rgb >> 16 & 255) * k), g = (int)((rgb >> 8 & 255) * k), b = (int)((rgb & 255) * k);
    r = r > 255 ? 255 : r;
    g = g > 255 ? 255 : g;
    b = b > 255 ? 255 : b;
    return (uint32_t)(r << 16 | g << 8 | b);
}

static uint32_t usable(uint32_t rgb)
{
    const int r = rgb >> 16 & 255, g = rgb >> 8 & 255, b = rgb & 255;
    return r + g + b > 240 ? rgb : 0xb0b4ba;        // too dark against the night
}

static void popup(float x, float y, const char *text, uint32_t colour)
{
    int k = 0;
    for (int i = 1; i < 4; i++) {
        if (s_pop[i].at < s_pop[k].at) k = i;
    }
    s_pop[k].x = x;
    s_pop[k].y = y;
    snprintf(s_pop[k].text, sizeof(s_pop[k].text), "%s", text);
    s_pop[k].colour = colour;
    s_pop[k].at = now_ms();
}

static void place_fighters(void)
{
    for (int i = 0; i < 2; i++) {
        fighter_t *f = &s_f[i];
        f->x = i ? W - 70 : 70;
        f->facing = i ? -1 : 1;
        f->hp = 100;
        f->aim = 0.2f;
        f->swing_at = f->stun_until = f->flash_until = f->dodge_until = f->dodge_ready = f->cooldown_until = 0;
        f->guard = false;
    }
}

static void setup_match(void)
{
    for (int i = 0; i < 2; i++) {
        fighter_t *f = &s_f[i];
        memset(f, 0, sizeof(*f));
        f->colour = i ? 0xff3c28 : 0x2a6cf0;
        f->pad = fh1_pad_info(i, f->name, sizeof(f->name), &f->colour, &f->motion);
        f->colour = usable(f->colour);
        // Player 1 spiky-haired with a red headband, player 2 with a ponytail and a white one; the gi in
        // the controller's colour, the hakama a dark shade of it.
        f->look = (ftr_look_t){ .skin = i ? 0xe6b088 : 0xf6d2b0, .hair = i ? 0x5a1018 : 0x18203a, .top = f->colour,
                                .bottom = shade(f->colour, 0.42f), .band = i ? 0xf0f0f4 : 0xd02828, .wraps = 0xf0f0f4,
                                .boots = 0x3a2a26, .eyes = 0x3c3c48, .hair_style = i ? FTR_HAIR_PONYTAIL : FTR_HAIR_SPIKY };
        s_spr[i].valid = false;
        f->playstation = strstr(f->name, "DualSense") || strstr(f->name, "DualShock") || strstr(f->name, "Wireless Controller");
        if (!f->pad) {
            f->cpu = i == 1;           // player 1 without a controller plays with the FH1's own keys
            snprintf(f->name, sizeof(f->name), "%s", i ? "FH1" : "Jogador 1");
        }
    }
    s_round = 1;
    place_fighters();
    set_state(ST_INTRO);
}

// ── Input ──

typedef struct {
    float walk, aim, strike;
    bool guard, dodge, start;
} intent_t;

static intent_t read_player(int i, uint32_t now)
{
    fighter_t *f = &s_f[i];
    intent_t in = { 0, NAN, 0, false, false, false };
    fh1_pad_t p;
    const bool have = f->pad && fh1_pad(i, &p);
    if (!have && i == 0) {             // the FH1's own keys: A strikes, X guards, Y dodges
        memset(&p, 0, sizeof(p));
        if (fh1_input_is_down(FH1_KEY_LEFT)) p.buttons |= FH1_PAD_BTN_LEFT;
        if (fh1_input_is_down(FH1_KEY_RIGHT)) p.buttons |= FH1_PAD_BTN_RIGHT;
        if (fh1_input_is_down(FH1_KEY_UP)) p.buttons |= FH1_PAD_BTN_UP;
        if (fh1_input_is_down(FH1_KEY_DOWN)) p.buttons |= FH1_PAD_BTN_DOWN;
        if (fh1_input_is_down(FH1_KEY_A)) p.buttons |= FH1_PAD_BTN_A;
        if (fh1_input_is_down(FH1_KEY_X)) p.buttons |= FH1_PAD_BTN_L1;
        if (fh1_input_is_down(FH1_KEY_Y)) p.buttons |= FH1_PAD_BTN_B;
        if (fh1_input_is_down(FH1_KEY_START)) p.buttons |= FH1_PAD_BTN_START;
    } else if (!have) {
        return in;
    }
    if (p.lx < -140 || (p.buttons & FH1_PAD_BTN_LEFT)) in.walk = -1;
    if (p.lx > 140 || (p.buttons & FH1_PAD_BTN_RIGHT)) in.walk = 1;
    in.guard = (p.buttons & (FH1_PAD_BTN_L1 | FH1_PAD_BTN_R1 | FH1_PAD_BTN_L2 | FH1_PAD_BTN_R2)) || p.l2 > 300 || p.r2 > 300;
    const bool dodge = (p.buttons & FH1_PAD_BTN_B) != 0;
    in.dodge = dodge && !f->dodge_was;
    f->dodge_was = dodge;
    const bool strike = (p.buttons & FH1_PAD_BTN_A) != 0;
    if (strike && !f->strike_was) in.strike = 12;
    f->strike_was = strike;
    in.start = (p.buttons & FH1_PAD_BTN_START) != 0 || strike;
    if (have && f->motion) {
        // The blade's height: gravity across the controller (X), as an angle.
        float s = p.accel[0] * (1 / 981.0f);
        s = s > 1 ? 1 : s < -1 ? -1 : s;
        in.aim = asinf(s) * 1.2f;
        // A swing: the rotation's size.
        const float g = sqrtf((float)p.gyro[0] * p.gyro[0] + (float)p.gyro[1] * p.gyro[1] + (float)p.gyro[2] * p.gyro[2]) * 0.001f;
        if (g > SWING_RADS) in.strike = g * 2.4f;
    } else {
        if (p.buttons & FH1_PAD_BTN_UP) in.aim = f->aim + 0.12f;
        if (p.buttons & FH1_PAD_BTN_DOWN) in.aim = f->aim - 0.12f;
    }
    // Leaving: Select held 1.5 s on player 1's controller.
    if (i == 0 && have) {
        if (p.buttons & FH1_PAD_BTN_SELECT) {
            if (!s_select_since) s_select_since = now ? now : 1;
            else if (now - s_select_since > 1500) fh1_app_exit();
        } else {
            s_select_since = 0;
        }
    }
    return in;
}

// The FH1 as player 2: close in, strike now and then, guard or dodge some blows.
static intent_t read_cpu(uint32_t now)
{
    fighter_t *me = &s_f[1], *them = &s_f[0];
    intent_t in = { 0, NAN, 0, false, false, false };
    const float gap = me->x - them->x;
    if (gap > REACH - 6) in.walk = -1;
    else if (gap < MIN_GAP + 8) in.walk = 1;
    static uint32_t next, decided;
    static int answer;                 // to this swing: 0 take it, 1 guard, 2 dodge
    if (them->swing_at) {
        if (decided != them->swing_at) {
            decided = them->swing_at;
            const int r = rand() % 10;
            answer = r < 4 ? 1 : r < 6 ? 2 : 0;
        }
        if (answer == 1) {
            in.guard = true;
            in.aim = them->swing_aim;
        }
        if (answer == 2) in.dodge = true;
    } else if (now > next) {
        in.aim = ((rand() % 140) - 70) * DEG;
    }
    if (gap < REACH && now > next && !them->swing_at) {   // gentler than a person: slower, softer blows
        in.strike = 5 + rand() % 7;
        next = now + 1100 + rand() % 1200;
    }
    return in;
}

static void apply(int i, intent_t in, uint32_t now)
{
    fighter_t *f = &s_f[i], *o = &s_f[1 - i];
    f->moving = false;
    if (now < f->stun_until) {
        f->guard = false;
        return;
    }
    // Guard: up and down; when it went up counts for a parry.
    if (in.guard && !f->guard) f->guard_at = now;
    f->guard = in.guard && !f->swing_at;
    // Dodge: a dash back, untouchable for a moment.
    if (in.dodge && now >= f->dodge_ready && !f->swing_at) {
        f->dodge_until = now + DODGE_MS;
        f->dodge_ready = now + DODGE_COOLDOWN_MS;
    }
    float step = in.walk * (f->guard ? 1.2f : 2.4f);
    if (now < f->dodge_until) step = -f->facing * 4.0f;
    if (step != 0) {
        float x = f->x + step;
        if (x < EDGE_X) x = EDGE_X;
        if (x > W - EDGE_X) x = W - EDGE_X;
        if ((i == 0 && o->x - x < MIN_GAP) || (i == 1 && x - o->x < MIN_GAP)) x = f->x;
        f->moving = x != f->x;
        f->x = x;
    }
    if (!isnan(in.aim)) {
        const float lim = 70 * DEG;
        f->aim = in.aim > lim ? lim : in.aim < -lim ? -lim : in.aim;
    }
    if (in.strike > 0 && !f->swing_at && !f->guard && now >= f->cooldown_until && now >= f->dodge_until) {
        f->swing_at = now;
        f->swing_aim = f->aim;
        f->swing_power = in.strike > 25 ? 25 : in.strike;
        f->resolved = false;
        f->cooldown_until = now + COOLDOWN_MS;
    }
}

// A swing's middle decides it: out of reach, dodged, blocked, parried, or a hit.
static void resolve(int i, uint32_t now)
{
    fighter_t *f = &s_f[i], *o = &s_f[1 - i];
    if (!f->swing_at) return;
    const uint32_t t = now - f->swing_at;
    if (t >= SWING_MS) {
        f->swing_at = 0;
        return;
    }
    if (f->resolved || t < SWING_MS / 2) return;
    f->resolved = true;
    if (fabsf(f->x - o->x) > REACH) return;
    const float px = (f->x + o->x) * 0.5f, py = GROUND - 40 - 14 * sinf(f->swing_aim);
    if (now < o->dodge_until) {
        popup(o->x, GROUND - 74, "Esquiva!", 0x9adcff);
        return;
    }
    if (o->guard && fabsf(o->aim - f->swing_aim) < GUARD_DEG * DEG) {
        s_spark_x = px;
        s_spark_y = py;
        s_spark_at = now;
        if (now - o->guard_at < PARRY_MS) {
            f->stun_until = now + STUN_MS;
            f->swing_at = 0;
            popup(px, py - 14, "Aparo!", 0xf4c21f);
        } else {
            o->hp -= 1;
            o->x += f->facing * 4;
            popup(px, py - 14, "Defesa", 0xd8dee6);
        }
        return;
    }
    s_spark_x = px;
    s_spark_y = py;
    s_spark_at = now;
    o->hp -= (int)f->swing_power;
    o->flash_until = now + 200;
    o->x += f->facing * 8;
    if (o->x < EDGE_X) o->x = EDGE_X;
    if (o->x > W - EDGE_X) o->x = W - EDGE_X;
    if (f->swing_power >= 18) popup(o->x, GROUND - 78, "Crítico!", 0xff6a4a);
    if (o->hp <= 0) {
        o->hp = 0;
        s_round_winner = i;
        f->wins++;
        set_state(ST_KO);
    }
}

// ── Drawing ──

static void text_c(int cx, int y, const char *t, uint32_t colour)
{
    const int w = fh1_canvas_text_width(s_canvas, t);
    fh1_canvas_text(s_canvas, cx - w / 2 + 1, y + 1, t, 0x000000);       // a shadow
    fh1_canvas_text(s_canvas, cx - w / 2, y, t, colour);
}

static void banner(const char *t, uint32_t colour)
{
    const int w = fh1_canvas_text_width(s_canvas, t) + 24;
    fh1_canvas_rect(s_canvas, W / 2 - w / 2, 50, w, 22, 0x0a0a14);
    fh1_canvas_rect(s_canvas, W / 2 - w / 2, 50, w, 2, colour);
    fh1_canvas_rect(s_canvas, W / 2 - w / 2, 70, w, 2, colour);
    text_c(W / 2, 56, t, colour);
}

static void draw_background(uint32_t now)
{
    // Sky: bands from deep blue to violet at the horizon.
    for (int b = 0; b < 10; b++) {
        const float k = b / 9.0f;
        const uint32_t c = (uint32_t)((int)(0x0b + k * 0x30) << 16 | (int)(0x10 + k * 0x18) << 8 | (int)(0x26 + k * 0x34));
        fh1_canvas_rect(s_canvas, 0, b * GROUND / 10, W, GROUND / 10 + 1, c);
    }
    // Stars, twinkling.
    for (int k = 0; k < 18; k++) {
        const int x = (k * 97 + 13) % W, y = (k * 53 + 7) % 60;
        if ((now / 300 + k) % 7) fh1_canvas_rect(s_canvas, x, y, 1, 1, 0xd8dcff);
    }
    // The moon.
    fh1_canvas_circle(s_canvas, W - 52, 30, 12, 0xf2ead0);
    fh1_canvas_circle(s_canvas, W - 56, 27, 3, 0xd8d0b4);
    fh1_canvas_circle(s_canvas, W - 48, 34, 2, 0xd8d0b4);
    // Mountains, far and near.
    fh1_canvas_tri(s_canvas, -20, GROUND, 60, 58, 140, GROUND, 0x2a2048);
    fh1_canvas_tri(s_canvas, 90, GROUND, 180, 50, 270, GROUND, 0x2a2048);
    fh1_canvas_tri(s_canvas, 170, GROUND, 250, 72, 320, GROUND, 0x221a3c);
    fh1_canvas_tri(s_canvas, 172, 55, 180, 50, 188, 57, 0xd8dcf0);             // snow
    // The cherry tree on the left.
    fh1_canvas_line(s_canvas, 18, GROUND, 24, 70, 3, 0x3a2418);
    fh1_canvas_line(s_canvas, 23, 82, 44, 66, 2, 0x3a2418);
    fh1_canvas_line(s_canvas, 22, 76, 6, 62, 2, 0x3a2418);
    static const int8_t bl[][3] = { { 24, 62, 11 }, { 42, 62, 9 }, { 8, 60, 8 }, { 32, 54, 9 }, { 52, 68, 6 }, { 16, 52, 7 } };
    for (int k = 0; k < 6; k++) fh1_canvas_circle(s_canvas, bl[k][0], bl[k][1], bl[k][2], k % 2 ? 0xe89ab8 : 0xf4b8cc);
    // Ground, its edge of grass.
    fh1_canvas_rect(s_canvas, 0, GROUND, W, H - GROUND, 0x1c241c);
    fh1_canvas_rect(s_canvas, 0, GROUND, W, 2, 0x34502e);
    for (int x = 3; x < W; x += 9) fh1_canvas_line(s_canvas, x, GROUND, x + 2, GROUND - 3, 1, 0x34502e);
    // Petals, drifting in the wind.
    for (int k = 0; k < 16; k++) {
        s_petal[k].x += 0.6f + s_petal[k].v;
        s_petal[k].y += 0.35f + 0.3f * s_petal[k].v;
        if (s_petal[k].x > W || s_petal[k].y > GROUND) {
            s_petal[k].x = (float)(rand() % 60) - 10;
            s_petal[k].y = (float)(rand() % 70) + 40;
            s_petal[k].v = (rand() % 100) * 0.006f;
        }
        const int sway = (int)(2 * sinf(now * 0.004f + k));
        fh1_canvas_rect(s_canvas, (int)s_petal[k].x, (int)s_petal[k].y + sway, 2, 1, 0xf4b8cc);
    }
}

// ── Fighters (fighter.c) ──

enum { BLIT_NORMAL, BLIT_FLASH, BLIT_SHADOW };

// A sprite onto the canvas, its middle at x and its soles on the ground, mirrored when facing left. Rows go as
// runs of one colour (one rectangle each). FLASH: all white but the outline (a hit); SHADOW: all one colour.
static void blit(const uint32_t *px, int x, int dir, int mode, uint32_t tint)
{
    const int oy = GROUND - 1 - FTR_FEET;
    for (int sy = 0; sy < FTR_H; sy++) {
        int run_x = 0, run_n = 0;
        uint32_t run_c = 0;
        for (int k = 0; k <= FTR_W; k++) {
            uint32_t c = 0;
            int cx = 0;
            if (k < FTR_W) {
                const int sx = dir > 0 ? k : FTR_W - 1 - k;          // left to right on the canvas
                c = px[sy * FTR_W + sx];
                cx = x + dir * (sx - FTR_CX);
                if (c && mode == BLIT_FLASH) c = (c & 0xffffff) == FTR_EDGE ? c : (FTR_SET | 0xffffff);
                if (c && mode == BLIT_SHADOW) c = FTR_SET | tint;
            }
            if (run_n && (c != run_c || cx != run_x + run_n)) {
                fh1_canvas_rect(s_canvas, run_x, sy + oy, run_n, 1, run_c & 0xffffff);
                run_n = 0;
            }
            if (!c) continue;
            if (!run_n) {
                run_x = cx;
                run_c = c;
            }
            run_n++;
        }
    }
}

// Knocked out: the same sprite turned a quarter, lying on its back with the head away from the opponent.
static void blit_lying(const uint32_t *px, int x, int dir)
{
    const int hip_row = FTR_FEET - 27, back_col = FTR_CX - 12;
    for (int sy = 0; sy < FTR_H; sy++) {
        for (int sx = 0; sx < FTR_W; sx++) {
            const uint32_t c = px[sy * FTR_W + sx];
            if (!c) continue;
            const int cy = GROUND - 1 - (sx - back_col);
            if (cy > GROUND + 1) continue;
            fh1_canvas_rect(s_canvas, x + dir * (sy - hip_row), cy, 1, 1, c & 0xffffff);
        }
    }
}

static const uint32_t *sprite(int i, const ftr_pose_t *p)
{
    if (!s_spr[i].valid || memcmp(&s_spr[i].pose, p, sizeof(*p)) != 0) {
        ftr_draw(s_spr[i].px, &s_f[i].look, p, &s_spr[i].grip_x, &s_spr[i].grip_y);
        memcpy(&s_spr[i].pose, p, sizeof(*p));
        s_spr[i].valid = true;
    }
    return s_spr[i].px;
}

static float quant(float v, float step) { return floorf(v / step + 0.5f) * step; }

// They breathe standing, step walking, shout swinging, wince when hit, blink now and then, glow guarding, blur
// dodging, see stars stunned, lie down knocked out.
static void draw_fighter(int i, uint32_t now)
{
    const fighter_t *f = &s_f[i];
    const int d = f->facing, x = (int)f->x;
    const bool ko = (s_state == ST_KO || s_state == ST_RESULT) && f->hp <= 0;
    const bool stunned = now < f->stun_until;
    const bool swinging = f->swing_at && now - f->swing_at < SWING_MS;   // (a swing cut by a knock-out ends here)
    ftr_pose_t p;
    memset(&p, 0, sizeof(p));          // the pose is compared whole, padding included
    p.breath = quant(sinf(now * 0.004f + i * 1.7f) * (f->moving ? 0.4f : 1.0f), 0.5f);
    p.step = f->moving ? quant(sinf(f->walk), 0.25f) : 0;
    p.wave = quant(sinf(now * 0.008f + i), 0.5f);
    p.steel = stunned ? 0x707880 : 0xc8d4e0;
    // The blade: at the aim, sweeping down through it in a swing, upright on guard.
    float a = f->aim;
    if (swinging) {
        const float t = (float)(now - f->swing_at) * (1.0f / SWING_MS);
        a = f->swing_aim + 0.9f - 1.8f * t;
    } else if (f->guard) {
        a = f->aim + 0.5f;
    }
    p.blade = quant(a, 0.04f);
    p.face = swinging ? FTR_FACE_SHOUT
           : (ko || stunned || now < f->flash_until || (now + i * 1300) % 3200 < 120) ? FTR_FACE_CLOSED : FTR_FACE_OPEN;
    if (ko) {
        p.no_blade = true;
        p.breath = p.step = 0;
        p.blade = 0;
        // Lying, the head reaches 44 behind the hips and the feet 30 ahead: kept inside the arena.
        const int lx = d > 0 ? (x < 46 ? 46 : x > W - 32 ? W - 32 : x) : (x > W - 46 ? W - 46 : x < 32 ? 32 : x);
        blit_lying(sprite(i, &p), lx, d);
        fh1_canvas_line(s_canvas, lx + 6 * d, GROUND - 1, lx + 36 * d, GROUND - 1, 1, 0xa0a8b0);   // the dropped blade
        return;
    }
    const uint32_t *px = sprite(i, &p);
    if (now < f->dodge_until) blit(px, x + d * 10, d, BLIT_SHADOW, shade(f->colour, 0.3f));   // the afterimage
    blit(px, x, d, now < f->flash_until ? BLIT_FLASH : BLIT_NORMAL, 0);
    // Over it: the swing's trail and the guard's glow round the hand, the stars round the head.
    const int oy = GROUND - 1 - FTR_FEET;
    const int gx = x + d * (s_spr[i].grip_x - FTR_CX), gy = s_spr[i].grip_y + oy;
    if (swinging) {
        const float t0 = f->swing_aim + 0.9f;
        for (int k = 1; k < 4; k++) {
            const float ta = t0 - (t0 - a) * k * 0.25f;
            fh1_canvas_line(s_canvas, gx + (int)(d * 25 * cosf(ta)), gy - (int)(25 * sinf(ta)), gx + (int)(d * 31 * cosf(ta)),
                            gy - (int)(31 * sinf(ta)), 1, 0x9adcff);
        }
    }
    if (f->guard) {
        for (int k = -2; k <= 2; k++) {
            const float ga = a + k * 0.18f;
            fh1_canvas_rect(s_canvas, gx + (int)(d * 34 * cosf(ga)), gy - (int)(34 * sinf(ga)), 2, 2, 0x6ab8ff);
        }
    }
    if (stunned) {
        const int hx = x + d * 1, hy = GROUND - 1 - 60;
        for (int k = 0; k < 3; k++) {
            const float sa = now * 0.01f + k * 2.1f;
            fh1_canvas_rect(s_canvas, hx + (int)(10 * cosf(sa)), hy + (int)(3 * sinf(sa)), 2, 2, 0xf4c21f);
        }
    }
}

static void draw_hud(void)
{
    for (int i = 0; i < 2; i++) {
        const fighter_t *f = &s_f[i];
        const int bw = 108, x = i ? W - 8 - bw : 8;
        fh1_canvas_rect(s_canvas, x - 1, 5, bw + 2, 8, 0xd8dee6);
        fh1_canvas_rect(s_canvas, x, 6, bw, 6, 0x301418);
        const int fill = bw * f->hp / 100;
        fh1_canvas_rect(s_canvas, i ? x + bw - fill : x, 6, fill, 6, f->colour);
        fh1_canvas_rect(s_canvas, i ? x + bw - fill : x, 6, fill, 2, shade(f->colour, 1.4f));
        fh1_canvas_text(s_canvas, i ? x + bw - fh1_canvas_text_width(s_canvas, f->name) : x, 15, f->name, 0xd8dee6);
        for (int k = 0; k < WINS; k++) {   // rounds won
            const int cx = i ? x + bw - 4 - k * 8 : x + 4 + k * 8;
            fh1_canvas_circle(s_canvas, cx, 30, 3, k < f->wins ? 0xf4c21f : 0x404858);
        }
    }
    char t[16];
    snprintf(t, sizeof(t), "R%d", s_round);
    text_c(W / 2, 6, t, 0xf4c21f);
}

// How to play, written for each player's controller.
static void draw_intro(uint32_t now)
{
    fh1_canvas_rect(s_canvas, 6, 6, W - 12, H - 12, 0x0a0a14);
    fh1_canvas_rect(s_canvas, 6, 6, W - 12, 2, 0xd02828);
    text_c(W / 2, 11, "D U E L O", 0xf4c21f);
    int y = 26;
    for (int i = 0; i < 2; i++) {
        const fighter_t *f = &s_f[i];
        char t[64];
        snprintf(t, sizeof(t), "Jogador %d: %s", i + 1, f->name);
        fh1_canvas_rect(s_canvas, 12, y + 2, 4, 4, f->colour);
        fh1_canvas_text(s_canvas, 20, y, t, f->colour);
        y += 11;
        const char *lines[4];
        int n = 0;
        if (f->cpu) {
            lines[n++] = "O FH1 luta como jogador 2";
        } else if (f->motion && f->playstation) {
            lines[n++] = "Controle de lado: altura";
            lines[n++] = "Balance (ou Cruz): golpe";
            lines[n++] = "L1/R1: defesa. Na hora: aparar";
            lines[n++] = "Círculo: esquiva";
        } else if (f->motion) {
            lines[n++] = "De lado: altura. Balance: golpe";
            lines[n++] = "Ombro: defesa. Direita: esquiva";
        } else if (f->pad) {
            lines[n++] = "Cima/Baixo: altura";
            lines[n++] = "Botão de baixo: golpe";
            lines[n++] = "Ombro: defesa. Direita: esquiva";
        } else {
            lines[n++] = "Cima/Baixo: altura. A: golpe";
            lines[n++] = "X: defesa. Y: esquiva";
        }
        for (int k = 0; k < n; k++, y += 9) fh1_canvas_text(s_canvas, 20, y, lines[k], 0xb8c0cc);
        y += 4;
    }
    if ((now / 500) % 2) text_c(W / 2, H - 20, "Start ou Cruz: lutar", 0xf4c21f);
}

static void frame(void *user)
{
    if (!s_spr[0].px) return;        // closing
    const uint32_t now = now_ms();
    const uint32_t in_state = now - s_state_at;
    intent_t in[2];
    for (int i = 0; i < 2; i++) in[i] = s_f[i].cpu ? read_cpu(now) : read_player(i, now);
    if (fh1_input_is_down(FH1_KEY_START) || fh1_input_is_down(FH1_KEY_A)) in[0].start = true;   // the FH1's own keys too
    switch (s_state) {
    case ST_INTRO:
        if (in_state > 600 && (in[0].start || in[1].start)) {
            place_fighters();
            set_state(ST_READY);
        }
        break;
    case ST_READY:
        if (in_state > 1800) set_state(ST_FIGHT);
        break;
    case ST_FIGHT:
        for (int i = 0; i < 2; i++) apply(i, in[i], now);
        for (int i = 0; i < 2; i++) resolve(i, now);
        break;
    case ST_KO:
        if (in_state > 1600) set_state(ST_RESULT);
        break;
    case ST_RESULT:
        if (in_state > 1200 && (in[0].start || in[1].start)) {
            if (s_f[0].wins >= WINS || s_f[1].wins >= WINS) {
                setup_match();
            } else {
                s_round++;
                place_fighters();
                set_state(ST_READY);
            }
        }
        break;
    }
    for (int i = 0; i < 2; i++) {
        if (s_f[i].moving) s_f[i].walk += 0.35f;
    }
    if (s_state != ST_KO) {            // each faces the other (frozen in the knock-out)
        s_f[0].facing = s_f[1].x >= s_f[0].x ? 1 : -1;
        s_f[1].facing = -s_f[0].facing;
    }

    draw_background(now);
    if (s_state == ST_INTRO) {
        draw_intro(now);
        fh1_canvas_show(s_canvas);
        return;
    }
    for (int i = 0; i < 2; i++) draw_fighter(i, now);
    if (now - s_spark_at < 160) {
        for (int k = 0; k < 8; k++) {
            const float a = k * PI / 4 + now * 0.01f;
            fh1_canvas_line(s_canvas, (int)s_spark_x, (int)s_spark_y, (int)(s_spark_x + 8 * cosf(a)), (int)(s_spark_y + 8 * sinf(a)), 1,
                            k % 2 ? 0xffffff : 0xf4c21f);
        }
    }
    for (int k = 0; k < 4; k++) {      // words that rise and fade
        const uint32_t age = now - s_pop[k].at;
        if (s_pop[k].at && age < 700) text_c((int)s_pop[k].x, (int)s_pop[k].y - (int)(age / 40), s_pop[k].text, s_pop[k].colour);
    }
    draw_hud();
    char t[48];
    switch (s_state) {
    case ST_READY:
        snprintf(t, sizeof(t), "Round %d", s_round);
        banner(in_state < 1200 ? t : "Lute!", in_state < 1200 ? 0xd8dee6 : 0xf4c21f);
        break;
    case ST_KO:
        if ((in_state / 150) % 2 == 0) banner("K.O.!", 0xff3c28);
        break;
    case ST_RESULT:
        if (s_f[s_round_winner].wins >= WINS) snprintf(t, sizeof(t), "%s vence o duelo!", s_f[s_round_winner].name);
        else snprintf(t, sizeof(t), "%s vence o round", s_f[s_round_winner].name);
        banner(t, s_f[s_round_winner].colour);
        if ((now / 500) % 2) text_c(W / 2, 80, "Start ou Cruz: continuar", 0xd8dee6);
        break;
    default: break;
    }
    fh1_canvas_show(s_canvas);
}

// The FH1's own keys go to the game (player 1 without a controller); B leaves (the window closes).
static bool on_key(fh1_win_t *win, int key, void *user) { return key != FH1_KEY_B; }

static void on_close(fh1_win_t *win, void *user)
{
    for (int i = 0; i < 2; i++) {
        free(s_spr[i].px);
        s_spr[i].px = NULL;
    }
    ftr_free();
}

int app_main(int argc, char *argv[])
{
    srand(fh1_millis());
    s_win = fh1_app_window("Duelo", W + 12, H + 34);
    s_canvas = fh1_canvas(fh1_win_body(s_win), W, H);
    s_spr[0].px = malloc(FTR_W * FTR_H * sizeof(uint32_t));
    s_spr[1].px = malloc(FTR_W * FTR_H * sizeof(uint32_t));
    if (!s_canvas || !s_spr[0].px || !s_spr[1].px) {
        on_close(s_win, NULL);
        fh1_ui_message("Duelo", "Sem memória para a arena.");
        fh1_app_exit();
        return 0;
    }
    for (int k = 0; k < 16; k++) {
        s_petal[k].x = (float)(rand() % W);
        s_petal[k].y = (float)(rand() % GROUND);
        s_petal[k].v = (rand() % 100) * 0.006f;
    }
    fh1_win_on_key(s_win, on_key, NULL);
    fh1_win_on_close(s_win, on_close, NULL);
    fh1_pad_motion(true);              // the swings
    fh1_pad_hold_keys(true);           // player 1's controller plays, not the menus
    setup_match();
    fh1_log("players: %s (%s), %s (%s)", s_f[0].name, s_f[0].motion ? "motion" : "keys", s_f[1].name,
            s_f[1].cpu ? "FH1" : s_f[1].motion ? "motion" : "keys");
    fh1_app_timer(FRAME_MS, frame, NULL);
    return 0;
}
