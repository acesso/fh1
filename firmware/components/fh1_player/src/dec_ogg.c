// Ogg Vorbis with Tremor (integer-only Vorbis): tags from the Vorbis comments, sample-exact seeking.
// A chained file (several streams one after another) plays through; the format of the first one is shown.
#include <string.h>
#include "player_priv.h"
#include "ivorbisfile.h"

typedef struct {
    OggVorbis_File vf;
    int channels;
    int16_t *tmp;
} oggd_t;

#define TMP_FRAMES 1024

static size_t on_read(void *ptr, size_t size, size_t nmemb, void *u) { return src_read(u, ptr, size * nmemb) / (size ? size : 1); }
static int on_seek(void *u, ogg_int64_t off, int whence) { return src_seek(u, off, whence) ? 0 : -1; }
static int on_close(void *u) { return 0; }        // the player closes the file
static long on_tell(void *u) { return (long)src_tell(u); }

static void ogg_close(void *p)
{
    oggd_t *d = p;
    if (!d) return;
    if (d->tmp) ov_clear(&d->vf);
    pl_free(d->tmp);
    pl_free(d);
}

static void *ogg_open(src_t *s, track_info_t *ti, char *err, size_t n)
{
    oggd_t *d = pl_calloc(1, sizeof(*d));
    if (!d) {
        snprintf(err, n, "out of memory");
        return NULL;
    }
    ov_callbacks cb = { on_read, on_seek, on_close, on_tell };
    if (ov_open_callbacks(s, &d->vf, NULL, 0, cb) != 0) {
        snprintf(err, n, "not an Ogg Vorbis file");
        pl_free(d);
        return NULL;
    }
    vorbis_info *vi = ov_info(&d->vf, -1);
    d->channels = vi ? vi->channels : 0;
    d->tmp = d->channels > 0 ? pl_malloc(TMP_FRAMES * 8 * sizeof(int16_t)) : NULL;    // up to 8 channels
    if (!d->tmp) {
        ov_clear(&d->vf);
        pl_free(d);
        snprintf(err, n, "not playable");
        return NULL;
    }
    ti->rate = vi->rate;
    ti->channels = d->channels;
    ogg_int64_t total = ov_pcm_total(&d->vf, -1);
    ti->frames = total > 0 ? (uint64_t)total : 0;
    long br = ov_bitrate(&d->vf, -1);
    ti->kbps = br > 0 ? (uint16_t)(br / 1000) : 0;
    vorbis_comment *vc = ov_comment(&d->vf, -1);
    for (int i = 0; vc && i < vc->comments; i++) meta_vorbis_comment(ti, vc->user_comments[i], vc->comment_lengths[i]);
    return d;
}

static int ogg_read(void *p, int16_t *lr, int frames)
{
    oggd_t *d = p;
    int out = 0, holes = 0;
    while (out < frames) {
        int want = frames - out < TMP_FRAMES ? frames - out : TMP_FRAMES;
        // A chained stream may change channels: take the current one's.
        vorbis_info *vi = ov_info(&d->vf, -1);
        int ch = vi && vi->channels > 0 && vi->channels <= 8 ? vi->channels : d->channels;
        int bs;
        long r = ov_read(&d->vf, d->tmp, want * ch * 2, &bs);
        if (r == OV_HOLE && ++holes < 8) continue;           // a gap in the data: carry on
        if (r < 0) return out ? out : (int)r;
        if (r == 0) break;
        int got = (int)(r / 2 / ch);
        for (int i = 0; i < got; i++) {
            lr[2 * (out + i)] = d->tmp[i * ch];
            lr[2 * (out + i) + 1] = d->tmp[i * ch + (ch > 1)];
        }
        out += got;
    }
    return out;
}

static bool ogg_seek(void *p, uint64_t frame) { return ov_pcm_seek(&((oggd_t *)p)->vf, (ogg_int64_t)frame) == 0; }

const dec_ops_t dec_ogg = { "OGG", ogg_open, ogg_read, ogg_seek, ogg_close };
