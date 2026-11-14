// A view turned a quarter, for every game of the engine: instead of cutting to the new view, the frame shown turns
// away like a cube's face while the new one turns in beside it (each a little darker as it faces away), in `ms`.
// The game keeps drawing its new view each frame; viewturn_draw folds the old one in over it. A frame counts at most
// 40 ms toward the turn, so a slow one (the new view drawn the first time) does not cut it short.
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint16_t *from;                  // the frame shown when the turn began (allocated once, kept for the next turn)
    int w, h, dir;                   // dir +1: the old view leaves to the left, the new comes from the right; -1 back
    uint32_t t, ms;
    bool on;
} viewturn_t;

// The turn starts from the frame in px (the last one shown, still the old view). False if out of memory: no turn.
bool viewturn_begin(viewturn_t *v, const uint16_t *px, int w, int h, int dir, uint32_t ms);
static inline bool viewturn_active(const viewturn_t *v) { return v->on; }
void viewturn_update(viewturn_t *v, uint32_t dt);
// px holds the new view: turned in, the old view beside it turning away.
void viewturn_draw(const viewturn_t *v, uint16_t *px, int w, int h);
void viewturn_free(viewturn_t *v);
