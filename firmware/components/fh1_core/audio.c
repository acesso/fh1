// The audio.out class. The handle does not bind to a device: each call looks up the ready
// device, so a device that goes away mid-game turns the handle into the null driver and back.
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "fh1_audio.h"
#include "fh1_settings.h"

struct fh1_audio {
    uint32_t rate;
    uint8_t volume;
};

static fh1_audio_t s_fallback;     // handed out when out of memory: open never returns NULL

static char s_out[16];             // the output chosen in Inputs and outputs ("" = the first ready one)
static bool s_out_loaded;

// The chosen output when it is ready, else the first ready one.
static const fh1_audio_ops_t *ready_ops(fh1_device_t **dev)
{
    if (!s_out_loaded) {
        fh1_settings_get_str("audio", "out", s_out, sizeof(s_out));
        s_out_loaded = true;
    }
    fh1_device_t *d = s_out[0] ? fh1_dev_find(s_out) : NULL;
    if (!d || d->state != FH1_DEV_READY || strncmp(d->cls, "audio.out", 9) != 0) d = fh1_dev_ready_of_class("audio.out");
    *dev = d;
    return d ? (const fh1_audio_ops_t *)d->drv->ops : NULL;
}

void fh1_audio_set_output(const char *dev_id)
{
    snprintf(s_out, sizeof(s_out), "%s", dev_id ? dev_id : "");
    s_out_loaded = true;
    fh1_settings_set_str("audio", "out", s_out);
}

const char *fh1_audio_output(void)
{
    fh1_device_t *d;
    ready_ops(&d);
    return s_out;
}

fh1_audio_t *fh1_audio_open(uint32_t sample_rate)
{
    fh1_audio_t *a = calloc(1, sizeof(*a));
    if (!a) a = &s_fallback;
    a->rate = sample_rate;
    a->volume = 70;
    return a;
}

int fh1_audio_write(fh1_audio_t *a, const int16_t *frames, size_t count)
{
    fh1_device_t *d;
    const fh1_audio_ops_t *ops = ready_ops(&d);
    if (!a || !ops || !ops->write) return FH1_UNAVAILABLE;
    return ops->write(d, frames, count, a->rate);
}

int fh1_audio_write_stereo(fh1_audio_t *a, const int16_t *lr, size_t count)
{
    fh1_device_t *d;
    const fh1_audio_ops_t *ops = ready_ops(&d);
    if (!a || !ops) return FH1_UNAVAILABLE;
    if (ops->write_stereo) return ops->write_stereo(d, lr, count, a->rate);
    if (!ops->write) return FH1_UNAVAILABLE;
    // A mono-only device: mix down in small pieces.
    int16_t m[128];
    size_t done = 0;
    while (done < count) {
        size_t n = count - done < 128 ? count - done : 128;
        for (size_t i = 0; i < n; i++) m[i] = (int16_t)(((int)lr[2 * (done + i)] + lr[2 * (done + i) + 1]) / 2);
        int w = ops->write(d, m, n, a->rate);
        if (w <= 0) break;
        done += w;
        if ((size_t)w < n) break;
    }
    return (int)done;
}

void fh1_audio_set_volume(fh1_audio_t *a, uint8_t pct)
{
    fh1_device_t *d;
    const fh1_audio_ops_t *ops = ready_ops(&d);
    if (!a) return;
    a->volume = pct;
    if (ops && ops->set_volume) ops->set_volume(d, pct);
}

bool fh1_audio_available(void) { return fh1_dev_ready_of_class("audio.out") != NULL; }

uint8_t fh1_audio_master_volume(void) { return fh1_settings_get_u8("audio", "vol", 70); }

void fh1_audio_preview_volume(uint8_t pct)
{
    fh1_device_t *d;
    const fh1_audio_ops_t *ops = ready_ops(&d);
    if (ops && ops->set_volume) ops->set_volume(d, pct > 100 ? 100 : pct);
}

void fh1_audio_set_master_volume(uint8_t pct)
{
    if (pct > 100) pct = 100;
    fh1_settings_set_u8("audio", "vol", pct);
    fh1_audio_preview_volume(pct);
}

fh1_audio_channels_t fh1_audio_channels(void)
{
    return (fh1_audio_channels_t)(fh1_settings_get_u8("audio", "chan", FH1_AUDIO_STEREO) % FH1_AUDIO_CHANNEL_MODES);
}

void fh1_audio_set_channels(fh1_audio_channels_t mode)
{
    fh1_settings_set_u8("audio", "chan", (uint8_t)(mode % FH1_AUDIO_CHANNEL_MODES));
    fh1_device_t *d;
    const fh1_audio_ops_t *ops = ready_ops(&d);
    if (ops && ops->set_channels) ops->set_channels(d, mode % FH1_AUDIO_CHANNEL_MODES);
}

void fh1_audio_close(fh1_audio_t *a)
{
    if (a && a != &s_fallback) free(a);
}
