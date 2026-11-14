#include "flag.h"
#include <stdio.h>

bool flag_load(flag_t *f, const fart_t *a, const char *name)
{
    char n[32];
    f->n = 0;
    for (int i = 0; i < FLAG_FRAMES_MAX; i++) {
        snprintf(n, sizeof(n), "%s.%d", name, i);
        const int k = fart_image(a, n);
        if (k < 0) break;
        f->frame[f->n++] = (uint16_t)k;
    }
    snprintf(n, sizeof(n), "%s.key", name);
    const int k = fart_image(a, n);
    uint8_t mask[4];
    if (f->n == 0 || k < 0 || !fart_unpack(&a->img[k], f->key, mask, 2)) {
        f->n = 0;
        return false;
    }
    return true;
}

int flag_frame(const flag_t *f, uint32_t t_ms)
{
    return f->n ? f->frame[t_ms / FLAG_MS % (uint32_t)f->n] : FART_NONE;
}

void flag_draw(const flag_t *f, const fart_t *a, uint16_t *dst, int stride, const fart_clip_t *clip, int x, int y,
               uint32_t t_ms, const uint16_t colours[2])
{
    const int i = flag_frame(f, t_ms);
    if (i == FART_NONE) return;
    fart_map(f->key, colours, 2);
    fart_draw(dst, stride, clip, &a->img[i], x, y, false);
    fart_map(NULL, NULL, 0);
}
