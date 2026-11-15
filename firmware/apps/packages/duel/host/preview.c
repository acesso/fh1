// Duelo's fighters on a computer, each beside the FH1 avatar that dresses it: the two fighters of players without a
// user, then avatars that go through every hair style, top, bottom and accessory. One PPM sheet at 3x.
//
//   cc -O2 -Wall -I main -I ../../../components/fh1_avatar/include -I ../../../components/fh1_sdk/include -o preview
//      host/preview.c main/fighter.c ../../../components/fh1_avatar/avatar.c -lm
//   ./preview sheet.ppm                        (from the package's folder)
#include <stdio.h>
#include <string.h>
#include "fighter.h"
#include "fh1_avatar.h"

#define CW 190
#define CH 140
#define COLS 5
#define ROWS 3
#define K 3

static uint32_t s_sheet[ROWS * CH * COLS * CW];

// fh1_user_look's mapping (components/fh1_apps/apps_play.c), for the preview.
static void look_of(const fh1_avatar_t *a, fh1_look_t *l)
{
    memset(l, 0, sizeof(*l));
    l->body = a->f[FH1_AV_BASE];
    l->hair_style = a->f[FH1_AV_HAIR_STYLE];
    l->top = a->f[FH1_AV_TOP];
    l->bottom = a->f[FH1_AV_BOTTOM];
    l->acc = a->acc;
    l->skin = fh1_avatar_choice_colour(FH1_AV_SKIN, a->f[FH1_AV_SKIN]);
    l->hair = fh1_avatar_choice_colour(FH1_AV_HAIR, a->f[FH1_AV_HAIR]);
    l->eyes = fh1_avatar_choice_colour(FH1_AV_EYES, a->f[FH1_AV_EYES]);
    l->top_colour = fh1_avatar_choice_colour(FH1_AV_TOP_COLOUR, a->f[FH1_AV_TOP_COLOUR]);
    l->bottom_colour = fh1_avatar_choice_colour(FH1_AV_BOTTOM_COLOUR, a->f[FH1_AV_BOTTOM_COLOUR]);
    l->acc_colour = fh1_avatar_choice_colour(FH1_AV_ACC_COLOUR, a->f[FH1_AV_ACC_COLOUR]);
}

// Duelo's own fighters (duel.c's): player 1 spiky with a red band, player 2 a ponytail and a white one.
static ftr_look_t base(int i, uint32_t colour)
{
    return (ftr_look_t){ .skin = i ? 0xe6b088 : 0xf6d2b0, .hair = i ? 0x5a1018 : 0x18203a, .top = colour, .bottom = 0x1a2a52,
                         .band = i ? 0xf0f0f4 : 0xd02828, .wraps = 0xf0f0f4, .boots = 0x3a2a26, .eyes = 0x3c3c48,
                         .hair_style = i ? FTR_HAIR_PONYTAIL : FTR_HAIR_SPIKY, .top_style = FTR_TOP_GI,
                         .bottom_style = FTR_BOTTOM_HAKAMA };
}

static void put(int cell, int x, int y, uint32_t rgb)
{
    const int W = COLS * CW, cx = (cell % COLS) * CW + x, cy = (cell / COLS) * CH + y;
    if (x >= 0 && y >= 0 && x < CW && y < CH && cell < ROWS * COLS) s_sheet[cy * W + cx] = rgb;
}

static void cell(int n, const fh1_avatar_t *a, const ftr_look_t *f)
{
    for (int y = 0; y < CH; y++)
        for (int x = 0; x < CW; x++) put(n, x, y, (x / 8 + y / 8) % 2 ? 0x1b1d2c : 0x22253a);
    if (a) {
        static uint32_t px[FH1_AVATAR_FULL_W * FH1_AVATAR_FULL_H];
        fh1_avatar_render_full(a, FH1_AV_NEUTRAL, 0, px);
        for (int y = 0; y < FH1_AVATAR_FULL_H; y++)
            for (int x = 0; x < FH1_AVATAR_FULL_W; x++)
                if (px[y * FH1_AVATAR_FULL_W + x] >> 24) put(n, 2 + x, 6 + y, px[y * FH1_AVATAR_FULL_W + x] & 0xffffff);
    }
    static uint32_t fp[FTR_W * FTR_H];
    const ftr_pose_t pose = { .blade = 0.35f, .face = FTR_FACE_OPEN, .steel = 0xc8d4e0 };
    int gx, gy;
    ftr_draw(fp, f, &pose, &gx, &gy);
    for (int y = 0; y < FTR_H; y++)
        for (int x = 0; x < FTR_W; x++)
            if (fp[y * FTR_W + x]) put(n, 104 + x, 40 + y, fp[y * FTR_W + x] & 0xffffff);
}

int main(int argc, char **argv)
{
    int n = 0;
    ftr_look_t f = base(0, 0x2a6cf0);
    cell(n++, NULL, &f);
    f = base(1, 0xff3c28);
    cell(n++, NULL, &f);
    // Every hair style, top, bottom and accessory, in turn, on both bodies.
    for (int i = 0; i < 13; i++) {
        fh1_avatar_t a = { { 0 }, 0 };
        a.f[FH1_AV_BASE] = i % 2;
        a.f[FH1_AV_HAIR_STYLE] = i % fh1_avatar_choices(FH1_AV_HAIR_STYLE);
        a.f[FH1_AV_HAIR] = (i * 3) % fh1_avatar_choices(FH1_AV_HAIR);
        a.f[FH1_AV_SKIN] = i % fh1_avatar_choices(FH1_AV_SKIN);
        a.f[FH1_AV_EYES] = i % fh1_avatar_choices(FH1_AV_EYES);
        a.f[FH1_AV_TOP] = i % fh1_avatar_choices(FH1_AV_TOP);
        a.f[FH1_AV_TOP_COLOUR] = (i * 5 + 2) % fh1_avatar_choices(FH1_AV_TOP_COLOUR);
        a.f[FH1_AV_BOTTOM] = i % fh1_avatar_choices(FH1_AV_BOTTOM);
        a.f[FH1_AV_BOTTOM_COLOUR] = (i * 7 + 1) % fh1_avatar_choices(FH1_AV_BOTTOM_COLOUR);
        a.f[FH1_AV_ACC_COLOUR] = i % fh1_avatar_choices(FH1_AV_ACC_COLOUR);
        a.acc = i < FH1_AV_ACCS ? 1u << i : 0;
        fh1_look_t l;
        look_of(&a, &l);
        f = base(i % 2, 0);
        ftr_dress(&f, &l);
        cell(n++, &a, &f);
    }
    const int W = COLS * CW, Hh = ((n + COLS - 1) / COLS) * CH;
    FILE *o = fopen(argc > 1 ? argv[1] : "sheet.ppm", "wb");
    if (!o) return 1;
    fprintf(o, "P6 %d %d 255\n", W * K, Hh * K);
    for (int y = 0; y < Hh * K; y++)
        for (int x = 0; x < W * K; x++) {
            const uint32_t c = s_sheet[(y / K) * W + x / K];
            fputc(c >> 16, o), fputc(c >> 8 & 255, o), fputc(c & 255, o);
        }
    fclose(o);
    ftr_free();
    return 0;
}
