// Network play for apps (fh1_sdk.h, SDK 0.9): netplay.c's platform on the FH1, and the SDK's fh1_net_* and
// fh1_user_look. Each player travels as a name, an avatar code and their controller's name, colour and motion
// (np_who_t): a user who has not made their avatar sends the one the system draws for them, so both units show
// the same; a player without a user sends none.
#include <string.h>
#include <stdio.h>
#include "apps_priv.h"
#include "netplay.h"
#include "fh1_avatar.h"
#include "fh1_users.h"
#include "fh1_gamepad.h"
#include "fh1_input.h"
#include "fh1_devconsole.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_log.h"

static const char *TAG = "play";

#define STACK 4096                         // internal RAM: the task reads controllers, which may load settings

static SemaphoreHandle_t s_mu;
static StaticSemaphore_t s_mu_buf;
static portMUX_TYPE s_init = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_done;
static StaticSemaphore_t s_done_buf;
static volatile bool s_run;

// ── The platform ──

uint32_t np_port_millis(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
uint32_t np_port_random(void) { return esp_random(); }

void np_port_lock(void)
{
    if (!s_mu) {
        taskENTER_CRITICAL(&s_init);
        if (!s_mu) {
            s_mu = xSemaphoreCreateMutexStatic(&s_mu_buf);
            s_done = xSemaphoreCreateBinaryStatic(&s_done_buf);
        }
        taskEXIT_CRITICAL(&s_init);
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
}

void np_port_unlock(void) { xSemaphoreGive(s_mu); }

static void play_task(void *arg)
{
    while (s_run) np_service(8);
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

bool np_port_task_start(void)
{
    np_port_lock();                                              // (makes the semaphores)
    np_port_unlock();
    xSemaphoreTake(s_done, 0);
    s_run = true;
    if (xTaskCreatePinnedToCore(play_task, "netplay", STACK, NULL, 4, NULL, 0) != pdPASS) {
        s_run = false;
        ESP_LOGE(TAG, "no memory for the network play task");
        return false;
    }
    return true;
}

void np_port_task_stop(void)
{
    if (!s_run) return;
    s_run = false;
    xSemaphoreTake(s_done, pdMS_TO_TICKS(1000));
}

// A guest's player 1: their controller, or the unit's keys as a controller's buttons by position (as the web
// console's pad: A the right face button, B the bottom one).
bool np_port_local_pad(fh1_pad_t *out)
{
    fh1_gamepad_info_t g;
    fh1_pad_state_t st;
    if (fh1_gamepad_of_player(0, &g) && fh1_gamepad_state(g.id, &st)) {
        memcpy(out, &st, sizeof(*out));
        return true;
    }
    static const uint32_t btn[FH1_KEY_COUNT] = {
        [FH1_KEY_UP] = FH1_PAD_BTN_UP, [FH1_KEY_DOWN] = FH1_PAD_BTN_DOWN, [FH1_KEY_LEFT] = FH1_PAD_BTN_LEFT,
        [FH1_KEY_RIGHT] = FH1_PAD_BTN_RIGHT, [FH1_KEY_A] = FH1_PAD_BTN_B, [FH1_KEY_B] = FH1_PAD_BTN_A,
        [FH1_KEY_X] = FH1_PAD_BTN_Y, [FH1_KEY_Y] = FH1_PAD_BTN_X, [FH1_KEY_START] = FH1_PAD_BTN_START,
        [FH1_KEY_SELECT] = FH1_PAD_BTN_SELECT, [FH1_KEY_MENU] = FH1_PAD_BTN_SYSTEM,
    };
    memset(out, 0, sizeof(*out));
    for (int k = 0; k < FH1_KEY_COUNT; k++)
        if (fh1_input_is_down(k)) out->keys |= 1u << k, out->buttons |= btn[k];
    out->accel[1] = 981;                                         // lying still, face up
    out->time_ms = np_port_millis();
    return true;
}

// ── Players ──

static void avatar_of_user(const fh1_user_t *u, fh1_avatar_t *a)
{
    if (!(u->avatar_len && fh1_avatar_decode(u->avatar, u->avatar_len, a))) fh1_avatar_from_seed(u->id, strlen(u->id), a);
}

// A local player as sent: their user (or the unit), their controller (or the unit's keys).
static void who_local(int player, np_who_t *w)
{
    memset(w, 0, sizeof(*w));
    fh1_user_t u;
    if (fh1_users_player(player, &u)) {
        fh1_avatar_t a;
        avatar_of_user(&u, &a);
        snprintf(w->name, sizeof(w->name), "%s", u.name);
        w->avatar_len = (uint8_t)fh1_avatar_encode(&a, w->avatar, sizeof(w->avatar));
    } else {
        snprintf(w->name, sizeof(w->name), "FH1");
    }
    fh1_gamepad_info_t g;
    if (fh1_gamepad_of_player(player, &g)) {
        snprintf(w->pad, sizeof(w->pad), "%s", g.name);
        w->rgb = g.rgb;
        w->motion = (g.caps & FH1_PAD_CAP_MOTION) != 0;
    } else {
        snprintf(w->pad, sizeof(w->pad), "FH1");
    }
}

static void look_of_avatar(const fh1_avatar_t *a, fh1_look_t *l)
{
    memset(l, 0, sizeof(*l));
    l->body = a->f[FH1_AV_BASE];
    l->hair_style = a->f[FH1_AV_HAIR_STYLE];
    l->top = a->f[FH1_AV_TOP];
    l->bottom = a->f[FH1_AV_BOTTOM];
    l->acc = a->acc;
    l->skin = fh1_avatar_choice_colour(FH1_AV_SKIN, a->f[FH1_AV_SKIN]);
    l->hair = fh1_avatar_choice_colour(FH1_AV_HAIR, a->f[FH1_AV_HAIR]);
    l->eyes = fh1_avatar_choice_colour(FH1_AV_EYES, a->f[FH1_AV_EYES]);
    l->top_colour = fh1_avatar_choice_colour(FH1_AV_TOP_COLOUR, a->f[FH1_AV_TOP_COLOUR]);
    l->bottom_colour = fh1_avatar_choice_colour(FH1_AV_BOTTOM_COLOUR, a->f[FH1_AV_BOTTOM_COLOUR]);
    l->acc_colour = fh1_avatar_choice_colour(FH1_AV_ACC_COLOUR, a->f[FH1_AV_ACC_COLOUR]);
}

static bool look_of_who(const np_who_t *w, fh1_look_t *l)       // false: they play without a user
{
    fh1_avatar_t a;
    if (!w->avatar_len || !fh1_avatar_decode(w->avatar, w->avatar_len, &a)) return false;
    look_of_avatar(&a, l);
    return true;
}

bool play_remote_who(int player, np_who_t *w) { return np_remote(player) && np_player(player, w); }

// ── The SDK ──

bool fh1_user_look(int player, fh1_look_t *out)
{
    np_who_t w;
    if (play_remote_who(player, &w)) return look_of_who(&w, out);
    fh1_user_t u;
    if (!fh1_users_player(player, &u)) return false;
    fh1_avatar_t a;
    avatar_of_user(&u, &a);
    look_of_avatar(&a, out);
    return true;
}

bool fh1_net_host(int first_player, int seats)
{
    if (!fh1_wifi_connected()) return false;
    np_who_t local[FH1_NET_PLAYERS];
    const int n = first_player < FH1_NET_PLAYERS ? first_player : FH1_NET_PLAYERS;
    for (int p = 0; p < n; p++) who_local(p, &local[p]);
    return np_host(fh1_app_id(), first_player, seats, local, n);
}

bool fh1_net_find(void) { return fh1_wifi_connected() && np_find(fh1_app_id(), 0); }
int fh1_net_games(fh1_net_game_t *out, int max) { return np_games(out, max); }

bool fh1_net_join(const fh1_net_game_t *game)
{
    if (!fh1_wifi_connected()) return false;
    np_who_t me;
    who_local(0, &me);
    return np_join(fh1_app_id(), game, &me);
}

int fh1_net_status(void) { return np_status(); }
int fh1_net_me(void) { return np_me(); }
bool fh1_net_remote(int player) { return np_remote(player); }

bool fh1_net_player(int player, char *name, size_t n)
{
    np_who_t w;
    if (!np_player(player, &w)) return false;
    if (name && n) snprintf(name, n, "%s", w.name);
    return true;
}

bool fh1_net_look(int player, fh1_look_t *look)
{
    np_who_t w;
    return np_player(player, &w) && look_of_who(&w, look);
}

bool fh1_net_state_send(const void *data, size_t len) { return np_state_send(data, len); }
int fh1_net_state_recv(void *data, size_t max) { return np_state_recv(data, max); }
void fh1_net_close(void) { np_close(); }
