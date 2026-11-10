// Gamepads from every source (today: controllers on the Bluetooth bridges) and which one each player uses.
// Sources add a gamepad when it connects and report the keys it holds; apps ask for a player's keys. The
// assignment is the user's (System > Inputs and outputs), saved:
//   ""     automatic: player 1 takes the first gamepad that no one else has chosen, player 2 the next
//   "fh1"  player 1 only: the FH1's own keys, no gamepad
//   id     that gamepad (its Bluetooth address), when it is connected
// Player 1 always also has the FH1's own keys; its gamepad's keys are posted as key events, so the whole
// interface works with it. Players 2 and up are read with fh1_input_player_keys (fh1_input.h).
//
// Everything else a controller has is here too: its full state (sticks, analog triggers, every button,
// motion) and its outputs (rumble, light, adaptive triggers), each as far as the controller has it (caps).
// The player LEDs are the core's: every controller shows the number of the player it plays for (none: off).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FH1_GAMEPADS_MAX 8
#define FH1_PLAYERS 2

// What a gamepad looks like, for its icon (shape) in the tray.
enum { FH1_PAD_FLAT, FH1_PAD_PLAYSTATION, FH1_PAD_PRO, FH1_PAD_JOYCON_L, FH1_PAD_JOYCON_R, FH1_PAD_REMOTE,
       FH1_PAD_KEYS /* a board's keys (a bridge's audio kit) */, FH1_PAD_KINDS };

// What a gamepad has besides buttons and sticks.
enum {
    FH1_PAD_CAP_RUMBLE = 1 << 0, FH1_PAD_CAP_LIGHT = 1 << 1, FH1_PAD_CAP_LEDS = 1 << 2, FH1_PAD_CAP_MOTION = 1 << 3,
    FH1_PAD_CAP_TRIGGERS = 1 << 4,           // adaptive triggers (DualSense)
};

typedef struct {
    char id[18];                // stable: "48:18:8D:C3:B1:78"
    char name[24];              // "DualSense"
    char via[24];               // where it is connected: the bridge's alias
    uint8_t battery;            // 0 unknown, 1 empty .. 255 full
    uint8_t kind;               // FH1_PAD_*
    uint32_t rgb;               // its colour: light bar or body (0xRRGGBB)
    uint8_t caps;               // FH1_PAD_CAP_*
    uint16_t vid, pid;          // USB-style ids, 0 unknown
    bool manual;                // never taken by an automatic player (a board's keys): chosen by name only
} fh1_gamepad_info_t;

// Its buttons by position (A is the bottom face button: cross on a PlayStation pad, B on a Nintendo one).
enum {
    FH1_PAD_BTN_A = 1 << 0, FH1_PAD_BTN_B = 1 << 1, FH1_PAD_BTN_X = 1 << 2, FH1_PAD_BTN_Y = 1 << 3,
    FH1_PAD_BTN_L1 = 1 << 4, FH1_PAD_BTN_R1 = 1 << 5, FH1_PAD_BTN_L2 = 1 << 6, FH1_PAD_BTN_R2 = 1 << 7,
    FH1_PAD_BTN_L3 = 1 << 8, FH1_PAD_BTN_R3 = 1 << 9,
    FH1_PAD_BTN_SYSTEM = 1 << 10, FH1_PAD_BTN_SELECT = 1 << 11, FH1_PAD_BTN_START = 1 << 12, FH1_PAD_BTN_CAPTURE = 1 << 13,
    FH1_PAD_BTN_UP = 1 << 14, FH1_PAD_BTN_DOWN = 1 << 15, FH1_PAD_BTN_LEFT = 1 << 16, FH1_PAD_BTN_RIGHT = 1 << 17,
};
typedef struct {
    uint32_t keys;              // as FH1 keys (1 << fh1_key_t): what the interface sees
    uint32_t buttons;           // FH1_PAD_BTN_*
    int16_t lx, ly, rx, ry;     // -512..511, y down positive
    uint16_t l2, r2;            // 0..1023
    int16_t gyro[3];            // rad/s x 1000
    int16_t accel[3];           // m/s^2 x 100
    uint32_t time_ms;           // when it arrived (esp_timer ms)
} fh1_pad_state_t;

// A source's outputs (any may be NULL); ctx is the source's own.
typedef struct {
    void (*rumble)(void *ctx, uint8_t weak, uint8_t strong, uint16_t ms);
    void (*light)(void *ctx, uint32_t rgb);
    void (*leds)(void *ctx, uint8_t mask);                 // player LEDs: bit n, player n+1
    void (*trigger)(void *ctx, int side, uint8_t effect, uint8_t a, uint8_t b, uint8_t c);   // FH1_TRIG_*
    void (*motion)(void *ctx, bool on);                    // stream motion fully (it costs link capacity)
} fh1_gamepad_ops_t;
// Adaptive trigger effects; side 0 L2, 1 R2. FEEDBACK: resists from position a (0-9), strength b (0-8). WEAPON: a
// click between positions a (2-7) and b (a+1-8), strength c. VIBRATION: from position a, amplitude b (0-8),
// frequency c (Hz).
enum { FH1_TRIG_OFF, FH1_TRIG_FEEDBACK, FH1_TRIG_WEAPON, FH1_TRIG_VIBRATION };

// Sources (any task). add returns a handle, or -1 when full; the same id again returns the same handle.
int fh1_gamepad_add(const fh1_gamepad_info_t *info);
void fh1_gamepad_remove(int handle);
void fh1_gamepad_set_battery(int handle, uint8_t battery);
void fh1_gamepad_set_colour(int handle, uint32_t rgb);
void fh1_gamepad_set_keys(int handle, uint32_t keys);      // the keys held now, 1 << fh1_key_t
void fh1_gamepad_set_state(int handle, const fh1_pad_state_t *st);   // everything (keys included)
void fh1_gamepad_set_ops(int handle, const fh1_gamepad_ops_t *ops, void *ctx);

// For the interface: the connected gamepads, and who plays with each.
int fh1_gamepad_count(void);
bool fh1_gamepad_at(int i, fh1_gamepad_info_t *out, int *player);   // player: 0-based, -1 for none

// Players (0-based). choice: "", "fh1" (player 0 only) or a gamepad id.
void fh1_gamepad_assign(int player, const char *choice);
void fh1_gamepad_assignment(int player, char *out, size_t n);
bool fh1_gamepad_of_player(int player, fh1_gamepad_info_t *out);    // the gamepad playing now, if any
int fh1_gamepad_player_of_id(const char *id);                        // the player a connected gamepad plays for, or -1
// The gamepad a player chose by name, as last seen (name, kind, colour; empty name if never seen), even while
// it is away; false when the player is automatic or the FH1's own keys.
bool fh1_gamepad_chosen(int player, fh1_gamepad_info_t *out);

// For apps: a gamepad's full state (by id: fh1_gamepad_of_player gives a player's), and its outputs (false
// when it is not connected or has not that). Motion is streamed fully while anyone wants it (counted).
bool fh1_gamepad_state(const char *id, fh1_pad_state_t *out);
bool fh1_gamepad_rumble(const char *id, uint8_t weak, uint8_t strong, uint16_t ms);
bool fh1_gamepad_light(const char *id, uint32_t rgb);
bool fh1_gamepad_trigger(const char *id, int side, uint8_t effect, uint8_t a, uint8_t b, uint8_t c);
void fh1_gamepad_want_motion(bool on);
// While muted, player 1's gamepad does not drive the interface (an input test reads it instead).
void fh1_gamepad_mute_keys(bool mute);
// Any connected gamepad that is not player 1's takes player 1 over when its Menu (PS, Home, Guide) is held for
// FH1_GAMEPAD_CLAIM_MS: for a controller whose battery ran out, or that went away. fn(id) is told, from the
// source's task (once per hold); it does the change, fh1_gamepad_assign(0, id).
#define FH1_GAMEPAD_CLAIM_MS 3000
void fh1_gamepad_on_claim(void (*fn)(const char *id));

// How a player's buttons are shown (hint chips, a game's key icons): by the controller it plays with. The FH1's
// own keys and plain controllers: the FH1's coloured caps; an Xbox controller: its letters in their colours; a
// Nintendo one (Pro, Joy-Con): its letters; a PlayStation one: its shapes. FH1 keys are by position (A the
// right face button), so each style names the button that sends them.
enum { FH1_KEYS_FH1, FH1_KEYS_XBOX, FH1_KEYS_NINTENDO, FH1_KEYS_PLAYSTATION, FH1_KEYS_STYLES };
int fh1_gamepad_style(int player);

#ifdef __cplusplus
}
#endif
