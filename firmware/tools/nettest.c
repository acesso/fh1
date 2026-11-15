// Network play on a computer (components/fh1_apps/netplay.c, the FH1's own code): a host or a guest from the command
// line, to check the protocol without a second FH1, or to play the other side against one.
//
//   cc -O2 -Wall -I components/fh1_apps -I components/fh1_sdk/include -o nettest
//      tools/nettest.c components/fh1_apps/netplay.c -lpthread
//   ./nettest host APP [NAME] [SECONDS]        host APP's game: one seat (player 2); sends a state 30 times a second
//                                              (its number and the guest's buttons), prints who joins and leaves
//   ./nettest find APP [ADDR]                  APP's games on the network (and at ADDR)
//   ./nettest join APP ADDR [NAME] [SECONDS]   join the game at ADDR; presses A every half second, prints the states
//
// A host prints "guest player N: NAME", the guest "joined as player N", "state ..." and "roster ..." lines: the
// lines the end-to-end check looks for.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include "netplay.h"

static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t s_th;
static volatile bool s_run;
static uint32_t s_t0;

uint32_t np_port_millis(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000) - s_t0;
}
uint32_t np_port_random(void) { return (uint32_t)rand() << 16 ^ (uint32_t)rand(); }
void np_port_lock(void) { pthread_mutex_lock(&s_mu); }
void np_port_unlock(void) { pthread_mutex_unlock(&s_mu); }

static void *loop(void *arg)
{
    (void)arg;
    while (s_run) np_service(8);
    return NULL;
}
bool np_port_task_start(void)
{
    s_run = true;
    return pthread_create(&s_th, NULL, loop, NULL) == 0;
}
void np_port_task_stop(void)
{
    if (!s_run) return;
    s_run = false;
    pthread_join(s_th, NULL);
}

// The guest's "controller": A pressed every other half second.
bool np_port_local_pad(fh1_pad_t *out)
{
    memset(out, 0, sizeof(*out));
    if ((np_port_millis() / 500) % 2) out->buttons = 1u << 0;   // FH1_PAD_BTN_A
    out->accel[1] = 981;
    return true;
}

static uint32_t resolve(const char *host)
{
    struct addrinfo hints = { .ai_family = AF_INET }, *r = NULL;
    if (getaddrinfo(host, NULL, &hints, &r) != 0 || !r) return 0;
    const uint32_t a = ((struct sockaddr_in *)r->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(r);
    return a;
}

static void sleep_ms(int ms) { usleep(ms * 1000); }

static void print_roster(void)
{
    char line[160] = "roster";
    for (int p = 0; p < FH1_NET_PLAYERS; p++) {
        np_who_t w;
        if (np_player(p, &w)) snprintf(line + strlen(line), sizeof(line) - strlen(line), " %d:%s", p + 1, w.name);
    }
    printf("%s\n", line);
}

static np_who_t who(const char *name)
{
    np_who_t w;
    memset(&w, 0, sizeof(w));
    snprintf(w.name, sizeof(w.name), "%s", name);
    snprintf(w.pad, sizeof(w.pad), "PC");
    w.rgb = 0x2a6cf0;
    return w;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    s_t0 = 0;
    s_t0 = np_port_millis();
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    if (argc < 3) {
        fprintf(stderr, "nettest host APP [NAME] [S] | find APP [ADDR] | join APP ADDR [NAME] [S]\n");
        return 2;
    }
    const char *cmd = argv[1], *app = argv[2];
    if (!strcmp(cmd, "host")) {
        const np_who_t me = who(argc > 3 ? argv[3] : "PC");
        const int secs = argc > 4 ? atoi(argv[4]) : 30;
        if (!np_host(app, 1, 1, &me, 1)) {
            fprintf(stderr, "cannot host (port %d taken?)\n", NP_PORT);
            return 1;
        }
        printf("hosting %s on port %d\n", app, NP_PORT);
        bool had = false;
        uint32_t last_buttons = ~0u;
        for (int k = 0; k < secs * 30; k++) {
            const bool has = np_remote(1);
            np_who_t g;
            if (has != had) {
                if (has && np_player(1, &g)) printf("guest player 2: %s (%s)\n", g.name, g.pad);
                else if (!has) printf("guest left\n");
                had = has;
            }
            fh1_pad_t pad;
            if (np_remote_pad(1, &pad) && pad.buttons != last_buttons) {
                printf("guest buttons %#x\n", (unsigned)pad.buttons);
                last_buttons = pad.buttons;
            }
            char st[64];
            const int n = snprintf(st, sizeof(st), "tick %d buttons %#x", k, has ? (unsigned)pad.buttons : 0);
            np_state_send(st, n + 1);
            sleep_ms(33);
        }
        np_close();
        return 0;
    }
    if (!strcmp(cmd, "find")) {
        if (!np_find(app, argc > 3 ? resolve(argv[3]) : 0)) return 1;
        sleep_ms(1200);
        fh1_net_game_t g[8];
        const int n = np_games(g, 8);
        for (int i = 0; i < n; i++) {
            struct in_addr a = { g[i].addr };
            printf("game at %s: %s, %d player(s), %d seat(s) free\n", inet_ntoa(a), g[i].name, g[i].players, g[i].seats);
        }
        if (!n) printf("no games\n");
        np_close();
        return n ? 0 : 1;
    }
    if (!strcmp(cmd, "join") && argc > 3) {
        fh1_net_game_t g;
        memset(&g, 0, sizeof(g));
        g.addr = resolve(argv[3]);
        if (!g.addr) return 1;
        const np_who_t me = who(argc > 4 ? argv[4] : "PC guest");
        const int secs = argc > 5 ? atoi(argv[5]) : 10;
        np_find(app, g.addr);                                    // its session, if it answers
        sleep_ms(300);
        if (!np_join(app, &g, &me)) return 1;
        int last = -1, states = 0;
        char st[FH1_NET_STATE_MAX];
        for (int k = 0; k < secs * 100; k++) {
            const int s = np_status();
            if (s != last) {
                printf(s == FH1_NET_GUEST ? "joined as player %d\n" : s == FH1_NET_LOST ? "lost\n" : "joining\n", np_me() + 1);
                last = s;
                if (s == FH1_NET_LOST) break;
            }
            const int n = np_state_recv(st, sizeof(st) - 1);
            if (n > 0) {
                st[n] = 0;
                if (states++ % 30 == 0) {
                    printf("state %s\n", st);
                    print_roster();
                }
            }
            sleep_ms(10);
        }
        printf("%d states\n", states);
        const bool ok = np_status() == FH1_NET_GUEST && states > 0;
        np_close();
        return ok ? 0 : 1;
    }
    fprintf(stderr, "unknown command\n");
    return 2;
}
