// The game's sounds, from the shared asset package rpg-sfx (a .fsfx bank on the SD card): loaded
// a set at a time (the menus' once, a battle's before it starts, a little each frame for a loading screen), played
// through the firmware's mixer.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    S_HOVER, S_CONFIRM, S_DECLINE, S_DENIED, S_PAUSE, S_UNPAUSE, S_EQUIP, S_UNEQUIP, S_BUY, S_USE_ITEM,   // menus
    S_SLASH, S_HIT, S_BITE, S_CLAW, S_DEATH, S_WHOOSH, S_FIRE, S_THUNDER, S_HEAL, S_REVIVE, S_ENCOUNTER,  // battle
    S_STEP_GRASS, S_STEP_ROCK, S_STEP_WATER, S_JUMP, S_LANDING, S_MISS, S_WATER, S_EARTH,
    S_COUNT
} snd_t;

enum { SND_SET_MENU = 1, SND_SET_BATTLE = 2 };

bool snd_open(void);                 // the bank's table; false without the asset package (the game plays silent)
void snd_close(void);
// Loading a set: begin, then step until it returns 1.0 (each step reads up to max_bytes); progress 0..1.
void snd_load_begin(int sets);
float snd_load_step(uint32_t max_bytes);
void snd_unload(int set);
void snd_play(snd_t s, uint8_t volume);
