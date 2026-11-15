// Duelo's fighters in the 16-bit fighting-game style: a black outline, four saturated tones and a shine, a
// three-quarter stance with the knees bent, wrapped fists and boots, about six heads tall, with a katana in the
// front hand; dressed as the player's avatar is (or in a gi and hakama). Drawn from shapes (ellipses, polygons,
// capsules) in 96 x 128 design units, at half size.
//
// Plain C (math.h only), so it also builds on the PC to look at the sprites (host/preview.c).
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "fh1_sdk.h"                 // (fh1_look_t only)

#define FTR_W 80                     // the sprite, in pixels
#define FTR_H 92
#define FTR_CX 26                    // the body's middle (column), facing right
#define FTR_FEET 89                  // the row the soles stand on
#define FTR_EDGE 0x0c0812u           // the outline's colour
#define FTR_SET (1u << 24)           // a drawn pixel: FTR_SET | 0xRRGGBB; 0 is empty

typedef struct {
    uint32_t skin, hair, top, bottom, band, wraps, boots, eyes;
    uint32_t acc_colour;             // glasses, earrings, a scarf, a necklace
    int hair_style;                  // FTR_HAIR_*
    int top_style, bottom_style;     // FTR_TOP_*, FTR_BOTTOM_*
    int body;                        // FTR_BODY_*
    unsigned acc;                    // FTR_ACC_*
} ftr_look_t;

// As the FH1's avatars have them (fh1_sdk.h, fh1_look_t): a player's avatar dresses their fighter.
enum { FTR_HAIR_SPIKY, FTR_HAIR_SHORT, FTR_HAIR_BOB, FTR_HAIR_LONG, FTR_HAIR_TWIN_TAILS, FTR_HAIR_PONYTAIL };
enum { FTR_TOP_TSHIRT, FTR_TOP_HOODIE, FTR_TOP_JACKET, FTR_TOP_GI, FTR_TOP_SAILOR };
enum { FTR_BOTTOM_TROUSERS, FTR_BOTTOM_SHORTS, FTR_BOTTOM_SKIRT, FTR_BOTTOM_HAKAMA };
enum { FTR_BODY_MALE, FTR_BODY_FEMALE };
enum { FTR_ACC_ROUND_GLASSES = 1 << 0, FTR_ACC_SQUARE_GLASSES = 1 << 1, FTR_ACC_EARRINGS = 1 << 2,
       FTR_ACC_SCARF = 1 << 3, FTR_ACC_NECKLACE = 1 << 4, FTR_ACC_FACE_MARKS = 1 << 5 };
enum { FTR_FACE_OPEN, FTR_FACE_CLOSED, FTR_FACE_SHOUT };

typedef struct {
    float breath;                    // -1..1
    float step;                      // -1..1, the walk's phase
    float blade;                     // the blade's angle in radians (up positive); no_blade: none (knocked out)
    bool no_blade;
    int face;                        // FTR_FACE_*
    float wave;                      // the headband's ribbons, -1..1
    uint32_t steel;                  // the blade's colour
} ftr_pose_t;

// Dresses a fighter as a player's avatar (fh1_user_look): body, hair, clothes, accessories and their colours; the
// headband, wraps and boots stay the game's.
void ftr_dress(ftr_look_t *f, const fh1_look_t *a);
// Draws the fighter facing right into px (FTR_W x FTR_H). *grip_x, *grip_y: where the hand holds the blade
// (pixels in the sprite), for the effects drawn over it.
void ftr_draw(uint32_t *px, const ftr_look_t *l, const ftr_pose_t *p, int *grip_x, int *grip_y);
// Frees the working memory (drawing again allocates it).
void ftr_free(void);
