// The bridge board's own hardware besides the radio: board_a1s.c on the ESP32-A1S Audio Kit, board_none.c
// (nothing) on the WROOM.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "fh1_bridge_proto.h"

// The board's devices, by their ids in the FH1's list (fh1b_board_dev_t.id).
enum { BOARD_DEV_OUT, BOARD_DEV_IN, BOARD_DEV_KEYS, BOARD_DEV_LIGHT, BOARD_DEV_SD };

void board_init(void);
uint32_t board_keys(void);             // bit i: key i held
int board_key_count(void);
const char *board_key_name(int i);
bool board_headphones(void);           // plugged
void board_led(bool on);
uint8_t board_codec(void);             // its I2C address (0x10 ES8388, 0x1A AC101), 0 none

// Sound out (a board with a codec): started at a rate (Hz), fed 16-bit stereo frames (L, R; blocks until
// queued), stopped (outputs powered down, so an idle codec stays quiet). The speaker amplifier is on only
// while playing with no headphones in.
bool board_audio_start(uint32_t rate);
void board_audio_stop(void);
int board_audio_write(const int16_t *lr, size_t frames);
void board_audio_volume(uint8_t pct);

// An SD card slot: whether there is one, and the card in it (0 MB: none or unreadable). Checked by a task of
// its own (card detect, then the card read), so these return at once.
bool board_has_sd(void);
uint32_t board_sd_mb(void);

// A device's controls (protocol 7: fh1b_ctrl_t), drawn by the FH1 in that device's view: how many, one
// described as it is now, one set (applied at once and saved).
int board_ctrl_count(uint8_t dev);
bool board_ctrl_get(uint8_t dev, int i, fh1b_ctrl_t *out);
bool board_ctrl_set(uint8_t dev, uint8_t id, int16_t value);
