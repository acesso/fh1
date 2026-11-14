// Waving flags for every game of the engine: frames made on the PC (tools/fh1model.py's flag_set, a recipe's
// "buildings"/"flags" in fh1art.py) in two reference colours, drawn here in any two: a team's, a realm's, a tower's.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "fart.h"

#define FLAG_FRAMES_MAX 16
#define FLAG_MS 110                  // a frame

typedef struct {
    uint16_t frame[FLAG_FRAMES_MAX]; // the pack's images, name.0 ..
    int n;
    uint16_t key[2];                 // the cloth's reference colours (light, dark): name.key
} flag_t;

// The flag `name` from a pack: false if it has none.
bool flag_load(flag_t *f, const fart_t *a, const char *name);
// Its image at a time (looping); FART_NONE if not loaded.
int flag_frame(const flag_t *f, uint32_t t_ms);
// Draws it with its pole's foot at (x, y), its cloth in colours[0] (light) and colours[1] (dark).
void flag_draw(const flag_t *f, const fart_t *a, uint16_t *dst, int stride, const fart_clip_t *clip, int x, int y,
               uint32_t t_ms, const uint16_t colours[2]);
