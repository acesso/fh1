#include "snd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fh1_sdk.h"

#define ASSETS "org.fh1.assets.rpg-sfx"

// Each sound: its name in the bank and the set it belongs to.
static const struct { const char *name; uint8_t set; } s_def[S_COUNT] = {
    [S_HOVER] = { "hover_01", SND_SET_MENU },        [S_CONFIRM] = { "confirm_03", SND_SET_MENU },
    [S_DECLINE] = { "decline_09", SND_SET_MENU },    [S_DENIED] = { "denied_03", SND_SET_MENU },
    [S_PAUSE] = { "pause_04", SND_SET_MENU },        [S_UNPAUSE] = { "unpause_04", SND_SET_MENU },
    [S_EQUIP] = { "equip_10", SND_SET_MENU },        [S_UNEQUIP] = { "unequip_01", SND_SET_MENU },
    [S_BUY] = { "buy_sell_01", SND_SET_MENU },       [S_USE_ITEM] = { "use_item_01", SND_SET_MENU },
    [S_SLASH] = { "slash_04", SND_SET_BATTLE },      [S_HIT] = { "impact_flesh_02", SND_SET_BATTLE },
    [S_BITE] = { "bite_04", SND_SET_BATTLE },        [S_CLAW] = { "claw_03", SND_SET_BATTLE },
    [S_DEATH] = { "enemy_death_01", SND_SET_BATTLE }, [S_WHOOSH] = { "attack_03", SND_SET_BATTLE },
    [S_FIRE] = { "fire_explosion_04_medium", SND_SET_BATTLE }, [S_THUNDER] = { "thunder_02", SND_SET_BATTLE },
    [S_HEAL] = { "heal_02", SND_SET_BATTLE },        [S_REVIVE] = { "revive_03", SND_SET_BATTLE },
    [S_ENCOUNTER] = { "encounter_02", SND_SET_BATTLE }, [S_STEP_GRASS] = { "step_grass_03", SND_SET_BATTLE },
    [S_STEP_ROCK] = { "step_rock_02", SND_SET_BATTLE }, [S_STEP_WATER] = { "step_water_02", SND_SET_BATTLE },
    [S_JUMP] = { "jump_03", SND_SET_BATTLE },        [S_LANDING] = { "landing_01", SND_SET_BATTLE },
    [S_MISS] = { "miss_evade_02", SND_SET_BATTLE },  [S_WATER] = { "water_02", SND_SET_BATTLE },
    [S_EARTH] = { "earth_02", SND_SET_BATTLE },
};

static struct {
    uint32_t rate, frames, offset;   // from the bank's table (frames 0: not in the bank)
    int16_t *pcm;                    // loaded, or NULL
    uint32_t got;                    // frames read so far while loading
} s_snd[S_COUNT];
static char s_path[128];
static bool s_open;
static int s_loading;                // the sets being loaded

bool snd_open(void)
{
    const char *dir = fh1_asset_dir(ASSETS);
    if (!dir) return false;
    snprintf(s_path, sizeof(s_path), "%s/assets/rpg.fsfx", dir);
    FILE *f = fopen(s_path, "rb");
    if (!f) return false;
    uint8_t head[8];
    bool ok = fread(head, 1, 8, f) == 8 && !memcmp(head, "FSFX", 4);
    const int n = head[6] | head[7] << 8;
    for (int i = 0; ok && i < n; i++) {
        uint8_t e[48];
        if (fread(e, 1, 48, f) != 48) break;
        e[31] = 0;
        for (int k = 0; k < S_COUNT; k++)
            if (!strcmp((const char *)e, s_def[k].name)) {
                memcpy(&s_snd[k].rate, e + 32, 4);
                memcpy(&s_snd[k].frames, e + 36, 4);
                memcpy(&s_snd[k].offset, e + 40, 4);
            }
    }
    fclose(f);
    s_open = ok;
    return ok;
}

void snd_close(void)
{
    fh1_sfx_stop_all();
    for (int k = 0; k < S_COUNT; k++) {
        free(s_snd[k].pcm);
        s_snd[k].pcm = NULL;
        s_snd[k].got = 0;
    }
    s_open = false;
}

void snd_load_begin(int sets) { s_loading = sets; }

float snd_load_step(uint32_t max_bytes)
{
    if (!s_open) return 1.0f;
    uint32_t want = 0, have = 0;                         // frames (32 bits: no 64-bit float helpers)
    FILE *f = NULL;
    for (int k = 0; k < S_COUNT; k++) {
        if (!(s_def[k].set & s_loading) || !s_snd[k].frames) continue;
        want += s_snd[k].frames;
        if (!s_snd[k].pcm) s_snd[k].pcm = malloc(s_snd[k].frames * 2);      // large: PSRAM
        if (!s_snd[k].pcm) continue;
        if (s_snd[k].got < s_snd[k].frames && max_bytes >= 2) {
            if (!f) f = fopen(s_path, "rb");
            if (f) {
                uint32_t n = s_snd[k].frames - s_snd[k].got;
                if (n > max_bytes / 2) n = max_bytes / 2;
                fseek(f, s_snd[k].offset + s_snd[k].got * 2, SEEK_SET);
                const size_t r = fread(s_snd[k].pcm + s_snd[k].got, 2, n, f);
                s_snd[k].got += r;
                max_bytes -= r * 2;
                if (r < n) s_snd[k].got = s_snd[k].frames;              // a short file: what came is what plays
            }
        }
        have += s_snd[k].got;
    }
    if (f) fclose(f);
    return want ? (float)have / want : 1.0f;
}

void snd_unload(int set)
{
    fh1_sfx_stop_all();
    for (int k = 0; k < S_COUNT; k++)
        if (s_def[k].set == set) {
            free(s_snd[k].pcm);
            s_snd[k].pcm = NULL;
            s_snd[k].got = 0;
        }
}

void snd_play(snd_t s, uint8_t volume)
{
    if (s >= S_COUNT || !s_snd[s].pcm || s_snd[s].got < s_snd[s].frames) return;
    fh1_sfx_play(s_snd[s].pcm, s_snd[s].frames, s_snd[s].rate, volume);
}
