// Where a frame's time goes (the dev command "fps"): CPU cycles summed per phase, read from the cycle counter
// (a register read: nothing to the frame's time). fh1_doom.c logs them every 5 s.
#pragma once
#include <stdint.h>

enum { PROF_TICS, PROF_WALLS, PROF_PLANES, PROF_SPRITES, PROF_HUD, PROF_FINISH, PROF_VIEW, PROF_DISPLAY, PROF_STEP, PROF_COLS, PROF_SPANS, PROF_SEGS, PROF_ADDSPR, PROF_COMPOSITE, PROF_PATCH, PROF_READ, PROF_SPRDRAW, PROF_MASKSEG, PROF_PSPR, PROF_N };
extern uint32_t doom_prof[PROF_N];

static inline uint32_t prof_now(void)
{
    uint32_t c;
    __asm__ volatile("rsr %0, ccount" : "=a"(c));
    return c;
}

#define PROF(slot, stmt) do { const uint32_t prof_t0_ = prof_now(); stmt; doom_prof[slot] += prof_now() - prof_t0_; } while (0)
