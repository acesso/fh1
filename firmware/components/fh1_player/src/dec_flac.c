// FLAC with dr_flac: tags from its Vorbis comment block, sample-exact seeking.
#include <string.h>
#include "player_priv.h"

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#include "dr_flac.h"

// dr_flac's user pointer: the source, and (while opening) where the tags go.
typedef struct {
    src_t *s;
    track_info_t *ti;
} open_ctx_t;

typedef struct {
    drflac *f;
    open_ctx_t *ctx;
    int16_t *tmp;                   // the file's channels, before they become stereo
} flacd_t;

#define TMP_FRAMES 1024

static size_t on_read(void *u, void *buf, size_t n) { return src_read(u, buf, n); }
static drflac_bool32 on_seek(void *u, int off, drflac_seek_origin o)
{
    return src_seek(u, off, o == DRFLAC_SEEK_SET ? SEEK_SET : o == DRFLAC_SEEK_END ? SEEK_END : SEEK_CUR);
}
static drflac_bool32 on_tell(void *u, drflac_int64 *at)
{
    *at = src_tell(u);
    return *at >= 0;
}
static void on_meta(track_info_t *ti, drflac_metadata *m)
{
    if (!ti || m->type != DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) return;
    drflac_vorbis_comment_iterator it;
    drflac_init_vorbis_comment_iterator(&it, m->data.vorbis_comment.commentCount, m->data.vorbis_comment.pComments);
    drflac_uint32 len;
    const char *c;
    while ((c = drflac_next_vorbis_comment(&it, &len)) != NULL) meta_vorbis_comment(ti, c, len);
}

static void *a_malloc(size_t n, void *u) { return pl_malloc(n); }
static void *a_realloc(void *p, size_t n, void *u) { return pl_realloc(p, n); }
static void a_free(void *p, void *u) { pl_free(p); }
static const drflac_allocation_callbacks s_alloc = { NULL, a_malloc, a_realloc, a_free };

static void flac_close(void *p)
{
    flacd_t *d = p;
    if (!d) return;
    if (d->f) drflac_close(d->f);
    pl_free(d->ctx);
    pl_free(d->tmp);
    pl_free(d);
}

static size_t ctx_read(void *u, void *buf, size_t n) { return on_read(((open_ctx_t *)u)->s, buf, n); }
static drflac_bool32 ctx_seek(void *u, int off, drflac_seek_origin o) { return on_seek(((open_ctx_t *)u)->s, off, o); }
static drflac_bool32 ctx_tell(void *u, drflac_int64 *at) { return on_tell(((open_ctx_t *)u)->s, at); }
static void ctx_meta(void *u, drflac_metadata *m) { on_meta(((open_ctx_t *)u)->ti, m); }

static void *flac_open(src_t *s, track_info_t *ti, char *err, size_t n)
{
    flacd_t *d = pl_calloc(1, sizeof(*d));
    // The context must outlive the open: dr_flac keeps the user pointer for every later read.
    open_ctx_t *ctx = d ? pl_malloc(sizeof(*ctx)) : NULL;
    if (!ctx) {
        pl_free(d);
        snprintf(err, n, "out of memory");
        return NULL;
    }
    ctx->s = s;
    ctx->ti = ti;
    d->ctx = ctx;
    d->f = drflac_open_with_metadata(ctx_read, ctx_seek, ctx_tell, ctx_meta, ctx, &s_alloc);
    d->tmp = pl_malloc(TMP_FRAMES * 8 * sizeof(int16_t));
    if (!d->f || !d->tmp) {
        snprintf(err, n, d->f ? "out of memory" : "not a FLAC file");
        flac_close(d);
        return NULL;
    }
    ctx->ti = NULL;                                  // only valid while opening
    ti->rate = d->f->sampleRate;
    ti->channels = d->f->channels;
    ti->frames = d->f->totalPCMFrameCount;
    if (ti->frames && ti->rate) ti->kbps = (uint16_t)(s->size * 8 / (ti->frames * 1000 / ti->rate + 1));
    return d;
}

static int flac_read(void *p, int16_t *lr, int frames)
{
    flacd_t *d = p;
    const int ch = d->f->channels;
    int out = 0;
    while (out < frames) {
        int want = frames - out < TMP_FRAMES ? frames - out : TMP_FRAMES;
        int got = (int)drflac_read_pcm_frames_s16(d->f, want, d->tmp);
        for (int i = 0; i < got; i++) {
            lr[2 * (out + i)] = d->tmp[i * ch];
            lr[2 * (out + i) + 1] = d->tmp[i * ch + (ch > 1)];
        }
        out += got;
        if (got < want) break;
    }
    return out;
}

static bool flac_seek(void *p, uint64_t frame) { return drflac_seek_to_pcm_frame(((flacd_t *)p)->f, frame); }

const dec_ops_t dec_flac = { "FLAC", flac_open, flac_read, flac_seek, flac_close };
