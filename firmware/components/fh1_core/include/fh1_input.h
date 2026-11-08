// Input events. Every source (buttons, stick, dev console, serial keys, gamepad) ends up here.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    FH1_KEY_UP, FH1_KEY_DOWN, FH1_KEY_LEFT, FH1_KEY_RIGHT,
    FH1_KEY_A, FH1_KEY_B, FH1_KEY_X, FH1_KEY_Y,
    FH1_KEY_START, FH1_KEY_SELECT, FH1_KEY_MENU,
    FH1_KEY_COUNT,
} fh1_key_t;

typedef struct {
    uint8_t key;       // fh1_key_t
    bool pressed;
} fh1_input_event_t;

void fh1_input_init(void);
void fh1_input_post(fh1_key_t key, bool pressed);
bool fh1_input_get(fh1_input_event_t *ev);     // non-blocking
void fh1_input_flush(void);                    // drop queued events (held keys stay held)
bool fh1_input_is_down(fh1_key_t key);
// The keys held by player 2, 3, ... (player >= 1, 0-based; player 1's are the ones above), as a mask of
// 1 << fh1_key_t: their gamepad's, chosen in System > Inputs and outputs (fh1_gamepad.h). 0 without one.
uint32_t fh1_input_player_keys(int player);
const char *fh1_key_name(fh1_key_t key);
int fh1_key_from_name(const char *name);       // -1 if unknown
