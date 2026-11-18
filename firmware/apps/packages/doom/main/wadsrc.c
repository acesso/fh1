// WADs from a file or over HTTP (wadsrc.h). Over HTTP: one kept-alive connection, ranged GETs of whole 16 KB blocks,
// the blocks kept in PSRAM and the least recently used one reused when the cache is full. The game keeps what it
// reads in its own zone memory too, so a level's data is fetched once, while it loads.
#include "wadsrc.h"
#include "fh1_sdk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#define BLOCK (16 * 1024)
#define CACHE_BLOCKS 8                   // 128 KB (the game keeps the lumps it reads itself)
#define TRIES 4

typedef struct {
    bool http;
    FILE *f;                             // a file
    char host[64], path[128];            // over HTTP
    int port, sock;
    size_t size;
} src_t;

typedef struct {
    src_t *src;
    size_t block;                        // its number in the file
    uint32_t used;                       // when last read (for reuse)
    uint8_t *data;
} slot_t;

#define MAX_SRC 8

static slot_t s_cache[CACHE_BLOCKS];
static src_t *s_open[MAX_SRC];
static uint32_t s_clock;
static unsigned s_requests, s_bytes, s_wait_ms;

size_t wadsrc_cache_bytes(void)
{
    size_t n = 0;
    for (int i = 0; i < CACHE_BLOCKS; i++)
        if (s_cache[i].data) n += BLOCK;
    return n;
}

void wadsrc_stats(unsigned *requests, unsigned *kbytes, unsigned *ms)
{
    *requests = s_requests;
    *kbytes = s_bytes / 1024;
    *ms = s_wait_ms;
}

// ── HTTP ──

static void http_close(src_t *s)
{
    if (s->sock >= 0) close(s->sock);
    s->sock = -1;
}

static bool http_connect(src_t *s)
{
    http_close(s);
    // An IP address (a computer on the local network): no name lookup.
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)s->port) };
    unsigned b[4];
    if (sscanf(s->host, "%u.%u.%u.%u", &b[0], &b[1], &b[2], &b[3]) != 4 || b[0] > 255 || b[1] > 255 || b[2] > 255 || b[3] > 255) {
        fh1_log("wad: %s: give the computer's IP address (http://192.168.0.2:8077/...)", s->host);
        return false;
    }
    a.sin_addr.s_addr = htonl(b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]);
    s->sock = socket(AF_INET, SOCK_STREAM, 0);
    if (s->sock < 0) return false;
    struct timeval tv = { .tv_sec = 5 };
    setsockopt(s->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s->sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(s->sock, IPPROTO_TCP, 1 /* TCP_NODELAY */, &one, sizeof(one));
    if (connect(s->sock, (struct sockaddr *)&a, sizeof(a)) != 0) {
        http_close(s);
        return false;
    }
    return true;
}

static bool send_all(int sock, const char *b, size_t n)
{
    while (n) {
        const int k = send(sock, b, n, 0);
        if (k <= 0) return false;
        b += k, n -= (size_t)k;
    }
    return true;
}

static bool recv_all(int sock, uint8_t *b, size_t n)
{
    while (n) {
        const int k = recv(sock, b, n, 0);
        if (k <= 0) return false;
        b += k, n -= (size_t)k;
    }
    return true;
}

// The response's headers (up to the blank line): its status, Content-Length and the total from Content-Range.
static bool read_headers(int sock, int *status, long *length, long *total)
{
    static char h[1024];                 // (off the app's 8 KB stack)
    size_t n = 0;
    while (n + 1 < sizeof(h)) {
        if (recv(sock, h + n, 1, 0) != 1) return false;
        n++;
        if (n >= 4 && !memcmp(h + n - 4, "\r\n\r\n", 4)) break;
    }
    h[n] = 0;
    *status = 0, *length = -1, *total = -1;
    sscanf(h, "HTTP/1.%*d %d", status);
    for (char *l = strstr(h, "\r\n"); l; l = strstr(l + 2, "\r\n")) {
        if (!strncasecmp(l + 2, "Content-Length:", 15)) *length = atol(l + 17);
        if (!strncasecmp(l + 2, "Content-Range:", 14)) {
            const char *slash = strchr(l + 2, '/');
            if (slash) *total = atol(slash + 1);
        }
    }
    return *status != 0;
}

// Bytes [from, from + n) of the file, into dest (n > 0), retried on a new connection if one breaks.
static bool http_get(src_t *s, size_t from, size_t n, uint8_t *dest, long *total)
{
    char req[320];
    snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: %s:%d\r\nRange: bytes=%u-%u\r\nConnection: keep-alive\r\n\r\n",
             s->path, s->host, s->port, (unsigned)from, (unsigned)(from + n - 1));
    const uint32_t t0 = fh1_millis();
    for (int t = 0; t < TRIES; t++) {
        if (s->sock < 0 && !http_connect(s)) continue;
        int status;
        long length, tot;
        if (!send_all(s->sock, req, strlen(req)) || !read_headers(s->sock, &status, &length, &tot)) {
            http_close(s);
            continue;
        }
        if (status != 206 || length != (long)n) {
            fh1_log("wad: %s answered %d (%ld bytes) for %u-%u", s->host, status, length, (unsigned)from,
                    (unsigned)(from + n - 1));
            http_close(s);
            return false;
        }
        if (!recv_all(s->sock, dest, n)) {
            http_close(s);
            continue;
        }
        if (total) *total = tot;
        s_requests++;
        s_bytes += n;
        s_wait_ms += fh1_millis() - t0;
        return true;
    }
    return false;
}

// ── The cache ──

static uint8_t *block_data(src_t *s, size_t b)
{
    slot_t *free_slot = NULL, *oldest = NULL;
    for (int i = 0; i < CACHE_BLOCKS; i++) {
        slot_t *c = &s_cache[i];
        if (c->src == s && c->block == b && c->data) {
            c->used = ++s_clock;
            return c->data;
        }
        if (!c->src && !free_slot) free_slot = c;
        if (c->src && (!oldest || c->used < oldest->used)) oldest = c;
    }
    slot_t *c = free_slot ? free_slot : oldest;
    if (!c->data && !(c->data = malloc(BLOCK))) return NULL;
    const size_t from = b * BLOCK, n = from + BLOCK <= s->size ? BLOCK : s->size - from;
    c->src = NULL;
    if (!http_get(s, from, n, c->data, NULL)) return NULL;
    c->src = s;
    c->block = b;
    c->used = ++s_clock;
    return c->data;
}

// ── Opening and reading ──

void *wadsrc_open(const char *name, size_t *size)
{
    int slot = 0;
    while (slot < MAX_SRC && s_open[slot]) slot++;
    if (slot == MAX_SRC) return NULL;
    src_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->sock = -1;
    if (!strncmp(name, "http://", 7)) {
        s->http = true;
        s->port = 80;
        const char *h = name + 7, *slash = strchr(h, '/'), *colon = strchr(h, ':');
        if (!slash) slash = h + strlen(h);
        const char *hend = colon && colon < slash ? colon : slash;
        snprintf(s->host, sizeof(s->host), "%.*s", (int)(hend - h), h);
        if (colon && colon < slash) s->port = atoi(colon + 1);
        snprintf(s->path, sizeof(s->path), "%s", *slash ? slash : "/");
        uint8_t first[12];
        long total = -1;
        if (!http_get(s, 0, sizeof(first), first, &total) || total <= 0) {
            fh1_log("wad: %s does not answer", name);
            http_close(s);
            free(s);
            return NULL;
        }
        s->size = (size_t)total;
        fh1_log("wad: %s, %u KB, over the network", name, (unsigned)(s->size / 1024));
    } else {
        s->f = fopen(name, "rb");
        if (!s->f) {
            free(s);
            return NULL;
        }
        fseek(s->f, 0, SEEK_END);
        s->size = (size_t)ftell(s->f);
    }
    *size = s->size;
    s_open[slot] = s;
    return s;
}

bool wadsrc_read(void *src, size_t offset, void *dest, size_t size)
{
    src_t *s = src;
    if (!s->http) {
        fseek(s->f, (long)offset, SEEK_SET);
        return fread(dest, 1, size, s->f) == size;
    }
    uint8_t *d = dest;
    while (size) {
        const size_t b = offset / BLOCK, in = offset % BLOCK;
        const uint8_t *data = block_data(s, b);
        if (!data) return false;
        size_t n = BLOCK - in;
        if (n > size) n = size;
        if (b * BLOCK + in + n > s->size) return false;
        memcpy(d, data + in, n);
        d += n, offset += n, size -= n;
    }
    return true;
}

void wadsrc_close(void *src)
{
    src_t *s = src;
    if (!s) return;
    for (int i = 0; i < CACHE_BLOCKS; i++)
        if (s_cache[i].src == s) s_cache[i].src = NULL;
    if (s->http) http_close(s);
    else if (s->f) fclose(s->f);
    for (int i = 0; i < MAX_SRC; i++)
        if (s_open[i] == s) s_open[i] = NULL;
    free(s);
}

void wadsrc_close_all(void)
{
    for (int i = 0; i < MAX_SRC; i++)
        if (s_open[i]) wadsrc_close(s_open[i]);
    for (int i = 0; i < CACHE_BLOCKS; i++) {
        free(s_cache[i].data);
        s_cache[i] = (slot_t){ 0 };
    }
}
