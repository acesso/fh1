// Avatars (firmware/docs/users.md, phase U2): style A, anime-inspired shaded pixel art drawn by code from a
// short code of choices, at three sizes, each drawn for its size: the full figure (96 x 128), the face (32 x 32)
// and the mini face (16 x 16, inline in text). Every view is animated: the eyes blink, the head moves with the
// expression, and the expression's effect plays (hearts, tears, "zzz"...). The same drawing as
// firmware/docs/avatar-showcase.html (its renderer is the reference; this is a port).
//
// Pixels are ARGB8888 in uint32_t (0xAARRGGBB, 0 transparent), row by row. Pure computation: any task.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FH1_AVATAR_FULL_W 96
#define FH1_AVATAR_FULL_H 128
#define FH1_AVATAR_CODE_LEN 12            // the code's bytes (fh1_users keeps up to 24)

// The choices, as fields (each a small number). Accessories are a set of flags.
enum {
    FH1_AV_BASE,            // 0 male, 1 female
    FH1_AV_HAIR_STYLE,
    FH1_AV_HAIR,            // colour
    FH1_AV_SKIN,
    FH1_AV_EYES,            // colour
    FH1_AV_TOP,
    FH1_AV_TOP_COLOUR,
    FH1_AV_BOTTOM,
    FH1_AV_BOTTOM_COLOUR,
    FH1_AV_ACC_COLOUR,
    FH1_AV_FIELDS,
};
enum { FH1_AV_ACC_ROUND_GLASSES, FH1_AV_ACC_SQUARE_GLASSES, FH1_AV_ACC_EARRINGS, FH1_AV_ACC_SCARF,
       FH1_AV_ACC_NECKLACE, FH1_AV_ACC_FACE_MARKS, FH1_AV_ACCS };

typedef struct {
    uint8_t f[FH1_AV_FIELDS];
    uint8_t acc;                          // 1 << FH1_AV_ACC_*
} fh1_avatar_t;

// Expressions, each with its eyes, brows, mouth and effect.
enum { FH1_AV_NEUTRAL, FH1_AV_HAPPY, FH1_AV_LAUGH, FH1_AV_SAD, FH1_AV_CRY, FH1_AV_ANGRY, FH1_AV_SURPRISED,
       FH1_AV_WINK, FH1_AV_LOVE, FH1_AV_SLEEPY, FH1_AV_COOL, FH1_AV_THINK, FH1_AV_EXPRS };

// The code: [version 1][the fields][acc]. Unknown values fall back to the first choice.
size_t fh1_avatar_encode(const fh1_avatar_t *a, uint8_t *out, size_t max);
bool fh1_avatar_decode(const uint8_t *in, size_t n, fh1_avatar_t *a);
// A pleasant avatar picked from some bytes (a user's id), for users who have not made theirs.
void fh1_avatar_from_seed(const void *seed, size_t n, fh1_avatar_t *a);

// The editor's lists. Names are English (translate with T()); a colour field's choices have colours instead.
int fh1_avatar_choices(int field);
const char *fh1_avatar_field_name(int field);
const char *fh1_avatar_choice_name(int field, int i);     // NULL for colour fields
uint32_t fh1_avatar_choice_colour(int field, int i);     // 0xRRGGBB, for colour fields
const char *fh1_avatar_acc_name(int acc);
const char *fh1_avatar_expr_name(int expr);

// Drawing at time t (ms; animations loop). full: FH1_AVATAR_FULL_W x _H pixels; face: size x size, 32 or 16.
void fh1_avatar_render_full(const fh1_avatar_t *a, int expr, uint32_t t, uint32_t *px);
void fh1_avatar_render_face(const fh1_avatar_t *a, int expr, uint32_t t, int size, uint32_t *px);

#ifdef __cplusplus
}
#endif
