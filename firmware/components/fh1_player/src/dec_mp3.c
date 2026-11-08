// MP3 (MPEG-1/2/2.5 layer III, also I and II) with minimp3, streamed from the file. Tags: ID3v2.2-2.4 and
// ID3v1. Length: the Xing/Info or VBRI header of VBR files, else the bitrate of a CBR file. Seeking is by
// position in the file (through the Xing table of contents when there is one): close, not sample-exact.
#include <string.h>
#include "player_priv.h"

static void *s_scratch;                     // minimp3's scratch (~16 KB, PSRAM): one MP3 plays at a time
#define MINIMP3_SCRATCH s_scratch
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#define INBUF (16 * 1024)                   // several frames: minimp3 checks a few in a row to find sync

typedef struct {
    mp3dec_t mp3;
    src_t *s;
    uint8_t *in;
    int in_len;
    bool eof;
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int pcm_n, pcm_pos, pcm_ch;             // frames decoded, frames handed out, their channels
    int64_t data_start, data_end;           // the audio, without the tags
    uint64_t frames;
    uint8_t toc[100];
    bool has_toc;
} mp3d_t;

// ── Tags ──

static void put_utf8(char **o, char *end, uint32_t cp)
{
    if (cp < 0x80 && *o + 1 < end) *(*o)++ = (char)cp;
    else if (cp < 0x800 && *o + 2 < end) {
        *(*o)++ = (char)(0xC0 | cp >> 6);
        *(*o)++ = (char)(0x80 | (cp & 0x3F));
    } else if (cp >= 0x800 && cp < 0x10000 && *o + 3 < end) {
        *(*o)++ = (char)(0xE0 | cp >> 12);
        *(*o)++ = (char)(0x80 | (cp >> 6 & 0x3F));
        *(*o)++ = (char)(0x80 | (cp & 0x3F));
    }
}

// An ID3 text frame's body (encoding byte, then the text) into UTF-8.
static void id3_text(char *dst, size_t n, const uint8_t *b, size_t len)
{
    if (!len || dst[0]) return;
    char tmp[200], *o = tmp, *end = tmp + sizeof(tmp);
    uint8_t enc = b[0];
    b++;
    len--;
    if (enc == 1 || enc == 2) {                         // UTF-16 with a byte-order mark, or big-endian
        bool be = enc == 2;
        size_t i = 0;
        if (enc == 1 && len >= 2) {
            be = b[0] == 0xFE && b[1] == 0xFF;
            i = 2;
        }
        for (; i + 1 < len; i += 2) {
            uint32_t cp = be ? (b[i] << 8 | b[i + 1]) : (b[i + 1] << 8 | b[i]);
            if (!cp) break;
            if (cp < 0xD800 || cp > 0xDFFF) put_utf8(&o, end, cp);
        }
    } else {
        for (size_t i = 0; i < len && b[i]; i++) {
            if (enc == 3) {
                if (o + 1 < end) *o++ = (char)b[i];           // already UTF-8
            } else {
                put_utf8(&o, end, b[i]);                      // ISO-8859-1
            }
        }
    }
    meta_copy(dst, n, tmp, o - tmp);
}

static uint32_t syncsafe(const uint8_t *b) { return (b[0] & 0x7F) << 21 | (b[1] & 0x7F) << 14 | (b[2] & 0x7F) << 7 | (b[3] & 0x7F); }

// Reads the frames we want from an ID3v2 tag at the file's start; returns the tag's size (0: none).
static int64_t id3v2(src_t *s, track_info_t *ti)
{
    uint8_t h[10];
    if (src_read(s, h, 10) != 10 || memcmp(h, "ID3", 3) != 0) return 0;
    const int ver = h[3];
    int64_t size = 10 + syncsafe(h + 6) + (h[5] & 0x10 ? 10 : 0), pos = 10;
    if (h[5] & 0x40 && ver >= 3) {                      // extended header: skip it
        uint8_t e[4];
        if (src_read(s, e, 4) != 4) return size;
        pos += ver == 4 ? syncsafe(e) : (uint32_t)(e[0] << 24 | e[1] << 16 | e[2] << 8 | e[3]) + 4;
    }
    const int hl = ver == 2 ? 6 : 10;
    for (int frames = 0; pos + hl < size && frames < 64; frames++) {
        uint8_t f[10];
        if (!src_seek(s, pos, SEEK_SET) || src_read(s, f, hl) != (size_t)hl || !f[0]) break;
        uint32_t len = ver == 2 ? (uint32_t)(f[3] << 16 | f[4] << 8 | f[5])
                     : ver == 4 ? syncsafe(f + 4) : (uint32_t)(f[4] << 24 | f[5] << 16 | f[6] << 8 | f[7]);
        pos += hl + len;
        char *dst = NULL;
        size_t dn = 0;
        if (ver == 2) {
            if (!memcmp(f, "TT2", 3)) dst = ti->title, dn = sizeof(ti->title);
            else if (!memcmp(f, "TP1", 3)) dst = ti->artist, dn = sizeof(ti->artist);
            else if (!memcmp(f, "TAL", 3)) dst = ti->album, dn = sizeof(ti->album);
        } else {
            if (!memcmp(f, "TIT2", 4)) dst = ti->title, dn = sizeof(ti->title);
            else if (!memcmp(f, "TPE1", 4)) dst = ti->artist, dn = sizeof(ti->artist);
            else if (!memcmp(f, "TALB", 4)) dst = ti->album, dn = sizeof(ti->album);
        }
        if (dst && len > 1) {
            uint8_t b[256];
            size_t k = len < sizeof(b) ? len : sizeof(b);
            id3_text(dst, dn, b, src_read(s, b, k));
        }
    }
    return size;
}

// ID3v1 (the last 128 bytes): fills what ID3v2 did not; returns true if there is one.
static bool id3v1(src_t *s, track_info_t *ti)
{
    uint8_t t[128];
    if (s->size < 128 || !src_seek(s, s->size - 128, SEEK_SET) || src_read(s, t, 128) != 128 || memcmp(t, "TAG", 3)) return false;
    char *dst[3] = { ti->title, ti->artist, ti->album };
    size_t dn[3] = { sizeof(ti->title), sizeof(ti->artist), sizeof(ti->album) };
    for (int i = 0; i < 3; i++) {
        uint8_t b[31] = { 0 };                          // as an ID3v2 frame body: ISO-8859-1, then the text
        memcpy(b + 1, t + 3 + 30 * i, 30);
        id3_text(dst[i], dn[i], b, 31);
    }
    return true;
}

// ── Stream ──

static void refill(mp3d_t *d)
{
    if (d->eof || d->in_len >= INBUF) return;
    int64_t at = src_tell(d->s);
    int64_t want = INBUF - d->in_len;
    if (at + want > d->data_end) want = d->data_end - at;
    size_t r = want > 0 ? src_read(d->s, d->in + d->in_len, (size_t)want) : 0;
    d->in_len += (int)r;
    if ((int64_t)r < want || want <= 0) d->eof = true;
}

static void consume(mp3d_t *d, int n)
{
    if (n > d->in_len) n = d->in_len;
    memmove(d->in, d->in + n, d->in_len - n);
    d->in_len -= n;
}

// Next frame's samples into pcm; false at the end.
static bool next_frame(mp3d_t *d, mp3dec_frame_info_t *info)
{
    for (int tries = 0; tries < 64; tries++) {
        refill(d);
        if (!d->in_len) return false;
        int n = mp3dec_decode_frame(&d->mp3, d->in, d->in_len, d->pcm, info);
        if (!info->frame_bytes) {                       // no frame in what is left
            if (d->eof) return false;
            consume(d, d->in_len > 4 ? d->in_len - 4 : 0);
            continue;
        }
        consume(d, info->frame_bytes);
        if (n > 0) {
            d->pcm_n = n;
            d->pcm_pos = 0;
            d->pcm_ch = info->channels;
            return true;
        }
    }
    return false;
}

// The Xing/Info header (VBR) in the first frame: the number of frames, the bytes and a table of contents.
static void xing(mp3d_t *d, const uint8_t *fr, int len, const mp3dec_frame_info_t *info, uint64_t *frames, uint64_t *bytes)
{
    const bool mpeg1 = fr[1] & 0x08;
    const int side = mpeg1 ? (info->channels == 1 ? 17 : 32) : (info->channels == 1 ? 9 : 17);
    const uint8_t *x = fr + 4 + side;
    if (len >= 4 + side + 16 && (!memcmp(x, "Xing", 4) || !memcmp(x, "Info", 4))) {
        uint32_t flags = x[4] << 24 | x[5] << 16 | x[6] << 8 | x[7];
        const uint8_t *p = x + 8;
        if (flags & 1) {
            *frames = (uint32_t)(p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]);
            p += 4;
        }
        if (flags & 2) {
            *bytes = (uint32_t)(p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]);
            p += 4;
        }
        if (flags & 4 && p + 100 <= fr + len) {
            memcpy(d->toc, p, 100);
            d->has_toc = true;
        }
    } else if (len >= 4 + 32 + 18 && !memcmp(fr + 36, "VBRI", 4)) {
        const uint8_t *v = fr + 36;
        *bytes = (uint32_t)(v[10] << 24 | v[11] << 16 | v[12] << 8 | v[13]);
        *frames = (uint32_t)(v[14] << 24 | v[15] << 16 | v[16] << 8 | v[17]);
    }
}

static void mp3_close(void *p)
{
    mp3d_t *d = p;
    if (d) pl_free(d->in);
    pl_free(d);
    pl_free(s_scratch);
    s_scratch = NULL;
}

static void *mp3_open(src_t *s, track_info_t *ti, char *err, size_t n)
{
    mp3d_t *d = pl_calloc(1, sizeof(*d));
    if (!s_scratch) s_scratch = pl_malloc(sizeof(mp3dec_scratch_t));
    if (!d || !s_scratch || !(d->in = pl_malloc(INBUF))) {
        snprintf(err, n, "out of memory");
        mp3_close(d);
        return NULL;
    }
    d->s = s;
    d->data_start = id3v2(s, ti);
    d->data_end = s->size;
    if (id3v1(s, ti)) d->data_end -= 128;
    src_seek(s, d->data_start, SEEK_SET);
    mp3dec_init(&d->mp3);

    // The first frame: the format, and a Xing/VBRI header if the file has one.
    refill(d);
    mp3dec_frame_info_t info = { 0 };
    int skipped = 0;
    int got = 0;
    while (d->in_len && skipped < 256 * 1024) {
        got = mp3dec_decode_frame(&d->mp3, d->in, d->in_len, d->pcm, &info);
        if (info.frame_bytes) break;
        skipped += d->in_len > 4 ? d->in_len - 4 : d->in_len;
        consume(d, d->in_len > 4 ? d->in_len - 4 : d->in_len);
        if (d->eof) break;
        refill(d);
    }
    if (!info.frame_bytes || !info.hz) {
        snprintf(err, n, "no MP3 audio found");
        mp3_close(d);
        return NULL;
    }
    // The frame decoded above starts where minimp3 found sync: frame_offset bytes in.
    const uint8_t *fr = d->in + info.frame_offset;
    const int spf = info.layer == 3 && info.hz < 32000 ? 576 : info.layer == 1 ? 384 : 1152;
    uint64_t xframes = 0, xbytes = 0;
    xing(d, fr, info.frame_bytes - info.frame_offset, &info, &xframes, &xbytes);
    d->data_start += skipped + info.frame_offset;
    consume(d, info.frame_bytes);
    if (xframes) {
        d->frames = xframes * spf;
        ti->kbps = xbytes ? (uint16_t)(xbytes * 8 / (d->frames * 1000 / info.hz ? d->frames * 1000 / info.hz : 1)) : info.bitrate_kbps;
        d->pcm_n = 0;                                   // the Xing frame is silence: not played
    } else {
        ti->kbps = info.bitrate_kbps;
        if (info.bitrate_kbps) d->frames = (uint64_t)(d->data_end - d->data_start) * 8 / info.bitrate_kbps * info.hz / 1000;
        d->pcm_n = got;                                 // a real frame: played
        d->pcm_pos = 0;
        d->pcm_ch = info.channels;
    }
    ti->rate = info.hz;
    ti->channels = info.channels;
    ti->frames = d->frames;
    return d;
}

static int mp3_read(void *p, int16_t *lr, int frames)
{
    mp3d_t *d = p;
    int out = 0;
    while (out < frames) {
        if (d->pcm_pos >= d->pcm_n) {
            mp3dec_frame_info_t info;
            if (!next_frame(d, &info)) break;
        }
        int k = d->pcm_n - d->pcm_pos;
        if (k > frames - out) k = frames - out;
        const int16_t *src = d->pcm + d->pcm_pos * d->pcm_ch;
        if (d->pcm_ch == 2) {
            memcpy(lr + 2 * out, src, k * 4);
        } else {
            for (int i = 0; i < k; i++) lr[2 * (out + i)] = lr[2 * (out + i) + 1] = src[i];
        }
        d->pcm_pos += k;
        out += k;
    }
    return out;
}

static bool mp3_seek(void *p, uint64_t frame)
{
    mp3d_t *d = p;
    if (!d->frames) return false;
    double f = (double)frame / d->frames;
    if (f > 0.999) f = 0.999;
    int64_t span = d->data_end - d->data_start, at;
    if (d->has_toc) {
        double pct = f * 100;
        int i = (int)pct;
        double a = d->toc[i], b = i < 99 ? d->toc[i + 1] : 256;
        at = d->data_start + (int64_t)((a + (b - a) * (pct - i)) / 256.0 * span);
    } else {
        at = d->data_start + (int64_t)(f * span);
    }
    if (!src_seek(d->s, at, SEEK_SET)) return false;
    d->in_len = 0;
    d->eof = false;
    d->pcm_n = d->pcm_pos = 0;
    mp3dec_init(&d->mp3);
    return true;
}

const dec_ops_t dec_mp3 = { "MP3", mp3_open, mp3_read, mp3_seek, mp3_close };
