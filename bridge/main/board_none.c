// A bridge board with nothing of its own besides the radio (the ESP32-WROOM-32 module). See board.h.
#include "board.h"

void board_init(void) {}
uint32_t board_keys(void) { return 0; }
int board_key_count(void) { return 0; }
const char *board_key_name(int i) { return "?"; }
bool board_headphones(void) { return false; }
void board_led(bool on) {}
uint8_t board_codec(void) { return 0; }
bool board_audio_start(uint32_t rate) { return false; }
void board_audio_stop(void) {}
int board_audio_write(const int16_t *lr, size_t frames) { return -1; }
void board_audio_volume(uint8_t pct) {}
bool board_has_sd(void) { return false; }
uint32_t board_sd_mb(void) { return 0; }
int board_ctrl_count(uint8_t dev) { return 0; }
bool board_ctrl_get(uint8_t dev, int i, fh1b_ctrl_t *out) { return false; }
bool board_ctrl_set(uint8_t dev, uint8_t id, int16_t value) { return false; }
