// Network play (fh1_sdk.h, SDK 0.9): the session between units, over UDP on the local network. Plain C on BSD
// sockets, so the same code runs on the FH1 (lwIP) and on a computer (tools/nettest.c); what it needs of its
// platform is below (np_port_*), and run.c turns it into the SDK's fh1_net_* functions.
//
// One port, NP_PORT, on the host. A guest looking for games sends FIND (its app) to the network's broadcast
// address (and to any address it is given); hosts of that app answer HERE. JOIN asks for a seat: WELCOME gives
// the player number, REFUSE why not (1: full, 2: another app). Then the guest sends its controls (PAD) about 60
// times a second, the host its game's state (STATE) when the game sends one, and every player's name and avatar
// (ROSTER) each second. BYE ends it; 3 s of silence too. Nothing is resent: the newest PAD and STATE win.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fh1_sdk.h"

#define NP_PORT 47720
#define NP_NAME 24
#define NP_AVATAR 24
#define NP_APP 32

typedef struct {
    char name[NP_NAME];                  // the user's (or the unit's) name
    uint8_t avatar[NP_AVATAR];           // their avatar's code (fh1_avatar.h); none: drawn from the name
    uint8_t avatar_len;
    char pad[NP_NAME];                   // their controller: its name, colour, motion
    uint32_t rgb;
    bool motion;
} np_who_t;

bool np_host(const char *app, int first_player, int seats, const np_who_t local[], int nlocal);
bool np_find(const char *app, uint32_t also_addr);   // also_addr: one more address to ask (network order), or 0
int np_games(fh1_net_game_t *out, int max);
bool np_join(const char *app, const fh1_net_game_t *game, const np_who_t *me);
int np_status(void);
int np_me(void);
bool np_remote(int player);
bool np_remote_pad(int player, fh1_pad_t *out);
bool np_player(int player, np_who_t *out);
bool np_state_send(const void *data, size_t len);
int np_state_recv(void *data, size_t max);
void np_close(void);

// The service: received packets handled, controls sent, the silent dropped. The port's task calls it in a loop
// while np_port_task_start's task runs; it waits for packets up to wait_ms.
void np_service(int wait_ms);

// ── What the platform gives ──
uint32_t np_port_millis(void);
uint32_t np_port_random(void);
void np_port_lock(void);
void np_port_unlock(void);
bool np_port_task_start(void);          // a task calling np_service until np_port_task_stop
void np_port_task_stop(void);           // returns once it has stopped
bool np_port_local_pad(fh1_pad_t *out); // a guest's player 1: its controller, or the unit's keys as one
