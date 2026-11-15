#include "sfxbank.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fh1_sdk.h"

// FSFX: "FSFX", a version (2 bytes), the sounds' count (2 bytes), then 48 bytes a sound (its name in 32, its rate,
// frames and offset in the file, 4 bytes each), then 16-bit mono samples.
bool sfxbank_load(sfxbank_t *b, const char *pool, const char *file, const char *const *names, int n)
{
    memset(b, 0, sizeof(*b));
    b->snd = calloc((size_t)n, sizeof(sfxbank_snd_t));
    if (!b->snd) return false;
    b->n = n;
    const char *dir = fh1_asset_dir(pool);
    if (!dir) return false;
    char path[128];
    snprintf(path, sizeof(path), "%s/assets/%s", dir, file);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t head[8];
    bool ok = fread(head, 1, 8, f) == 8 && !memcmp(head, "FSFX", 4);
    const int count = head[6] | head[7] << 8;
    uint32_t offset[64] = { 0 };
    for (int i = 0; ok && i < count; i++) {
        uint8_t e[48];
        if (fread(e, 1, 48, f) != 48) break;
        e[31] = 0;
        for (int k = 0; k < n && k < 64; k++)
            if (names[k] && !strcmp((const char *)e, names[k])) {
                memcpy(&b->snd[k].rate, e + 32, 4);
                memcpy(&b->snd[k].frames, e + 36, 4);
                memcpy(&offset[k], e + 40, 4);
            }
    }
    for (int k = 0; ok && k < n && k < 64; k++) {
        sfxbank_snd_t *s = &b->snd[k];
        if (!s->frames) continue;
        // At least 4160 bytes: PSRAM even for a short sound (ESP-IDF serves smaller blocks from internal RAM).
        s->pcm = malloc(s->frames * 2 < 4160 ? 4160 : s->frames * 2);
        if (!s->pcm) continue;
        fseek(f, (long)offset[k], SEEK_SET);
        s->frames = (uint32_t)fread(s->pcm, 2, s->frames, f);
    }
    fclose(f);
    return ok;
}

void sfxbank_play(const sfxbank_t *b, int i, uint8_t volume)
{
    if (!b->snd || i < 0 || i >= b->n || !b->snd[i].pcm || !b->snd[i].frames) return;
    fh1_sfx_play(b->snd[i].pcm, b->snd[i].frames, b->snd[i].rate, volume);
}

void sfxbank_free(sfxbank_t *b)
{
    fh1_sfx_stop_all();
    for (int i = 0; b->snd && i < b->n; i++) free(b->snd[i].pcm);
    free(b->snd);
    memset(b, 0, sizeof(*b));
}
