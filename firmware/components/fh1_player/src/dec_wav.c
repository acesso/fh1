// WAV (PCM 8-32 bit, float, A-law, mu-law, IMA and MS ADPCM) with dr_wav; RIFF, RIFF64 and W64.
#include <string.h>
#include "player_priv.h"

#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#define DR_WAV_NO_WCHAR
#include "dr_wav.h"

typedef struct {
    drwav w;
    int16_t *tmp;
} wavd_t;

#define TMP_FRAMES 1024

static size_t on_read(void *u, void *buf, size_t n) { return src_read(u, buf, n); }
static drwav_bool32 on_seek(void *u, int off, drwav_seek_origin o)
{
    return src_seek(u, off, o == DRWAV_SEEK_SET ? SEEK_SET : o == DRWAV_SEEK_END ? SEEK_END : SEEK_CUR);
}
static drwav_bool32 on_tell(void *u, drwav_int64 *at)
{
    *at = src_tell(u);
    return *at >= 0;
}
static void *a_malloc(size_t n, void *u) { return pl_malloc(n); }
static void *a_realloc(void *p, size_t n, void *u) { return pl_realloc(p, n); }
static void a_free(void *p, void *u) { pl_free(p); }
static const drwav_allocation_callbacks s_alloc = { NULL, a_malloc, a_realloc, a_free };

static void wav_close(void *p)
{
    wavd_t *d = p;
    if (!d) return;
    if (d->tmp) drwav_uninit(&d->w);
    pl_free(d->tmp);
    pl_free(d);
}

static void *wav_open(src_t *s, track_info_t *ti, char *err, size_t n)
{
    wavd_t *d = pl_calloc(1, sizeof(*d));
    if (!d) {
        snprintf(err, n, "out of memory");
        return NULL;
    }
    if (!drwav_init(&d->w, on_read, on_seek, on_tell, s, &s_alloc)) {
        snprintf(err, n, "not a WAV file this player reads");
        pl_free(d);
        return NULL;
    }
    d->tmp = pl_malloc(TMP_FRAMES * d->w.channels * sizeof(int16_t));
    if (!d->tmp) {
        drwav_uninit(&d->w);
        pl_free(d);
        snprintf(err, n, "out of memory");
        return NULL;
    }
    ti->rate = d->w.sampleRate;
    ti->channels = (uint8_t)d->w.channels;
    ti->frames = d->w.totalPCMFrameCount;
    ti->kbps = (uint16_t)(d->w.sampleRate * d->w.channels * d->w.bitsPerSample / 1000);
    return d;
}

static int wav_read(void *p, int16_t *lr, int frames)
{
    wavd_t *d = p;
    const int ch = d->w.channels;
    int out = 0;
    while (out < frames) {
        int want = frames - out < TMP_FRAMES ? frames - out : TMP_FRAMES;
        int got = (int)drwav_read_pcm_frames_s16(&d->w, want, d->tmp);
        for (int i = 0; i < got; i++) {
            lr[2 * (out + i)] = d->tmp[i * ch];
            lr[2 * (out + i) + 1] = d->tmp[i * ch + (ch > 1)];
        }
        out += got;
        if (got < want) break;
    }
    return out;
}

static bool wav_seek(void *p, uint64_t frame) { return drwav_seek_to_pcm_frame(&((wavd_t *)p)->w, frame); }

const dec_ops_t dec_wav = { "WAV", wav_open, wav_read, wav_seek, wav_close };
