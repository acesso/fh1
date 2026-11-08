// Audio output class. Apps open the class, never a device: when no audio device is ready
// (absent, failed or switched off), every call still works and returns FH1_UNAVAILABLE.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "fh1_devices.h"

#define FH1_UNAVAILABLE (-2)

// What an audio.out driver implements (fh1_driver_t.ops).
typedef struct {
    int (*write)(fh1_device_t *d, const int16_t *frames, size_t count, uint32_t rate);  // mono frames
    void (*set_volume)(fh1_device_t *d, uint8_t pct);
    int (*write_stereo)(fh1_device_t *d, const int16_t *lr, size_t count, uint32_t rate);   // L, R pairs
    void (*set_channels)(fh1_device_t *d, int mode);           // fh1_audio_channels_t
} fh1_audio_ops_t;

typedef struct fh1_audio fh1_audio_t;

fh1_audio_t *fh1_audio_open(uint32_t sample_rate);    // never NULL
int fh1_audio_write(fh1_audio_t *a, const int16_t *frames, size_t count);   // frames written or FH1_UNAVAILABLE
int fh1_audio_write_stereo(fh1_audio_t *a, const int16_t *lr, size_t count);  // interleaved L, R frames
void fh1_audio_set_volume(fh1_audio_t *a, uint8_t pct);
bool fh1_audio_available(void);
// The system volume, 0-100 %, saved: applied to the ready device now and whenever one starts.
// fh1_audio_preview_volume applies without saving (a slider being moved).
void fh1_audio_set_master_volume(uint8_t pct);
void fh1_audio_preview_volume(uint8_t pct);
uint8_t fh1_audio_master_volume(void);
// Which channels play, saved: stereo as the app sends it, both channels mixed to mono, or the mix on the
// left or right channel only (the other silent).
typedef enum { FH1_AUDIO_STEREO, FH1_AUDIO_MONO, FH1_AUDIO_LEFT, FH1_AUDIO_RIGHT, FH1_AUDIO_CHANNEL_MODES } fh1_audio_channels_t;
void fh1_audio_set_channels(fh1_audio_channels_t mode);
fh1_audio_channels_t fh1_audio_channels(void);
void fh1_audio_close(fh1_audio_t *a);
// Which audio.out device plays (System > Inputs and outputs), saved: a device id, or "" for the first ready
// one. When the chosen one is not ready, the first ready one plays.
void fh1_audio_set_output(const char *dev_id);
const char *fh1_audio_output(void);
