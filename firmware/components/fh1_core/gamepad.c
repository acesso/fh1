// Gamepads and players (fh1_gamepad.h). The table is small and changes from the sources' tasks (the
// Bluetooth host) and the UI task, so it sits under a spinlock; key events and settings are done outside it.
#include <string.h>
#include "fh1_gamepad.h"
#include "fh1_input.h"
#include "fh1_settings.h"
#include "freertos/FreeRTOS.h"

typedef struct {
    bool used;
    fh1_gamepad_info_t info;
    uint32_t keys;
    fh1_pad_state_t state;
    uint32_t order;                    // connection order: automatic players take the earliest
    const fh1_gamepad_ops_t *ops;
    void *ctx;
    int16_t leds;                      // player LEDs as last set, -1 not yet
} pad_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static pad_t s_pad[FH1_GAMEPADS_MAX];
static uint32_t s_next_order;
static char s_choice[FH1_PLAYERS][18];
static fh1_gamepad_info_t s_seen[FH1_PLAYERS];   // each player's chosen gamepad as last seen (for when it is away)
static bool s_loaded;
static uint32_t s_p1_posted;           // player 1's gamepad keys as last posted
static bool s_mute;                    // player 1's gamepad keys not posted (an input test)
static int s_motion;                   // who wants motion streamed

static const char *const s_keys[FH1_PLAYERS] = { "p1", "p2" };

static const char *const s_name_keys[FH1_PLAYERS] = { "p1n", "p2n" };
static const char *const s_kind_keys[FH1_PLAYERS] = { "p1k", "p2k" };
static const char *const s_rgb_keys[FH1_PLAYERS] = { "p1c", "p2c" };

static void load(void)
{
    if (s_loaded) return;
    for (int p = 0; p < FH1_PLAYERS; p++) {
        fh1_settings_get_str("input", s_keys[p], s_choice[p], sizeof(s_choice[p]));
        fh1_gamepad_info_t *g = &s_seen[p];
        memcpy(g->id, s_choice[p], sizeof(g->id));
        fh1_settings_get_str("input", s_name_keys[p], g->name, sizeof(g->name));
        g->kind = fh1_settings_get_u8("input", s_kind_keys[p], 0);
        g->rgb = fh1_settings_get_u32("input", s_rgb_keys[p], 0);
    }
    s_loaded = true;
}

// Saves how a player's chosen gamepad looks (outside the lock).
static void remember(int p, const fh1_gamepad_info_t *g)
{
    fh1_settings_set_str("input", s_name_keys[p], g->name);
    fh1_settings_set_u8("input", s_kind_keys[p], g->kind);
    fh1_settings_set_u32("input", s_rgb_keys[p], g->rgb);
}

static bool is_pad_id(const char *c) { return c[0] && strcmp(c, "fh1") != 0; }

// The gamepad a player uses now (under the lock), or -1.
static int resolve(int player)
{
    if (player < 0 || player >= FH1_PLAYERS) return -1;
    const char *c = s_choice[player];
    if (strcmp(c, "fh1") == 0) return -1;
    if (c[0]) {
        for (int i = 0; i < FH1_GAMEPADS_MAX; i++) {
            if (s_pad[i].used && strcmp(s_pad[i].info.id, c) == 0) return i;
        }
        return -1;
    }
    // Automatic: in player order, each automatic player takes the earliest gamepad nobody chose by name and
    // no earlier automatic player took.
    bool taken[FH1_GAMEPADS_MAX] = { 0 };
    for (int p = 0; p < FH1_PLAYERS; p++) {
        if (!is_pad_id(s_choice[p])) continue;
        for (int i = 0; i < FH1_GAMEPADS_MAX; i++) {
            if (s_pad[i].used && strcmp(s_pad[i].info.id, s_choice[p]) == 0) taken[i] = true;
        }
    }
    for (int p = 0; p <= player; p++) {
        if (s_choice[p][0]) continue;            // not automatic
        int best = -1;
        for (int i = 0; i < FH1_GAMEPADS_MAX; i++) {
            if (s_pad[i].used && !taken[i] && !s_pad[i].info.manual && (best < 0 || s_pad[i].order < s_pad[best].order)) best = i;
        }
        if (best < 0) return -1;
        taken[best] = true;
        if (p == player) return best;
    }
    return -1;
}

// Player 1's gamepad drives the interface: post what changed in its keys.
static void sync_player1(void)
{
    portENTER_CRITICAL(&s_mux);
    int h = resolve(0);
    uint32_t keys = h >= 0 && !s_mute ? s_pad[h].keys : 0;
    uint32_t changed = keys ^ s_p1_posted;
    s_p1_posted = keys;
    portEXIT_CRITICAL(&s_mux);
    for (int k = 0; k < FH1_KEY_COUNT; k++) {
        if (changed & (1u << k)) fh1_input_post((fh1_key_t)k, (keys >> k) & 1);
    }
}

// Every gamepad shows its player's number on its LEDs (none: off). Called whenever who plays with what may
// have changed; the sources are told outside the lock, only what changed.
static void sync_leds(void)
{
    struct { const fh1_gamepad_ops_t *ops; void *ctx; uint8_t mask; } todo[FH1_GAMEPADS_MAX];
    int n = 0;
    portENTER_CRITICAL(&s_mux);
    int player_of[FH1_GAMEPADS_MAX];
    for (int i = 0; i < FH1_GAMEPADS_MAX; i++) player_of[i] = -1;
    for (int p = 0; p < FH1_PLAYERS; p++) {
        int h = resolve(p);
        if (h >= 0) player_of[h] = p;
    }
    for (int i = 0; i < FH1_GAMEPADS_MAX; i++) {
        pad_t *d = &s_pad[i];
        if (!d->used || !d->ops || !d->ops->leds) continue;
        const uint8_t mask = player_of[i] >= 0 ? (uint8_t)(1u << player_of[i]) : 0;
        if (d->leds == mask) continue;
        d->leds = mask;
        todo[n].ops = d->ops;
        todo[n].ctx = d->ctx;
        todo[n].mask = mask;
        n++;
    }
    portEXIT_CRITICAL(&s_mux);
    for (int i = 0; i < n; i++) todo[i].ops->leds(todo[i].ctx, todo[i].mask);
}

int fh1_gamepad_add(const fh1_gamepad_info_t *info)
{
    load();
    int h = -1;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < FH1_GAMEPADS_MAX && h < 0; i++) {
        if (s_pad[i].used && strcmp(s_pad[i].info.id, info->id) == 0) h = i;
    }
    for (int i = 0; i < FH1_GAMEPADS_MAX && h < 0; i++) {
        if (!s_pad[i].used) {
            h = i;
            s_pad[i].used = true;
            s_pad[i].keys = 0;
            memset(&s_pad[i].state, 0, sizeof(s_pad[i].state));
            s_pad[i].ops = NULL;
            s_pad[i].ctx = NULL;
            s_pad[i].leds = -1;
            s_pad[i].order = s_next_order++;
        }
    }
    if (h >= 0) s_pad[h].info = *info;
    // A chosen gamepad is back: keep its look if it changed (its name, its colour).
    bool changed[FH1_PLAYERS] = { 0 };
    for (int p = 0; h >= 0 && p < FH1_PLAYERS; p++) {
        fh1_gamepad_info_t *g = &s_seen[p];
        if (strcmp(s_choice[p], info->id) != 0) continue;
        changed[p] = strcmp(g->name, info->name) != 0 || g->kind != info->kind || g->rgb != info->rgb;
        *g = *info;
    }
    portEXIT_CRITICAL(&s_mux);
    for (int p = 0; p < FH1_PLAYERS; p++) {
        if (changed[p]) remember(p, info);
    }
    sync_player1();
    sync_leds();
    return h;
}

void fh1_gamepad_remove(int h)
{
    if (h < 0 || h >= FH1_GAMEPADS_MAX) return;
    portENTER_CRITICAL(&s_mux);
    s_pad[h].used = false;
    s_pad[h].keys = 0;
    s_pad[h].ops = NULL;
    portEXIT_CRITICAL(&s_mux);
    sync_player1();
    sync_leds();                       // automatic players may have moved to other gamepads
}

void fh1_gamepad_set_battery(int h, uint8_t battery)
{
    if (h < 0 || h >= FH1_GAMEPADS_MAX) return;
    portENTER_CRITICAL(&s_mux);
    s_pad[h].info.battery = battery;
    portEXIT_CRITICAL(&s_mux);
}

void fh1_gamepad_set_colour(int h, uint32_t rgb)
{
    if (h < 0 || h >= FH1_GAMEPADS_MAX) return;
    portENTER_CRITICAL(&s_mux);
    s_pad[h].info.rgb = rgb;
    portEXIT_CRITICAL(&s_mux);
}

void fh1_gamepad_set_keys(int h, uint32_t keys)
{
    if (h < 0 || h >= FH1_GAMEPADS_MAX) return;
    portENTER_CRITICAL(&s_mux);
    bool changed = s_pad[h].used && s_pad[h].keys != keys;
    s_pad[h].keys = keys;
    portEXIT_CRITICAL(&s_mux);
    if (changed) sync_player1();
}

void fh1_gamepad_set_state(int h, const fh1_pad_state_t *st)
{
    if (h < 0 || h >= FH1_GAMEPADS_MAX) return;
    portENTER_CRITICAL(&s_mux);
    const bool changed = s_pad[h].used && s_pad[h].keys != st->keys;
    if (s_pad[h].used) {
        s_pad[h].state = *st;
        s_pad[h].keys = st->keys;
    }
    portEXIT_CRITICAL(&s_mux);
    if (changed) sync_player1();
}

void fh1_gamepad_set_ops(int h, const fh1_gamepad_ops_t *ops, void *ctx)
{
    if (h < 0 || h >= FH1_GAMEPADS_MAX) return;
    portENTER_CRITICAL(&s_mux);
    s_pad[h].ops = ops;
    s_pad[h].ctx = ctx;
    s_pad[h].leds = -1;
    const bool motion = s_motion > 0;
    portEXIT_CRITICAL(&s_mux);
    if (ops && ops->motion && motion) ops->motion(ctx, true);
    sync_leds();
}

// The outputs of the gamepad with this id (under the lock: copied out).
static bool ops_of(const char *id, const fh1_gamepad_ops_t **ops, void **ctx)
{
    bool ok = false;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < FH1_GAMEPADS_MAX && !ok; i++) {
        if (s_pad[i].used && s_pad[i].ops && strcmp(s_pad[i].info.id, id) == 0) {
            *ops = s_pad[i].ops;
            *ctx = s_pad[i].ctx;
            ok = true;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

bool fh1_gamepad_state(const char *id, fh1_pad_state_t *out)
{
    bool ok = false;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < FH1_GAMEPADS_MAX && !ok; i++) {
        if (s_pad[i].used && strcmp(s_pad[i].info.id, id) == 0) {
            *out = s_pad[i].state;
            ok = true;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

bool fh1_gamepad_rumble(const char *id, uint8_t weak, uint8_t strong, uint16_t ms)
{
    const fh1_gamepad_ops_t *o;
    void *c;
    if (!ops_of(id, &o, &c) || !o->rumble) return false;
    o->rumble(c, weak, strong, ms);
    return true;
}

bool fh1_gamepad_light(const char *id, uint32_t rgb)
{
    const fh1_gamepad_ops_t *o;
    void *c;
    if (!ops_of(id, &o, &c) || !o->light) return false;
    o->light(c, rgb);
    return true;
}

bool fh1_gamepad_trigger(const char *id, int side, uint8_t effect, uint8_t a, uint8_t b, uint8_t cc)
{
    const fh1_gamepad_ops_t *o;
    void *c;
    if (!ops_of(id, &o, &c) || !o->trigger) return false;
    o->trigger(c, side, effect, a, b, cc);
    return true;
}

void fh1_gamepad_want_motion(bool on)
{
    struct { const fh1_gamepad_ops_t *ops; void *ctx; } todo[FH1_GAMEPADS_MAX];
    int n = 0;
    portENTER_CRITICAL(&s_mux);
    const bool was = s_motion > 0;
    s_motion += on ? 1 : -1;
    if (s_motion < 0) s_motion = 0;
    const bool now = s_motion > 0;
    for (int i = 0; was != now && i < FH1_GAMEPADS_MAX; i++) {
        if (s_pad[i].used && s_pad[i].ops && s_pad[i].ops->motion) {
            todo[n].ops = s_pad[i].ops;
            todo[n].ctx = s_pad[i].ctx;
            n++;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    for (int i = 0; i < n; i++) todo[i].ops->motion(todo[i].ctx, now);
}

void fh1_gamepad_mute_keys(bool mute)
{
    s_mute = mute;
    sync_player1();                    // releases what was held
}

int fh1_gamepad_count(void)
{
    int n = 0;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < FH1_GAMEPADS_MAX; i++) n += s_pad[i].used;
    portEXIT_CRITICAL(&s_mux);
    return n;
}

bool fh1_gamepad_at(int idx, fh1_gamepad_info_t *out, int *player)
{
    load();
    bool ok = false;
    portENTER_CRITICAL(&s_mux);
    // In connection order.
    int order[FH1_GAMEPADS_MAX], n = 0;
    for (int i = 0; i < FH1_GAMEPADS_MAX; i++) {
        if (!s_pad[i].used) continue;
        int j = n++;
        while (j > 0 && s_pad[order[j - 1]].order > s_pad[i].order) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = i;
    }
    if (idx >= 0 && idx < n) {
        int h = order[idx];
        *out = s_pad[h].info;
        if (player) {
            *player = -1;
            for (int p = 0; p < FH1_PLAYERS; p++) {
                if (resolve(p) == h) *player = p;
            }
        }
        ok = true;
    }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

void fh1_gamepad_assign(int player, const char *choice)
{
    if (player < 0 || player >= FH1_PLAYERS) return;
    load();
    char c[18];
    snprintf(c, sizeof(c), "%s", choice ? choice : "");
    if (player > 0 && strcmp(c, "fh1") == 0) c[0] = 0;          // only player 1 has the FH1's keys
    portENTER_CRITICAL(&s_mux);
    // A gamepad plays for one player: whoever had it by name goes back to automatic.
    for (int p = 0; is_pad_id(c) && p < FH1_PLAYERS; p++) {
        if (p != player && strcmp(s_choice[p], c) == 0) s_choice[p][0] = 0;
    }
    memcpy(s_choice[player], c, sizeof(c));
    char saved[FH1_PLAYERS][18];
    memcpy(saved, s_choice, sizeof(saved));
    // Its look now, if it is connected, for when it is away.
    fh1_gamepad_info_t seen = { 0 };
    if (strcmp(s_seen[player].id, c) == 0) seen = s_seen[player];          // the same one again: keep what we knew
    memcpy(seen.id, c, sizeof(seen.id));
    for (int i = 0; is_pad_id(c) && i < FH1_GAMEPADS_MAX; i++) {
        if (s_pad[i].used && strcmp(s_pad[i].info.id, c) == 0) seen = s_pad[i].info;
    }
    for (int p = 0; p < FH1_PLAYERS; p++) {
        if (strcmp(s_seen[p].id, s_choice[p]) != 0) memset(&s_seen[p], 0, sizeof(s_seen[p]));   // dropped above
    }
    s_seen[player] = seen;
    fh1_gamepad_info_t seen_all[FH1_PLAYERS];
    memcpy(seen_all, s_seen, sizeof(seen_all));
    portEXIT_CRITICAL(&s_mux);
    for (int p = 0; p < FH1_PLAYERS; p++) {
        fh1_settings_set_str("input", s_keys[p], saved[p]);
        remember(p, &seen_all[p]);
    }
    sync_player1();
    sync_leds();
}

void fh1_gamepad_assignment(int player, char *out, size_t n)
{
    load();
    portENTER_CRITICAL(&s_mux);
    snprintf(out, n, "%s", player >= 0 && player < FH1_PLAYERS ? s_choice[player] : "");
    portEXIT_CRITICAL(&s_mux);
}

bool fh1_gamepad_chosen(int player, fh1_gamepad_info_t *out)
{
    load();
    if (player < 0 || player >= FH1_PLAYERS) return false;
    portENTER_CRITICAL(&s_mux);
    const bool ok = is_pad_id(s_choice[player]);
    if (ok) *out = s_seen[player];
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

int fh1_gamepad_player_of_id(const char *id)
{
    load();
    int player = -1;
    portENTER_CRITICAL(&s_mux);
    for (int p = 0; p < FH1_PLAYERS && player < 0; p++) {
        int h = resolve(p);
        if (h >= 0 && strcmp(s_pad[h].info.id, id) == 0) player = p;
    }
    portEXIT_CRITICAL(&s_mux);
    return player;
}

bool fh1_gamepad_of_player(int player, fh1_gamepad_info_t *out)
{
    load();
    portENTER_CRITICAL(&s_mux);
    int h = resolve(player);
    if (h >= 0) *out = s_pad[h].info;
    portEXIT_CRITICAL(&s_mux);
    return h >= 0;
}

uint32_t fh1_input_player_keys(int player)
{
    if (player < 1) return 0;
    portENTER_CRITICAL(&s_mux);
    int h = resolve(player);
    uint32_t k = h >= 0 ? s_pad[h].keys : 0;
    portEXIT_CRITICAL(&s_mux);
    return k;
}
