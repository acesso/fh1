// PCM5102A I2S DAC as an audio.out device (firmware/PLAN.md, "Dev-board audio"). The chip cannot be probed
// (no I2C, nothing to read back): the board registers it switched off, and the user enables it in Devices
// once it is wired.
#pragma once
#include "fh1_devices.h"

typedef struct {
    int bck, ws, dout;       // GPIOs: bit clock, word select (LCK), data (DIN); no MCLK
} fh1_pcm5102a_cfg_t;

extern const fh1_driver_t fh1_pcm5102a_driver;
