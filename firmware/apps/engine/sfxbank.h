// Sound effects for every game of the engine: the sounds a game names, taken from an asset package's FSFX bank
// (tools/fh1sfx.py; the rpg-sfx pool's "rpg.fsfx"), loaded into PSRAM once and played by index on the SDK's mixer.
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int16_t *pcm;                    // NULL: not in the bank (playing it does nothing)
    uint32_t frames, rate;
} sfxbank_snd_t;

typedef struct {
    sfxbank_snd_t *snd;              // one per name asked for, in their order
    int n;
} sfxbank_t;

// Loads the sounds named (n of them; a NULL name is left out) from pool `pool`'s file `file` (in its assets folder).
// false without the package or the file: every play is then silent, so a game needs no other check.
bool sfxbank_load(sfxbank_t *b, const char *pool, const char *file, const char *const *names, int n);
void sfxbank_play(const sfxbank_t *b, int i, uint8_t volume);
void sfxbank_free(sfxbank_t *b);
