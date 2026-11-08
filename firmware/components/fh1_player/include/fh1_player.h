// The audio player engine: plays one file at a time from the SD card or internal storage, on its own task
// (core 0), through the audio.out class. Formats: MP3, FLAC, Ogg Vorbis, WAV and tracker modules (.mod).
// It keeps the last second of what it played for visualisers (fh1_player_tap). No UI here: the Music app
// (fh1_apps) owns the playlist and the screens. See firmware/PLAN.md, "Music".
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// True if the name ends in an extension the player decodes (.mp3 .flac .ogg .oga .wav .mod).
bool fh1_player_supported(const char *name);

typedef enum { FH1_PLAYER_STOPPED, FH1_PLAYER_LOADING, FH1_PLAYER_PLAYING, FH1_PLAYER_PAUSED } fh1_player_state_t;

typedef struct {
    fh1_player_state_t state;
    uint32_t serial;            // changes with every track started (or failed)
    char path[160];
    char title[96], artist[64], album[64];   // UTF-8; title falls back to the file name
    const char *format;         // "MP3", "FLAC", "OGG", "WAV", "MOD", or "" when stopped
    uint32_t rate;              // Hz
    uint8_t channels;           // in the file (the output is always stereo)
    uint16_t kbps;              // average
    uint32_t pos_ms, dur_ms;    // dur_ms 0: unknown (modules)
    char error[64];             // why the last track did not play
} fh1_player_status_t;

// Start playing a file (stops what plays). The start happens on the player's task: the status shows
// LOADING, then PLAYING, or STOPPED with an error. From any task.
esp_err_t fh1_player_play(const char *path);
void fh1_player_pause(bool paused);
void fh1_player_toggle(void);
void fh1_player_stop(void);
void fh1_player_seek(int32_t delta_ms);       // relative, clamped to the track
void fh1_player_status(fh1_player_status_t *st);

// Called on the player's task when a track ends by itself (not on stop or error), e.g. to queue the next.
// Keep it short: hand the work to another task.
void fh1_player_on_end(void (*cb)(void *user), void *user);

// The most recent n stereo frames (L, R interleaved) about to be heard, oldest first, for visualisers.
// Returns the frames copied (fewer near the start of a track; 0 when nothing plays). *rate gets their rate.
size_t fh1_player_tap(int16_t *lr, size_t n, uint32_t *rate);

#ifdef __cplusplus
}
#endif
