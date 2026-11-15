// Network play's session (netplay.h). Every packet starts with "FH1P", the version, its type and the session (a
// number the host picks; 0 before one is known). Numbers are little-endian, texts NUL-padded.
#include "netplay.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// Large and zero at start: in PSRAM on the FH1 (internal RAM is scarce), anywhere on a computer.
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define NP_BSS EXT_RAM_BSS_ATTR
#else
#define NP_BSS
#endif

#define VER 1
#define HDR 10
#define SILENT_MS 3000
#define ROSTER_MS 1000
#define PAD_MS 16
#define JOIN_RETRY_MS 300
#define FOUND_MS 5000
#define FOUND_MAX 8
#define PKT 1400

enum { T_FIND = 1, T_HERE, T_JOIN, T_WELCOME, T_REFUSE, T_PAD, T_STATE, T_ROSTER, T_BYE };
enum { R_NONE, R_HOST, R_GUEST };

typedef struct {
    bool used;
    struct sockaddr_in addr;
    int player;
    np_who_t who;
    uint32_t heard, pad_seq;
    fh1_pad_t pad;
} guest_t;

static int s_sock = -1;
static NP_BSS struct {
    int role, status;
    char app[NP_APP];
    uint32_t session;
    // The host: its own players, its guests.
    int first, seats, nlocal;
    np_who_t local[FH1_NET_PLAYERS];
    guest_t guest[FH1_NET_PLAYERS];
    uint32_t roster_at, state_seq;
    // A guest: the games it found, the one it joined, the players there, the newest state.
    fh1_net_game_t found[FOUND_MAX];
    uint32_t found_session[FOUND_MAX], found_at[FOUND_MAX];
    int nfound;
    struct sockaddr_in host;
    np_who_t me;
    int me_player;
    uint32_t heard, join_since, join_at, pad_at, pad_seq, state_seq_in;
    struct {
        bool used, remote;
        np_who_t who;
    } roster[FH1_NET_PLAYERS];
    uint8_t state[FH1_NET_STATE_MAX];
    size_t state_len;
    bool state_new;
} s;

static NP_BSS uint8_t s_out[PKT];        // the service's packets (the task's, under the lock)
static NP_BSS uint8_t s_state_out[HDR + 6 + FH1_NET_STATE_MAX];   // the host's states (the game's task)
static NP_BSS uint8_t s_in[PKT];         // a packet received (the task's)

// ── Bytes ──

static uint8_t *put8(uint8_t *p, uint32_t v) { *p = (uint8_t)v; return p + 1; }
static uint8_t *put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); return p + 2; }
static uint8_t *put32(uint8_t *p, uint32_t v) { put16(p, v); return put16(p + 2, v >> 16); }
static uint8_t *puts_(uint8_t *p, const char *t, size_t n)
{
    memset(p, 0, n);
    size_t k = 0;
    while (t && k < n - 1 && t[k]) k++;
    memcpy(p, t, k);
    return p + n;
}
static uint32_t get16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t get32(const uint8_t *p) { return get16(p) | get16(p + 2) << 16; }
static void gets_(char *out, const uint8_t *p, size_t n)
{
    memcpy(out, p, n);
    out[n - 1] = 0;
}

static uint8_t *head(uint8_t *p, int type)
{
    memcpy(p, "FH1P", 4);
    p[4] = VER;
    p[5] = (uint8_t)type;
    return put32(p + 6, s.session);
}

#define WHO_LEN (NP_NAME + 1 + NP_AVATAR + NP_NAME + 4 + 1)
static uint8_t *put_who(uint8_t *p, const np_who_t *w)
{
    p = puts_(p, w->name, NP_NAME);
    p = put8(p, w->avatar_len);
    memcpy(p, w->avatar, NP_AVATAR);
    p += NP_AVATAR;
    p = puts_(p, w->pad, NP_NAME);
    p = put32(p, w->rgb);
    return put8(p, w->motion);
}
static void get_who(np_who_t *w, const uint8_t *p)
{
    memset(w, 0, sizeof(*w));
    gets_(w->name, p, NP_NAME);
    p += NP_NAME;
    w->avatar_len = *p > NP_AVATAR ? NP_AVATAR : *p;
    memcpy(w->avatar, p + 1, NP_AVATAR);
    p += 1 + NP_AVATAR;
    gets_(w->pad, p, NP_NAME);
    w->rgb = get32(p + NP_NAME);
    w->motion = p[NP_NAME + 4] != 0;
}

#define PAD_LEN 36
static uint8_t *put_pad(uint8_t *p, uint32_t seq, const fh1_pad_t *d)
{
    p = put32(p, seq);
    p = put32(p, d->keys);
    p = put32(p, d->buttons);
    const int16_t v[12] = { d->lx, d->ly, d->rx, d->ry, (int16_t)d->l2, (int16_t)d->r2, d->gyro[0], d->gyro[1],
                            d->gyro[2], d->accel[0], d->accel[1], d->accel[2] };
    for (int i = 0; i < 12; i++) p = put16(p, (uint16_t)v[i]);
    return p;
}
static void get_pad(fh1_pad_t *d, const uint8_t *p, uint32_t now)
{
    memset(d, 0, sizeof(*d));
    d->keys = get32(p + 4);
    d->buttons = get32(p + 8);
    int16_t v[12];
    for (int i = 0; i < 12; i++) v[i] = (int16_t)get16(p + 12 + 2 * i);
    d->lx = v[0], d->ly = v[1], d->rx = v[2], d->ry = v[3];
    d->l2 = (uint16_t)v[4], d->r2 = (uint16_t)v[5];
    d->gyro[0] = v[6], d->gyro[1] = v[7], d->gyro[2] = v[8];
    d->accel[0] = v[9], d->accel[1] = v[10], d->accel[2] = v[11];
    d->time_ms = now;
}

static bool newer(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }   // a after b (wrapping)

static void send_to(const struct sockaddr_in *to, const uint8_t *p, size_t n)
{
    if (s_sock >= 0) sendto(s_sock, p, n, 0, (const struct sockaddr *)to, sizeof(*to));
}

static struct sockaddr_in addr_of(uint32_t ip)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(NP_PORT);
    a.sin_addr.s_addr = ip;
    return a;
}

static bool same(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static int open_sock(uint16_t port)
{
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    struct timeval tv = { 0, 8000 };                          // np_service waits this long for a packet
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in a = addr_of(htonl(INADDR_ANY));
    a.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool start(int role, uint16_t port)
{
    np_close();
    const int fd = open_sock(port);
    if (fd < 0) return false;
    np_port_lock();
    memset(&s, 0, sizeof(s));
    s_sock = fd;
    s.role = role;
    s.me_player = -1;
    np_port_unlock();
    if (!np_port_task_start()) {
        close(fd);
        s_sock = -1;
        s.role = R_NONE;
        return false;
    }
    return true;
}

// ── The host ──

static int guests(void)
{
    int n = 0;
    for (int i = 0; i < FH1_NET_PLAYERS; i++) n += s.guest[i].used;
    return n;
}

static guest_t *guest_at(const struct sockaddr_in *a)
{
    for (int i = 0; i < FH1_NET_PLAYERS; i++)
        if (s.guest[i].used && same(&s.guest[i].addr, a)) return &s.guest[i];
    return NULL;
}

static void send_roster(void)
{
    uint8_t *p = head(s_out, T_ROSTER), *count = p++;
    int n = 0;
    for (int pl = 0; pl < FH1_NET_PLAYERS; pl++) {
        const np_who_t *w = pl < s.nlocal ? &s.local[pl] : NULL;
        bool remote = false;
        for (int i = 0; i < FH1_NET_PLAYERS && !w; i++)
            if (s.guest[i].used && s.guest[i].player == pl) w = &s.guest[i].who, remote = true;
        if (!w) continue;
        p = put8(p, pl);
        p = put8(p, remote);
        p = put_who(p, w);
        n++;
    }
    *count = (uint8_t)n;
    for (int i = 0; i < FH1_NET_PLAYERS; i++)
        if (s.guest[i].used) send_to(&s.guest[i].addr, s_out, p - s_out);
}

static void host_packet(int type, const uint8_t *b, int n, const struct sockaddr_in *from, uint32_t now)
{
    guest_t *g = guest_at(from);
    switch (type) {
    case T_FIND:
        if (n < NP_APP || strncmp((const char *)b, s.app, NP_APP) != 0) return;
        {
            uint8_t *p = head(s_out, T_HERE);
            p = puts_(p, s.app, NP_APP);
            p = puts_(p, s.nlocal ? s.local[0].name : "FH1", NP_NAME);
            p = put8(p, s.nlocal + guests());
            p = put8(p, s.seats - guests());
            send_to(from, s_out, p - s_out);
        }
        return;
    case T_JOIN: {
        if (n < NP_APP + WHO_LEN) return;
        int reason = strncmp((const char *)b, s.app, NP_APP) != 0 ? 2 : 0;   // 2: another app's game
        if (reason) g = NULL;
        if (!g && !reason) {                                     // a seat: the first free one
            for (int k = 0; k < s.seats && !g; k++) {
                bool taken = false;
                for (int i = 0; i < FH1_NET_PLAYERS; i++) taken |= s.guest[i].used && s.guest[i].player == s.first + k;
                if (taken) continue;
                for (int i = 0; i < FH1_NET_PLAYERS && !g; i++)
                    if (!s.guest[i].used) {
                        g = &s.guest[i];
                        memset(g, 0, sizeof(*g));
                        g->used = true;
                        g->addr = *from;
                        g->player = s.first + k;
                    }
            }
            if (!g) reason = 1;                                  // full
        }
        if (g) {
            get_who(&g->who, b + NP_APP);
            g->heard = now;
            s.roster_at = 0;                                     // everyone hears of them at once
        }
        uint8_t *p = head(s_out, g ? T_WELCOME : T_REFUSE);
        p = put8(p, g ? g->player : reason);
        send_to(from, s_out, p - s_out);
        return;
    }
    case T_PAD:
        if (!g || n < PAD_LEN) return;
        g->heard = now;
        if (!newer(get32(b), g->pad_seq) && g->pad_seq) return;
        g->pad_seq = get32(b);
        get_pad(&g->pad, b, now);
        return;
    case T_BYE:
        if (g) {
            g->used = false;
            s.roster_at = 0;
        }
        return;
    }
}

static void host_tick(uint32_t now)
{
    for (int i = 0; i < FH1_NET_PLAYERS; i++)
        if (s.guest[i].used && now - s.guest[i].heard > SILENT_MS) {
            s.guest[i].used = false;
            s.roster_at = 0;
        }
    if (!s.roster_at || now - s.roster_at >= ROSTER_MS) {
        send_roster();
        s.roster_at = now ? now : 1;
    }
}

// ── A guest ──

static void send_join(void)
{
    uint8_t *p = head(s_out, T_JOIN);
    p = puts_(p, s.app, NP_APP);
    p = put_who(p, &s.me);
    send_to(&s.host, s_out, p - s_out);
}

static void guest_packet(int type, uint32_t session, const uint8_t *b, int n, const struct sockaddr_in *from, uint32_t now)
{
    if (type == T_HERE) {
        if (n < NP_APP + NP_NAME + 2 || strncmp((const char *)b, s.app, NP_APP) != 0) return;
        int k = 0;
        while (k < s.nfound && s.found[k].addr != from->sin_addr.s_addr) k++;
        if (k == s.nfound) {
            if (k == FOUND_MAX) return;
            s.nfound++;
        }
        gets_(s.found[k].name, b + NP_APP, NP_NAME);
        s.found[k].addr = from->sin_addr.s_addr;
        s.found[k].players = b[NP_APP + NP_NAME];
        s.found[k].seats = b[NP_APP + NP_NAME + 1];
        s.found_session[k] = session;
        s.found_at[k] = now;
        return;
    }
    if (!same(from, &s.host) || (s.status != FH1_NET_JOINING && s.status != FH1_NET_GUEST)) return;
    if (s.status == FH1_NET_JOINING && (type == T_WELCOME || type == T_REFUSE)) s.session = session;   // (joined by address)
    if (session != s.session) return;
    s.heard = now;
    switch (type) {
    case T_WELCOME:
        if (n >= 1 && s.status == FH1_NET_JOINING) {
            s.me_player = b[0];
            s.status = FH1_NET_GUEST;
        }
        return;
    case T_REFUSE:
        if (s.status == FH1_NET_JOINING) s.status = FH1_NET_LOST;
        return;
    case T_STATE: {
        if (n < 6) return;
        const uint32_t seq = get32(b);
        const size_t len = get16(b + 4);
        if (len > FH1_NET_STATE_MAX || (int)(6 + len) > n || (s.state_seq_in && !newer(seq, s.state_seq_in))) return;
        s.state_seq_in = seq;
        memcpy(s.state, b + 6, len);
        s.state_len = len;
        s.state_new = true;
        return;
    }
    case T_ROSTER: {
        if (n < 1) return;
        memset(s.roster, 0, sizeof(s.roster));
        const uint8_t *p = b + 1;
        for (int k = 0; k < b[0] && p + 2 + WHO_LEN <= b + n; k++, p += 2 + WHO_LEN) {
            if (p[0] >= FH1_NET_PLAYERS) continue;
            s.roster[p[0]].used = true;
            s.roster[p[0]].remote = p[1] != 0;
            get_who(&s.roster[p[0]].who, p + 2);
        }
        return;
    }
    case T_BYE:
        s.status = FH1_NET_LOST;
        return;
    }
}

static void guest_tick(uint32_t now)
{
    if (s.status == FH1_NET_JOINING) {
        if (now - s.join_since > SILENT_MS) s.status = FH1_NET_LOST;
        else if (now - s.join_at >= JOIN_RETRY_MS) {
            send_join();
            s.join_at = now;
        }
    } else if (s.status == FH1_NET_GUEST) {
        if (now - s.heard > SILENT_MS) {
            s.status = FH1_NET_LOST;
            return;
        }
        if (now - s.pad_at >= PAD_MS) {
            fh1_pad_t pad;
            if (!np_port_local_pad(&pad)) memset(&pad, 0, sizeof(pad));
            uint8_t *p = head(s_out, T_PAD);
            p = put_pad(p, ++s.pad_seq, &pad);
            send_to(&s.host, s_out, p - s_out);
            s.pad_at = now;
        }
    }
}

// ── The service ──

void np_service(int wait_ms)
{
    (void)wait_ms;                                               // (the socket's receive timeout)
    uint8_t *b = s_in;
    struct sockaddr_in from;
    socklen_t fl = sizeof(from);
    const int n = s_sock >= 0 ? (int)recvfrom(s_sock, b, PKT, 0, (struct sockaddr *)&from, &fl) : -1;
    np_port_lock();
    const uint32_t now = np_port_millis();
    if (n >= HDR && !memcmp(b, "FH1P", 4) && b[4] == VER) {
        if (s.role == R_HOST && (b[5] == T_FIND || b[5] == T_JOIN || get32(b + 6) == s.session))
            host_packet(b[5], b + HDR, n - HDR, &from, now);
        else if (s.role == R_GUEST)
            guest_packet(b[5], get32(b + 6), b + HDR, n - HDR, &from, now);
    }
    if (s.role == R_HOST) host_tick(now);
    else if (s.role == R_GUEST) guest_tick(now);
    np_port_unlock();
}

// ── The API ──

bool np_host(const char *app, int first_player, int seats, const np_who_t local[], int nlocal)
{
    if (first_player < 1 || seats < 1 || first_player + seats > FH1_NET_PLAYERS) return false;
    if (!start(R_HOST, NP_PORT)) return false;
    np_port_lock();
    snprintf(s.app, sizeof(s.app), "%s", app);
    s.session = np_port_random() | 1;
    s.first = first_player;
    s.seats = seats;
    s.nlocal = nlocal < first_player ? nlocal : first_player;
    memcpy(s.local, local, s.nlocal * sizeof(np_who_t));
    s.status = FH1_NET_HOSTING;
    np_port_unlock();
    return true;
}

bool np_find(const char *app, uint32_t also_addr)
{
    if (s.role == R_HOST) return false;
    if (s.role == R_NONE && !start(R_GUEST, 0)) return false;
    np_port_lock();
    snprintf(s.app, sizeof(s.app), "%s", app);
    uint8_t p0[HDR + NP_APP], *p = head(p0, T_FIND);           // (hosts answer it whatever its session)
    p = puts_(p, s.app, NP_APP);
    np_port_unlock();
    const struct sockaddr_in all = addr_of(htonl(INADDR_BROADCAST));
    send_to(&all, p0, p - p0);
    if (also_addr) {
        const struct sockaddr_in one = addr_of(also_addr);
        send_to(&one, p0, p - p0);
    }
    return true;
}

int np_games(fh1_net_game_t *out, int max)
{
    np_port_lock();
    const uint32_t now = np_port_millis();
    int k = 0;
    for (int i = 0; i < s.nfound && k < max; i++)
        if (now - s.found_at[i] < FOUND_MS) out[k++] = s.found[i];
    np_port_unlock();
    return k;
}

bool np_join(const char *app, const fh1_net_game_t *game, const np_who_t *me)
{
    if (s.role == R_HOST || !game) return false;
    if (s.role == R_NONE && !start(R_GUEST, 0)) return false;
    np_port_lock();
    snprintf(s.app, sizeof(s.app), "%s", app);
    s.host = addr_of(game->addr);
    s.session = 0;
    for (int i = 0; i < s.nfound; i++)
        if (s.found[i].addr == game->addr) s.session = s.found_session[i];
    s.me = *me;
    s.me_player = -1;
    s.status = FH1_NET_JOINING;
    s.join_since = np_port_millis();
    s.join_at = s.join_since;
    s.state_seq_in = 0;
    s.state_new = false;
    memset(s.roster, 0, sizeof(s.roster));
    send_join();
    np_port_unlock();
    return true;
}

int np_status(void) { return s.status; }
int np_me(void) { return s.status == FH1_NET_GUEST ? s.me_player : -1; }

bool np_remote(int player)
{
    bool r = false;
    np_port_lock();
    for (int i = 0; s.role == R_HOST && i < FH1_NET_PLAYERS; i++) r |= s.guest[i].used && s.guest[i].player == player;
    np_port_unlock();
    return r;
}

bool np_remote_pad(int player, fh1_pad_t *out)
{
    bool r = false;
    np_port_lock();
    for (int i = 0; s.role == R_HOST && i < FH1_NET_PLAYERS && !r; i++)
        if (s.guest[i].used && s.guest[i].player == player) *out = s.guest[i].pad, r = true;
    np_port_unlock();
    return r;
}

bool np_player(int player, np_who_t *out)
{
    if (player < 0 || player >= FH1_NET_PLAYERS) return false;
    bool r = false;
    np_port_lock();
    if (s.role == R_HOST) {
        if (player < s.nlocal) *out = s.local[player], r = true;
        for (int i = 0; i < FH1_NET_PLAYERS && !r; i++)
            if (s.guest[i].used && s.guest[i].player == player) *out = s.guest[i].who, r = true;
    } else if (s.role == R_GUEST && s.roster[player].used) {
        *out = s.roster[player].who;
        r = true;
    }
    np_port_unlock();
    return r;
}

bool np_state_send(const void *data, size_t len)
{
    if (s.role != R_HOST || len > FH1_NET_STATE_MAX) return false;
    struct sockaddr_in to[FH1_NET_PLAYERS];
    int n = 0;
    np_port_lock();
    uint8_t *p = head(s_state_out, T_STATE);
    p = put32(p, ++s.state_seq);
    p = put16(p, (uint32_t)len);
    memcpy(p, data, len);
    p += len;
    for (int i = 0; i < FH1_NET_PLAYERS; i++)
        if (s.guest[i].used) to[n++] = s.guest[i].addr;
    np_port_unlock();
    for (int i = 0; i < n; i++) send_to(&to[i], s_state_out, p - s_state_out);
    return true;
}

int np_state_recv(void *data, size_t max)
{
    int r = -1;
    np_port_lock();
    if (s.role == R_GUEST && s.state_new) {
        const size_t n = s.state_len < max ? s.state_len : max;
        memcpy(data, s.state, n);
        s.state_new = false;
        r = (int)n;
    }
    np_port_unlock();
    return r;
}

void np_close(void)
{
    if (s.role == R_NONE) return;
    np_port_lock();
    uint8_t *p = head(s_out, T_BYE);
    if (s.role == R_HOST)
        for (int i = 0; i < FH1_NET_PLAYERS; i++) {
            if (s.guest[i].used) send_to(&s.guest[i].addr, s_out, p - s_out);
        }
    else if (s.status == FH1_NET_GUEST || s.status == FH1_NET_JOINING)
        send_to(&s.host, s_out, p - s_out);
    np_port_unlock();
    np_port_task_stop();
    np_port_lock();
    if (s_sock >= 0) close(s_sock);
    s_sock = -1;
    memset(&s, 0, sizeof(s));
    s.me_player = -1;
    np_port_unlock();
}
