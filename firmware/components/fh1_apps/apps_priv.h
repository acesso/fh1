#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#define FH1_USES_MAX 4
typedef struct {
    char id[48];
    char name[32];
    char version[16];
    uint32_t needs, wants;
    bool assets;                       // an asset package ("kind": "assets"): no code, installed once for every app
    bool on_sd;                        // an asset package's "where": "sd" (else internal)
    uint8_t nuses;
    char uses[FH1_USES_MAX][48];       // an app's asset packages ("uses")
} fh1_manifest_t;
void sfx_shutdown(void);                   // sfx.c: every sound stopped, the mixer ended (the app is going)

// pkg.c
esp_err_t pkg_manifest_parse(const char *json, fh1_manifest_t *m, char *err, size_t n);
esp_err_t pkg_peek(const char *path, fh1_manifest_t *m, char *err, size_t n);
esp_err_t pkg_install(const char *path, fh1_manifest_t *m, char *err, size_t n);
esp_err_t pkg_uninstall(const char *id);

// run.c (UI task only)
void run_init(void);
esp_err_t run_start(const fh1_manifest_t *m, char *err, size_t n);
void run_stop(void);
const char *run_current(void);       // id of the running packaged app, or NULL

// apps_games.c: the game library apps, one per console (UI task only)
#include "fh1_ui.h"
extern const fh1_ui_span_t s_gbc_name[6];     // "GameBoy Color", its menu entry and window title
void games_open_gameboy(void);
void games_open_nes(void);
void games_open_sms(void);

// apps_music.c: the music player: the library window and the full-screen deck (UI task only)
void music_open(void);
void music_stop_for_game(void);      // a game takes the sound: the music stops
void music_init(void);

// apps_bridges.c: System > Bridges, the extension boards and their devices (UI task only)
void bridges_open(void);

// apps_io.c: System > Inputs and outputs, which device each role uses for every app (UI task only)
void io_open(void);
void pad_open(const char *id);          // a controller's window (apps_io.c), by its gamepad id
void pair_all_cb(void *u);              // every bridge waits for a new controller (apps_io.c)

// apps_users.c: the Users menu, the people on this FH1 (UI task only)
void users_ui_init(void (*menu_changed)(void));          // menu_changed: refill the Users menu
void users_menu_fill(fh1_win_t *win);                      // the Users menu, in the launcher
void users_pick_for_pad(const char *pad_id);                        // whose a controller is
void users_pad_owner_text(const char *pad_id, char *out, size_t n);   // "ana#3FQ1", or ""

// apps_users.c, for the network and the launcher
void users_notify_contact(const char *id, const char *text);         // a notification with their face
void users_announce_players(void);                                   // who plays, as an app starts
lv_obj_t *users_face_widget(lv_obj_t *parent, const char *id, int size);   // an animated face (16 or 32)
void users_notify_player(int player, const char *text);              // a notification with a player's face
bool run_dev_push(const char *text);                                // SDK 0.6: a console line for the app
void io_claim_init(void);                                           // Menu held 3 s on a controller: player 1
void users_net_changed(void);                                        // contacts or statements changed
void users_trust_arrived(const char *local, const char *from, uint8_t level);

// apps_net.c: users between units (U4, U5)
void net_start(void);
void net_kick(void);                    // look for other units and deliver the outbox now

// stress.c (dev tool)
#include <stdbool.h>
esp_err_t stress_start(int seconds, bool psram, bool flash, bool ui);
