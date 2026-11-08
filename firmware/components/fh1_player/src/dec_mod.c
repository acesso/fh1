// Tracker modules (.mod: ProTracker and compatible, up to 32 channels) with pocketmod. The whole file is
// loaded (modules are small). A module has no set length: it plays once through, then ends. No seeking.
#include <string.h>
#include "player_priv.h"

#define POCKETMOD_IMPLEMENTATION
#include "pocketmod.h"

#define MOD_MAX (4 * 1024 * 1024)
#define MOD_RATE 44100
#define TMP_FRAMES 512

typedef struct {
    pocketmod_context ctx;
    uint8_t *data;
    float tmp[TMP_FRAMES * 2];
} modd_t;

static void mod_close(void *p)
{
    modd_t *d = p;
    if (!d) return;
    pl_free(d->data);
    pl_free(d);
}

static void *mod_open(src_t *s, track_info_t *ti, char *err, size_t n)
{
    if (s->size < 1084 || s->size > MOD_MAX) {
        snprintf(err, n, s->size > MOD_MAX ? "module too big (4 MB max)" : "not a module");
        return NULL;
    }
    modd_t *d = pl_calloc(1, sizeof(*d));
    if (d) d->data = pl_malloc((size_t)s->size);
    if (!d || !d->data || src_read(s, d->data, (size_t)s->size) != (size_t)s->size) {
        snprintf(err, n, d && d->data ? "cannot read the file" : "out of memory");
        mod_close(d);
        return NULL;
    }
    if (!pocketmod_init(&d->ctx, d->data, (int)s->size, MOD_RATE)) {
        snprintf(err, n, "not a module this player reads");
        mod_close(d);
        return NULL;
    }
    // The song's name: the first 20 bytes, ISO-8859-1 (plain ASCII in practice).
    char name[21];
    int k = 0;
    for (int i = 0; i < 20 && d->data[i]; i++) name[k++] = d->data[i] >= 0x20 && d->data[i] < 0x7F ? (char)d->data[i] : ' ';
    meta_copy(ti->title, sizeof(ti->title), name, k);
    ti->rate = MOD_RATE;
    ti->channels = 2;
    return d;
}

static int mod_read(void *p, int16_t *lr, int frames)
{
    modd_t *d = p;
    int out = 0;
    while (out < frames && pocketmod_loop_count(&d->ctx) == 0) {
        int want = frames - out < TMP_FRAMES ? frames - out : TMP_FRAMES;
        int bytes = pocketmod_render(&d->ctx, d->tmp, want * (int)sizeof(float[2]));
        int got = bytes / (int)sizeof(float[2]);
        for (int i = 0; i < got * 2; i++) {
            float v = d->tmp[i] * 32767.0f;
            lr[2 * out + i] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);
        }
        out += got;
        if (got < want) break;
    }
    return out;
}

const dec_ops_t dec_mod = { "MOD", mod_open, mod_read, NULL, mod_close };
