// The player engine: a task (core 0) that decodes and feeds the audio output and takes commands from a queue.
// Its stack is in PSRAM (16 KB: Tremor and dr_flac are deep), so playing costs almost no internal RAM. A task
// with its stack in PSRAM must not touch the flash (internal storage is flash), so the file is read by a small
// helper task with an internal stack (io_task). Both are made on the first play and stay. Decoders: dec_*.c.
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "fh1_player.h"
#include "player_priv.h"
#include "fh1_audio.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

static const char *TAG = "player";

#define CHUNK 1024                    // frames decoded per step (23 ms at 44.1 kHz)
#define TAP_FRAMES 8192               // what the visualisers can look back at (~0.19 s at 44.1 kHz)
#define OUT_LATENCY 1700              // frames between our write and the speaker (the I2S queue, ~40 ms)
#define STACK 16384                   // PSRAM; the deepest decoder measured used ~9 KB (Ogg)
#define IO_STACK 3584                 // internal; fread through the VFS and the SD or flash driver

typedef enum { CMD_PLAY, CMD_PAUSE, CMD_RESUME, CMD_TOGGLE, CMD_STOP, CMD_SEEK } cmd_type_t;
typedef struct {
    cmd_type_t type;
    int32_t arg;
    char *path;                       // CMD_PLAY, PSRAM, freed by the task
} cmd_t;

static SemaphoreHandle_t s_mu;        // the status
static QueueHandle_t s_q;
static fh1_player_status_t s_st;
static void (*s_on_end)(void *);
static void *s_on_end_user;

static int16_t *s_tap;                // TAP_FRAMES stereo frames, PSRAM
static volatile uint32_t s_tap_wr;    // frames written in all
static volatile uint32_t s_tap_rate;

// ── Memory ──

void *pl_malloc(size_t n) { return heap_caps_malloc_prefer(n, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT); }
void *pl_calloc(size_t n, size_t size)
{
    return heap_caps_calloc_prefer(n, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
}
void *pl_realloc(void *p, size_t n)
{
    return heap_caps_realloc_prefer(p, n, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
}
void pl_free(void *p) { heap_caps_free(p); }

// ── The file ──

// Every file operation runs on io_task: the caller (the player, PSRAM stack) waits for it.
typedef enum { IO_OPEN, IO_READ, IO_SEEK, IO_TELL, IO_CLOSE } io_op_t;
typedef struct {
    io_op_t op;
    src_t *s;
    const char *path;
    void *dst;
    size_t n;
    int64_t off;
    int whence;
    int64_t ret;
} io_req_t;
static QueueHandle_t s_io_q;
static SemaphoreHandle_t s_io_done;

static void io_task(void *arg)
{
    for (;;) {
        io_req_t *r;
        if (xQueueReceive(s_io_q, &r, portMAX_DELAY) != pdTRUE) continue;
        src_t *s = r->s;
        switch (r->op) {
        case IO_OPEN:
            s->f = fopen(r->path, "rb");
            if (s->f) {
                setvbuf(s->f, NULL, _IONBF, 0);      // our reads are big already: no second copy
                fseek(s->f, 0, SEEK_END);
                s->size = ftell(s->f);
                fseek(s->f, 0, SEEK_SET);
            }
            r->ret = s->f != NULL;
            break;
        case IO_READ: {                              // through the internal buffer: the SD card reads by DMA
            size_t done = 0;
            while (done < r->n) {
                size_t k = r->n - done > 4096 ? 4096 : r->n - done;
                size_t got = fread(s->bounce, 1, k, s->f);
                memcpy((uint8_t *)r->dst + done, s->bounce, got);
                done += got;
                if (got < k) break;
            }
            r->ret = (int64_t)done;
            break;
        }
        case IO_SEEK: r->ret = fseek(s->f, (long)r->off, r->whence) == 0; break;
        case IO_TELL: r->ret = ftell(s->f); break;
        case IO_CLOSE:
            fclose(s->f);
            s->f = NULL;
            r->ret = 0;
            break;
        }
        xSemaphoreGive(s_io_done);
    }
}

static int64_t io(io_req_t *r)
{
    xQueueSend(s_io_q, &r, portMAX_DELAY);
    xSemaphoreTake(s_io_done, portMAX_DELAY);
    return r->ret;
}

size_t src_read(src_t *s, void *dst, size_t n) { return (size_t)io(&(io_req_t){ .op = IO_READ, .s = s, .dst = dst, .n = n }); }
bool src_seek(src_t *s, int64_t off, int whence) { return io(&(io_req_t){ .op = IO_SEEK, .s = s, .off = off, .whence = whence }); }
int64_t src_tell(src_t *s) { return io(&(io_req_t){ .op = IO_TELL, .s = s }); }

// ── Tags ──

void meta_copy(char *dst, size_t n, const char *src, size_t len)
{
    while (len && (src[len - 1] == ' ' || src[len - 1] == 0)) len--;
    while (len && *src == ' ') {
        src++;
        len--;
    }
    if (len >= n) len = n - 1;
    memcpy(dst, src, len);
    dst[len] = 0;
}

void meta_vorbis_comment(track_info_t *ti, const char *c, size_t len)
{
    static const struct { const char *key; size_t off, size; } keys[] = {
        { "TITLE=", offsetof(track_info_t, title), sizeof(ti->title) },
        { "ARTIST=", offsetof(track_info_t, artist), sizeof(ti->artist) },
        { "ALBUM=", offsetof(track_info_t, album), sizeof(ti->album) },
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        size_t k = strlen(keys[i].key);
        char *dst = (char *)ti + keys[i].off;
        if (len > k && strncasecmp(c, keys[i].key, k) == 0 && !dst[0]) meta_copy(dst, keys[i].size, c + k, len - k);
    }
}

// ── Formats ──

static const struct { const char *ext; const dec_ops_t *ops; } s_formats[] = {
    { "mp3", &dec_mp3 }, { "flac", &dec_flac }, { "ogg", &dec_ogg }, { "oga", &dec_ogg },
    { "wav", &dec_wav }, { "mod", &dec_mod },
};

static const dec_ops_t *format_of(const char *name)
{
    const char *dot = strrchr(name, '.');
    for (size_t i = 0; dot && i < sizeof(s_formats) / sizeof(s_formats[0]); i++) {
        if (strcasecmp(dot + 1, s_formats[i].ext) == 0) return s_formats[i].ops;
    }
    return NULL;
}

bool fh1_player_supported(const char *name) { return format_of(name) != NULL; }

// "Artist - 03 Song_name.mp3" -> "Artist - 03 Song name"
static void title_from_path(const char *path, char *out, size_t n)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *dot = strrchr(base, '.');
    size_t len = dot ? (size_t)(dot - base) : strlen(base);
    meta_copy(out, n, base, len);
    for (char *p = out; *p; p++) {
        if (*p == '_') *p = ' ';
    }
}

// ── Status ──

static void set_state(fh1_player_state_t st)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_st.state = st;
    xSemaphoreGive(s_mu);
}

static void set_error(const char *path, const char *err)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_st.state = FH1_PLAYER_STOPPED;
    snprintf(s_st.error, sizeof(s_st.error), "%s", err);
    s_st.serial++;
    xSemaphoreGive(s_mu);
    ESP_LOGW(TAG, "%s: %s", path, err);
}

void fh1_player_status(fh1_player_status_t *st)
{
    if (!s_mu || !s_q) {
        memset(st, 0, sizeof(*st));
        st->format = "";
        return;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    *st = s_st;
    xSemaphoreGive(s_mu);
}

void fh1_player_on_end(void (*cb)(void *), void *user)
{
    s_on_end = cb;
    s_on_end_user = user;
}

size_t fh1_player_tap(int16_t *lr, size_t n, uint32_t *rate)
{
    if (rate) *rate = s_tap_rate;
    if (!s_tap) return 0;
    uint32_t end = s_tap_wr;
    end = end > OUT_LATENCY ? end - OUT_LATENCY : 0;
    if (n > end) n = end;
    if (n > TAP_FRAMES - CHUNK) n = TAP_FRAMES - CHUNK;      // never the part being written
    uint32_t from = end - n;
    for (size_t i = 0; i < n;) {
        uint32_t at = (from + i) % TAP_FRAMES;
        size_t k = TAP_FRAMES - at < n - i ? TAP_FRAMES - at : n - i;
        memcpy(lr + 2 * i, s_tap + 2 * at, k * 4);
        i += k;
    }
    return n;
}

static void tap_put(const int16_t *lr, int frames)
{
    for (int i = 0; i < frames;) {
        uint32_t at = s_tap_wr % TAP_FRAMES;
        int k = TAP_FRAMES - at < (uint32_t)(frames - i) ? (int)(TAP_FRAMES - at) : frames - i;
        memcpy(s_tap + 2 * at, lr + 2 * i, k * 4);
        i += k;
        s_tap_wr += k;
    }
}

// ── The task ──

typedef struct {
    src_t src;
    const dec_ops_t *ops;
    void *dec;
    fh1_audio_t *audio;
    uint64_t frame;                   // frames handed to the output
    uint32_t rate;
    uint64_t total;
} track_t;

static void track_close(track_t *t)
{
    // The stack is in PSRAM: measuring it scans 16 KB of it, so only at debug level.
    if (t->dec && esp_log_level_get(TAG) >= ESP_LOG_DEBUG)
        ESP_LOGD(TAG, "%s: stack %u bytes never used", t->ops->name, (unsigned)uxTaskGetStackHighWaterMark(NULL));
    if (t->dec) t->ops->close(t->dec);
    if (t->src.f) io(&(io_req_t){ .op = IO_CLOSE, .s = &t->src });
    if (t->audio) fh1_audio_close(t->audio);
    heap_caps_free(t->src.bounce);
    memset(t, 0, sizeof(*t));
}

static bool track_open(track_t *t, const char *path)
{
    char err[64] = "";
    track_info_t ti = { 0 };
    t->ops = format_of(path);
    t->src.bounce = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (t->ops && t->src.bounce) io(&(io_req_t){ .op = IO_OPEN, .s = &t->src, .path = path });
    if (!t->ops) snprintf(err, sizeof(err), "unknown format");
    else if (!t->src.bounce) snprintf(err, sizeof(err), "out of memory");
    else if (!t->src.f) snprintf(err, sizeof(err), "cannot open the file");
    if (err[0]) {
        set_error(path, err);
        track_close(t);
        return false;
    }
    t->dec = t->ops->open(&t->src, &ti, err, sizeof(err));
    if (!t->dec || !ti.rate || ti.rate > 96000) {
        set_error(path, err[0] ? err : "not a playable file");
        track_close(t);
        return false;
    }
    t->rate = ti.rate;
    t->total = ti.frames;
    t->audio = fh1_audio_open(ti.rate);
    s_tap_rate = ti.rate;
    if (!ti.title[0]) title_from_path(path, ti.title, sizeof(ti.title));

    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_st.state = FH1_PLAYER_PLAYING;
    snprintf(s_st.path, sizeof(s_st.path), "%s", path);
    memcpy(s_st.title, ti.title, sizeof(s_st.title));
    memcpy(s_st.artist, ti.artist, sizeof(s_st.artist));
    memcpy(s_st.album, ti.album, sizeof(s_st.album));
    s_st.format = t->ops->name;
    s_st.rate = ti.rate;
    s_st.channels = ti.channels;
    s_st.kbps = ti.kbps;
    s_st.pos_ms = 0;
    s_st.dur_ms = (uint32_t)(ti.frames * 1000 / ti.rate);
    s_st.error[0] = 0;
    s_st.serial++;
    xSemaphoreGive(s_mu);
    ESP_LOGI(TAG, "%s: %s, %lu Hz, %u ch, %u kbps, %lu s", path, t->ops->name, (unsigned long)ti.rate, ti.channels,
             ti.kbps, (unsigned long)(ti.frames / ti.rate));
    return true;
}

// Hands frames to the output; without a sound device, keeps time so the position and visualisers still run.
static void output(track_t *t, const int16_t *lr, int frames)
{
    tap_put(lr, frames);
    int done = 0, idle = 0;
    while (done < frames) {
        int w = fh1_audio_write_stereo(t->audio, lr + 2 * done, frames - done);
        if (w < 0) {
            vTaskDelay(pdMS_TO_TICKS((frames - done) * 1000 / t->rate) + 1);
            break;
        }
        if (w == 0 && ++idle > 20) break;
        done += w;
    }
    t->frame += frames;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_st.pos_ms = (uint32_t)(t->frame * 1000 / t->rate);
    xSemaphoreGive(s_mu);
}

static void seek(track_t *t, int32_t delta_ms)
{
    int64_t target = (int64_t)t->frame + (int64_t)delta_ms * t->rate / 1000;
    if (target < 0) target = 0;
    if (t->total && (uint64_t)target >= t->total) target = t->total - 1;
    if (t->ops->seek && t->ops->seek(t->dec, (uint64_t)target)) t->frame = (uint64_t)target;
    s_tap_wr += TAP_FRAMES;           // the visualisers start over (the old frames no longer apply)
    memset(s_tap, 0, TAP_FRAMES * 4);
}

static void player_task(void *arg)
{
    track_t t = { 0 };
    int16_t *buf = heap_caps_malloc(CHUNK * 4, MALLOC_CAP_SPIRAM);
    bool paused = false;
    for (;;) {
        cmd_t c;
        // Playing: take commands without waiting. Paused or between tracks: wait for one.
        TickType_t wait = t.dec && !paused ? 0 : portMAX_DELAY;
        if (xQueueReceive(s_q, &c, wait) == pdTRUE) {
            switch (c.type) {
            case CMD_PLAY:
                track_close(&t);
                paused = false;
                set_state(FH1_PLAYER_LOADING);
                memset(s_tap, 0, TAP_FRAMES * 4);
                if (buf) track_open(&t, c.path);
                heap_caps_free(c.path);
                break;
            case CMD_STOP:
                track_close(&t);
                set_state(FH1_PLAYER_STOPPED);
                break;
            case CMD_PAUSE:
            case CMD_RESUME:
            case CMD_TOGGLE:
                if (t.dec) {
                    paused = c.type == CMD_TOGGLE ? !paused : c.type == CMD_PAUSE;
                    set_state(paused ? FH1_PLAYER_PAUSED : FH1_PLAYER_PLAYING);
                }
                break;
            case CMD_SEEK:
                if (t.dec) seek(&t, c.arg);
                break;
            }
            continue;
        }
        if (!t.dec || paused) continue;
        int n = t.ops->read(t.dec, buf, CHUNK);
        if (n > 0) {
            output(&t, buf, n);
        } else {
            bool ended = n == 0;
            char path[160];
            snprintf(path, sizeof(path), "%s", s_st.path);
            track_close(&t);
            if (ended) {
                set_state(FH1_PLAYER_STOPPED);
                ESP_LOGI(TAG, "track ended");
                if (s_on_end) s_on_end(s_on_end_user);
            } else {
                set_error(path, "decoding error");
            }
        }
    }
}

// The tasks, on the first command. The player's control block must be internal; its stack is PSRAM.
static bool start(void)
{
    static bool started, failed;
    if (started || failed) return started;
    static StaticTask_t *tcb;
    static StackType_t *stack;
    s_mu = xSemaphoreCreateMutex();
    s_q = xQueueCreate(6, sizeof(cmd_t));
    s_io_q = xQueueCreate(1, sizeof(io_req_t *));
    s_io_done = xSemaphoreCreateBinary();
    s_tap = heap_caps_calloc(TAP_FRAMES, 4, MALLOC_CAP_SPIRAM);
    tcb = heap_caps_calloc(1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    stack = heap_caps_malloc(STACK, MALLOC_CAP_SPIRAM);
    s_st.format = "";
    started = s_mu && s_q && s_io_q && s_io_done && s_tap && tcb && stack &&
              xTaskCreatePinnedToCore(io_task, "player_io", IO_STACK, NULL, 6, NULL, 0) == pdPASS &&
              xTaskCreateStaticPinnedToCore(player_task, "player", STACK, NULL, 5, stack, tcb, 0) != NULL;
    failed = !started;
    if (failed) ESP_LOGE(TAG, "could not start (no memory)");
    return started;
}

static esp_err_t send(cmd_type_t type, int32_t arg, char *path)
{
    cmd_t c = { type, arg, path };
    if (!start() || xQueueSend(s_q, &c, pdMS_TO_TICKS(200)) != pdTRUE) {
        heap_caps_free(path);
        return s_q ? ESP_ERR_TIMEOUT : ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t fh1_player_play(const char *path)
{
    char *p = heap_caps_malloc(strlen(path) + 1, MALLOC_CAP_SPIRAM);
    if (!p) return ESP_ERR_NO_MEM;
    strcpy(p, path);
    return send(CMD_PLAY, 0, p);
}

void fh1_player_pause(bool paused) { send(paused ? CMD_PAUSE : CMD_RESUME, 0, NULL); }
void fh1_player_toggle(void) { send(CMD_TOGGLE, 0, NULL); }
void fh1_player_stop(void) { send(CMD_STOP, 0, NULL); }
void fh1_player_seek(int32_t delta_ms) { send(CMD_SEEK, delta_ms, NULL); }
