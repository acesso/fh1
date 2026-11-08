// Duelo's fighters in the 16-bit fighting-game style: a black outline, four saturated tones and a shine, a
// three-quarter stance with the knees bent, wrapped fists and boots, about six heads tall, with a katana in the
// front hand and a hakama. Drawn from shapes (ellipses, polygons, capsules) in 96 x 128 design units, at half size.
//
// Plain C (math.h only), so it also builds on the PC to look at the sprites.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define FTR_W 80                     // the sprite, in pixels
#define FTR_H 92
#define FTR_CX 26                    // the body's middle (column), facing right
#define FTR_FEET 89                  // the row the soles stand on
#define FTR_EDGE 0x0c0812u           // the outline's colour
#define FTR_SET (1u << 24)           // a drawn pixel: FTR_SET | 0xRRGGBB; 0 is empty

typedef struct {
    uint32_t skin, hair, top, bottom, band, wraps, boots, eyes;
    int hair_style;                  // FTR_HAIR_*
} ftr_look_t;

enum { FTR_HAIR_SPIKY, FTR_HAIR_PONYTAIL };
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

// Draws the fighter facing right into px (FTR_W x FTR_H). *grip_x, *grip_y: where the hand holds the blade
// (pixels in the sprite), for the effects drawn over it.
void ftr_draw(uint32_t *px, const ftr_look_t *l, const ftr_pose_t *p, int *grip_x, int *grip_y);
// Frees the working memory (drawing again allocates it).
void ftr_free(void);
