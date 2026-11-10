// Effects over the battlefield, in world pixels: projectiles (arrows, thrown items, anything that flies from a
// point to a point in an arc), particles (bursts, rising sparkles, glows) and lightning from the sky.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "fart.h"

void fx_reset(void);
void fx_update(uint32_t dt_ms);
void fx_draw(const fart_t *art, uint16_t *px, int w, int h, int camx, int camy);
bool fx_busy(void);

// A projectile from (x0, y0) to (x1, y1) in ms, rising arc pixels at its middle. img: an image; rot: img is the
// first of 16 rotations and the projectile turns to follow its path (an arrow); spin: it tumbles (a thrown item).
// Returns its id; fx_landed(id) is true once it arrives.
int fx_shoot(int img, bool rot, bool spin, float x0, float y0, float x1, float y1, float arc, uint32_t ms);
bool fx_landed(int id);
// Sparks left behind by a projectile (an elemental arrow; with no image, a ball of light: a blast).
void fx_trail(int id, uint16_t c1, uint16_t c2);
// How a projectile looks besides its image: an ice needle (a bright streak along its path, no image), a tempest
// arrow (its image with fading after-images and two spirals of wind wound round its path).
enum { FX_PLAIN, FX_NEEDLE, FX_TEMPEST };
void fx_style(int id, int style);
// A rock spike bursting out of the ground at (x, y) (its foot), and sinking back: earth.
void fx_spike(float x, float y);
// A small whirlwind rising from (x, y), crackling: the tempest arrow's landing.
void fx_vortex(float x, float y);

void fx_burst(float x, float y, uint16_t c1, uint16_t c2, int n, float speed);   // an explosion of sparks
void fx_sparkle(float x, float y, uint16_t c, int n);                            // sparkles rising (healing)
void fx_glow(float x, float y, uint16_t c);                                      // a few sparks around a point
void fx_bolt(float x, float y);                                                  // lightning striking (x, y)
// Decoration the battle does not wait for: a spark or two round (x, y) within r, each call (an Elemental's aura,
// an elemental weapon's glow). Kinds: rising (fire), dripping (water), dust (earth), swirling (air).
enum { FX_RISE, FX_DRIP, FX_DUST, FX_SWIRL };
void fx_aura(float x, float y, float r, uint16_t c1, uint16_t c2, int kind);
