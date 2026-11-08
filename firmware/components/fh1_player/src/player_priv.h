// Inside the player: the file source and the decoders' common interface.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// A file being played. Reads go through a 4 KB internal buffer: the SD card reads by DMA, and the decoders'
// buffers are in PSRAM.
typedef struct {
    FILE *f;
    uint8_t *bounce;
    int64_t size;
} src_t;
size_t src_read(src_t *s, void *dst, size_t n);
bool src_seek(src_t *s, int64_t off, int whence);       // SEEK_SET, SEEK_CUR, SEEK_END
int64_t src_tell(src_t *s);

// What a decoder found in the file. Strings are UTF-8; empty when the file does not say.
typedef struct {
    char title[96], artist[64], album[64];
    uint32_t rate;
    uint8_t channels;
    uint16_t kbps;
    uint64_t frames;            // total, 0 when unknown
} track_info_t;

// Copies a tag value (not NUL-terminated, len bytes) into dst, trimmed.
void meta_copy(char *dst, size_t n, const char *src, size_t len);
// "TITLE=...", "ARTIST=...", "ALBUM=..." (Vorbis comments: Ogg and FLAC) into the info.
void meta_vorbis_comment(track_info_t *ti, const char *c, size_t len);

typedef struct {
    const char *name;           // "MP3"
    void *(*open)(src_t *s, track_info_t *ti, char *err, size_t n);
    int (*read)(void *d, int16_t *lr, int frames);   // interleaved stereo; 0 at the end, < 0 on error
    bool (*seek)(void *d, uint64_t frame);
    void (*close)(void *d);
} dec_ops_t;

extern const dec_ops_t dec_mp3, dec_flac, dec_ogg, dec_wav, dec_mod;

// PSRAM first (the decoders' state and tables); shim/pl_alloc.h points malloc and friends here.
void *pl_malloc(size_t n);
void *pl_calloc(size_t n, size_t size);
void *pl_realloc(void *p, size_t n);
void pl_free(void *p);
