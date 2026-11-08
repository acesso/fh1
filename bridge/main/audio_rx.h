// Sound from the FH1 to the board's codec (audio_rx.c). Only on boards with an audio out (board_codec()).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fh1_bridge_proto.h"

void audio_rx_init(void);
void audio_rx_packet(const uint8_t *buf, size_t len);    // an FH1B_UDP_AUDIO payload (header and samples)
bool audio_rx_playing(void);
