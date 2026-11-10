// System > Inputs and outputs: which device each role uses, for every app. Players take gamepads
// (fh1_gamepad.h: automatic, the FH1's own keys only, or a chosen controller); the sound goes to the chosen
// audio output (fh1_audio_set_output). Below, the gamepads connected now and who plays with each. Every
// gamepad is shown with its icon (shape and colour, as in the tray), so two of the same model differ. Each
// opens its own window: its outputs (rumble, light, adaptive triggers), a player for it, and a live test of
// everything it reports (buttons, sticks, triggers, motion).
#include <string.h>
#include <math.h>
#include <stdio.h>
#include "apps_priv.h"
#include "fh1_ui.h"
#include "fh1_i18n.h"
#include "fh1_input.h"
#include "fh1_gamepad.h"
#include "fh1_audio.h"
#include "fh1_devices.h"
#include "fh1_bridge.h"
#include "fh1_users.h"

#define MAX_ROWS FH1_GAMEPADS_MAX

static struct {
    fh1_win_t *win;
    lv_obj_t *player[FH1_PLAYERS], *player_icon[FH1_PLAYERS], *sound, *pads[MAX_ROWS], *pad_icon[MAX_ROWS], *none;
    char ids[MAX_ROWS][18];
    int shown;
    lv_timer_t *timer;
} s_io;

// "DualSense ⌂CBA0" for a player's chosen gamepad; "DualSense (away)" when it is not connected (by its
// address if it was never seen).
static void pad_label(int player, const char *id, char *out, size_t n)
{
    fh1_gamepad_info_t g;
    for (int i = 0; fh1_gamepad_at(i, &g, NULL); i++) {
        if (strcmp(g.id, id) == 0) {
            snprintf(out, n, "%s " FH1_SYM_BRIDGE "%s", g.name, g.via);
            return;
        }
    }
    if (fh1_gamepad_chosen(player, &g) && g.name[0]) snprintf(out, n, "%s (%s)", g.name, T("away"));
    else snprintf(out, n, "%s (%s)", id, T("away"));
}

// The icon for a player's gamepad: the one playing, or the chosen one while away (as last seen).
static bool player_look(int p, fh1_gamepad_info_t *g)
{
    if (fh1_gamepad_of_player(p, g)) return true;
    return fh1_gamepad_chosen(p, g) && g->name[0];
}

static void player_text(int p, char *out, size_t n)
{
    char c[18], who[56];
    fh1_gamepad_assignment(p, c, sizeof(c));
    fh1_gamepad_info_t g;
    const bool has = fh1_gamepad_of_player(p, &g);
    if (!c[0]) {
        if (has) snprintf(who, sizeof(who), "%s · %s", T("auto"), g.name);
        else snprintf(who, sizeof(who), "%s · %s", T("auto"), p == 0 ? "FH1" : "-");
    } else if (strcmp(c, "fh1") == 0) {
        snprintf(who, sizeof(who), "%s", T("FH1 only"));
    } else {
        pad_label(p, c, who, sizeof(who));
    }
    snprintf(out, n, "%s %d: %s", T("Player"), p + 1, who);
}

static void sound_text(char *out, size_t n)
{
    const char *id = fh1_audio_output();
    fh1_device_t *d = id[0] ? fh1_dev_find(id) : NULL;
    if (d && strcmp(d->drv->name, "bridge-audio") == 0) snprintf(out, n, "%s: " FH1_SYM_BRIDGE "%s", T("Sound"), d->info);
    else if (d) snprintf(out, n, "%s: %s (%s)", T("Sound"), d->id, d->drv->name);
    else snprintf(out, n, "%s: %s", T("Sound"), T("auto"));
}

static void row_cb(void *u)
{
    int i = (int)(intptr_t)u;
    if (i >= 0 && i < MAX_ROWS && s_io.ids[i][0]) pad_open(s_io.ids[i]);
}

static void io_update(lv_timer_t *t)
{
    char b[96];
    for (int p = 0; p < FH1_PLAYERS; p++) {
        player_text(p, b, sizeof(b));
        fh1_ui_button_set_text(s_io.player[p], b);
        // The icon of the gamepad playing now; none for the FH1's own keys.
        fh1_gamepad_info_t g;
        const bool has = player_look(p, &g);
        if (has) fh1_ui_pad_icon_set(s_io.player_icon[p], g.kind, g.rgb);
        lv_obj_set_hidden(s_io.player_icon[p], !has);
    }
    sound_text(b, sizeof(b));
    fh1_ui_button_set_text(s_io.sound, b);
    // The gamepads connected now (rows rebuilt when their number changes).
    int n = fh1_gamepad_count();
    if (n > MAX_ROWS) n = MAX_ROWS;
    if (n != s_io.shown) {
        for (int i = 0; i < MAX_ROWS; i++) {
            if (s_io.pads[i]) lv_obj_delete(s_io.pads[i]);
            s_io.pads[i] = NULL;
        }
        if (s_io.none) lv_obj_delete(s_io.none);
        s_io.none = NULL;
        lv_obj_t *body = fh1_win_body(s_io.win);
        for (int i = 0; i < n; i++) {
            s_io.pads[i] = fh1_ui_button(s_io.win, body, "", row_cb, (void *)(intptr_t)i);
            s_io.pad_icon[i] = fh1_ui_lead_pad(s_io.pads[i], FH1_PAD_FLAT, 0);
        }
        if (!n) s_io.none = fh1_ui_line(body, T("No gamepads connected (System > Bridges)."));
        s_io.shown = n;
    }
    for (int i = 0; i < n; i++) {
        fh1_gamepad_info_t g;
        int p;
        if (!fh1_gamepad_at(i, &g, &p)) continue;
        char bat[8] = "-";
        if (g.battery) snprintf(bat, sizeof(bat), "%u%%", g.battery * 100 / 255);
        if (p >= 0) snprintf(b, sizeof(b), "%dP %s " FH1_SYM_BRIDGE "%s · %s", p + 1, g.name, g.via, bat);
        else snprintf(b, sizeof(b), "%s " FH1_SYM_BRIDGE "%s · %s", g.name, g.via, bat);
        fh1_ui_button_set_text(s_io.pads[i], b);
        fh1_ui_pad_icon_set(s_io.pad_icon[i], g.kind, g.rgb);
        memcpy(s_io.ids[i], g.id, sizeof(s_io.ids[i]));
    }
}

// ── Pickers ──

static struct {
    fh1_win_t *win;
    int role;                          // a player, or -1: the sound output
    char ids[MAX_ROWS + 3][18];
} s_pick;

static void pick_cb(void *u)
{
    int i = (int)(intptr_t)u;
    if (s_pick.role >= 0) fh1_gamepad_assign(s_pick.role, s_pick.ids[i]);
    else fh1_audio_set_output(s_pick.ids[i]);
    fh1_win_t *w = s_pick.win;
    s_pick.win = NULL;
    if (w) fh1_win_close(w);
    if (s_io.win) io_update(NULL);
}

static void pick_closed(fh1_win_t *w, void *u)
{
    if (s_pick.win == w) s_pick.win = NULL;
}

static void open_picker(int role)
{
    if (s_pick.win && fh1_win_is_open(s_pick.win)) fh1_win_close(s_pick.win);
    char title[40];
    if (role >= 0) snprintf(title, sizeof(title), "%s %d", T("Player"), role + 1);
    else snprintf(title, sizeof(title), "%s", T("Sound"));
    fh1_win_t *w = fh1_win_create(title, 220, 170);
    if (!w) return;
    memset(&s_pick, 0, sizeof(s_pick));
    s_pick.win = w;
    s_pick.role = role;
    const fh1_hint_t h[] = { { FH1_KEY_A, T("Choose") }, { FH1_KEY_B, T("Back") } };
    fh1_win_set_hints(w, h, 2);
    fh1_win_on_close(w, pick_closed, NULL);
    lv_obj_t *body = fh1_win_body(w);
    int k = 0;
    char cur[18] = "", t[64];
    if (role >= 0) fh1_gamepad_assignment(role, cur, sizeof(cur));
    else snprintf(cur, sizeof(cur), "%s", fh1_audio_output());
    int focus = 0;
    lv_obj_t *btn = NULL;
#define ADD(id, label)                                                         \
    do {                                                                       \
        snprintf(s_pick.ids[k], sizeof(s_pick.ids[k]), "%s", (id));           \
        if (strcmp(s_pick.ids[k], cur) == 0) focus = k;                        \
        btn = fh1_ui_button(w, body, (label), pick_cb, (void *)(intptr_t)k);   \
        k++;                                                                   \
    } while (0)
    if (role >= 0) {
        ADD("", T("Automatic"));
        if (role == 0) ADD("fh1", T("FH1 only"));
        bool cur_listed = !cur[0] || strcmp(cur, "fh1") == 0;
        fh1_gamepad_info_t g;
        for (int i = 0; k < MAX_ROWS + 2 && fh1_gamepad_at(i, &g, NULL); i++) {
            snprintf(t, sizeof(t), "%s " FH1_SYM_BRIDGE "%s", g.name, g.via);
            cur_listed |= strcmp(g.id, cur) == 0;
            ADD(g.id, t);
            fh1_ui_lead_pad(btn, g.kind, g.rgb);
        }
        if (!cur_listed) {                       // the chosen one, not connected now
            pad_label(role, cur, t, sizeof(t));
            ADD(cur, t);
            if (fh1_gamepad_chosen(role, &g) && g.name[0]) fh1_ui_lead_pad(btn, g.kind, g.rgb);
        }
    } else {
        ADD("", T("Automatic"));
        for (size_t i = 0; i < fh1_dev_count() && k < MAX_ROWS + 2; i++) {
            fh1_device_t *d = fh1_dev_at(i);
            if (strncmp(d->cls, "audio.out", 9) != 0) continue;
            if (strcmp(d->drv->name, "bridge-audio") == 0)       // a bridge's speaker: by the bridge
                snprintf(t, sizeof(t), FH1_SYM_BRIDGE "%s · %s", d->state == FH1_DEV_READY ? d->info : d->id, T(fh1_dev_state_name(d->state)));
            else
                snprintf(t, sizeof(t), "%s (%s) · %s", d->id, d->drv->name, T(fh1_dev_state_name(d->state)));
            ADD(d->id, t);
        }
    }
#undef ADD
    fh1_ui_focus_index(w, focus);
}

static void player_cb(void *u) { open_picker((int)(intptr_t)u); }

// Every bridge waits for a new controller at once (its own Devices does it for one).
void pair_all_cb(void *u)
{
    const int n = fh1_bridge_pair_all(60);
    if (!n) {
        fh1_ui_message(T("Pair a controller"), T("No bridge is connected."));
        return;
    }
    char b[200];
    snprintf(b, sizeof(b), T("%d bridge(s) waiting for 60 s (off Wi-Fi meanwhile). DualSense: hold Create and PS. Joy-Con: hold its sync button."), n);
    fh1_ui_message(T("Pair a controller"), b);
}
static void sound_cb(void *u) { open_picker(-1); }

static void io_closed(fh1_win_t *w, void *u)
{
    if (s_io.timer) lv_timer_delete(s_io.timer);
    memset(&s_io, 0, sizeof(s_io));
}

// ── Taking player 1 over: Menu held on another controller (fh1_gamepad_on_claim) ──

static char s_claim_id[18];

static void claim_ui(void *arg)
{
    fh1_gamepad_info_t g = { 0 };
    for (int i = 0; fh1_gamepad_at(i, &g, NULL); i++)
        if (strcmp(g.id, s_claim_id) == 0) break;
    if (strcmp(g.id, s_claim_id) != 0) return;            // gone meanwhile
    fh1_gamepad_assign(0, s_claim_id);
    fh1_gamepad_rumble(s_claim_id, 100, 180, 300);
    char t[64];
    snprintf(t, sizeof(t), "%s %s", g.name[0] ? g.name : T("Controller"), T("is player 1 now"));
    users_notify_player(0, t);
    if (fh1_users_count()) users_pick_for_pad(s_claim_id);   // whose it is; B keeps it as it was
}

static void on_claim(const char *id)
{
    strlcpy(s_claim_id, id, sizeof(s_claim_id));
    fh1_ui_async(claim_ui, NULL);
}

void io_claim_init(void) { fh1_gamepad_on_claim(on_claim); }

void io_open(void)
{
    if (s_io.win && fh1_win_is_open(s_io.win)) {
        fh1_win_focus(s_io.win);
        return;
    }
    fh1_win_t *w = fh1_win_create(T("Inputs and outputs"), 260, 200);
    if (!w) return;
    memset(&s_io, 0, sizeof(s_io));
    s_io.win = w;
    s_io.shown = -1;
    const fh1_hint_t h[] = { { FH1_KEY_A, T("Choose") }, { FH1_KEY_B, T("Close") } };
    fh1_win_set_hints(w, h, 2);
    fh1_win_on_close(w, io_closed, NULL);
    lv_obj_t *body = fh1_win_body(w);
    for (int p = 0; p < FH1_PLAYERS; p++) {
        s_io.player[p] = fh1_ui_button(w, body, "", player_cb, (void *)(intptr_t)p);
        // The player's gamepad's icon at the right end (hidden for the FH1's own keys).
        s_io.player_icon[p] = fh1_ui_pad_icon(s_io.player[p], FH1_PAD_FLAT, 0);
        lv_obj_align(s_io.player_icon[p], LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_pad_right(lv_obj_get_child(s_io.player[p], 1), 16, 0);     // the text stops before it
    }
    s_io.sound = fh1_ui_button(w, body, "", sound_cb, NULL);
    fh1_ui_button(w, body, T("Pair a controller (all bridges)"), pair_all_cb, NULL);
    io_update(NULL);
    s_io.timer = lv_timer_create(io_update, 1000, NULL);
}

// ── One controller: its outputs, its player, and a live test ──

static struct {
    fh1_win_t *win;
    char id[18];
    lv_obj_t *icon, *player, *battery, *via, *model, *addr, *caps, *trig_btn, *owner;
    lv_timer_t *timer;
    int colour, trig;
} s_pad;

static bool pad_info(fh1_gamepad_info_t *g, int *player)
{
    for (int i = 0; fh1_gamepad_at(i, g, player); i++) {
        if (strcmp(g->id, s_pad.id) == 0) return true;
    }
    return false;
}

static void pad_update(lv_timer_t *t)
{
    fh1_gamepad_info_t g;
    int p;
    if (!pad_info(&g, &p)) {
        fh1_ui_value_set(s_pad.player, T("disconnected"));
        return;
    }
    char b[64], who[40];
    users_pad_owner_text(s_pad.id, who, sizeof(who));
    snprintf(b, sizeof(b), "%s: %s", T("Belongs to"), who[0] ? who : T("nobody"));
    fh1_ui_button_set_text(s_pad.owner, b);
    fh1_ui_pad_icon_set(s_pad.icon, g.kind, g.rgb);
    if (p >= 0) snprintf(b, sizeof(b), T("Player %d"), p + 1);
    fh1_ui_value_set(s_pad.player, p >= 0 ? b : "-");
    if (g.battery) snprintf(b, sizeof(b), "%u%%", g.battery * 100 / 255);
    fh1_ui_value_set(s_pad.battery, g.battery ? b : "-");
    snprintf(b, sizeof(b), FH1_SYM_BRIDGE "%s", g.via);
    fh1_ui_value_set(s_pad.via, b);
    snprintf(b, sizeof(b), "%04x:%04x", g.vid, g.pid);
    fh1_ui_value_set(s_pad.model, g.vid || g.pid ? b : "-");
    fh1_ui_value_set(s_pad.addr, g.id);
    snprintf(b, sizeof(b), "%s%s%s%s%s", g.caps & FH1_PAD_CAP_RUMBLE ? T("rumble ") : "", g.caps & FH1_PAD_CAP_LIGHT ? T("light ") : "",
             g.caps & FH1_PAD_CAP_LEDS ? T("LEDs ") : "", g.caps & FH1_PAD_CAP_MOTION ? T("motion ") : "",
             g.caps & FH1_PAD_CAP_TRIGGERS ? T("triggers") : "");
    fh1_ui_value_set(s_pad.caps, b[0] ? b : "-");
}

static void pad_msg_if(bool ok)
{
    if (!ok) fh1_ui_message(T("Controller"), T("The controller is not connected, or has not that."));
}

static void pad_owner_cb(void *u) { users_pick_for_pad(s_pad.id); }

static void pad_rumble(void *u) { pad_msg_if(fh1_gamepad_rumble(s_pad.id, 200, 200, 400)); }

static void pad_light(void *u)
{
    static const uint32_t colours[] = { 0x0000ff, 0xff0000, 0x00ff00, 0xff00ff, 0xffa000, 0x00ffff, 0xffffff, 0x000000 };
    s_pad.colour = (s_pad.colour + 1) % (int)(sizeof(colours) / sizeof(colours[0]));
    pad_msg_if(fh1_gamepad_light(s_pad.id, colours[s_pad.colour]));
}

// The adaptive triggers, in turn: off, resistance, a click (weapon), vibration (both triggers).
static const char *const s_trig_names[] = { "off", "resistance", "click", "vibration" };
static void trig_text(void)
{
    char b[48];
    snprintf(b, sizeof(b), "%s: %s", T("Triggers"), T(s_trig_names[s_pad.trig]));
    fh1_ui_button_set_text(s_pad.trig_btn, b);
}
static void pad_trigger(void *u)
{
    s_pad.trig = (s_pad.trig + 1) % 4;
    static const uint8_t args[4][4] = { { FH1_TRIG_OFF, 0, 0, 0 }, { FH1_TRIG_FEEDBACK, 2, 6, 0 },
                                        { FH1_TRIG_WEAPON, 3, 6, 8 }, { FH1_TRIG_VIBRATION, 2, 6, 30 } };
    bool ok = true;
    for (int side = 0; side < 2; side++) ok &= fh1_gamepad_trigger(s_pad.id, side, args[s_pad.trig][0], args[s_pad.trig][1], args[s_pad.trig][2], args[s_pad.trig][3]);
    pad_msg_if(ok);
    trig_text();
}

static void pad_as_player(void *u)
{
    fh1_gamepad_assign((int)(intptr_t)u, s_pad.id);
    pad_update(NULL);
}

static void test_open(void *u);

static void pad_closed(fh1_win_t *w, void *u)
{
    if (s_pad.timer) lv_timer_delete(s_pad.timer);
    memset(&s_pad, 0, sizeof(s_pad));
}

void pad_open(const char *id)
{
    if (s_pad.win && fh1_win_is_open(s_pad.win)) fh1_win_close(s_pad.win);
    fh1_gamepad_info_t g = { 0 };
    bool found = false;
    for (int i = 0; !found && fh1_gamepad_at(i, &g, NULL); i++) found = strcmp(g.id, id) == 0;
    if (!found) return;
    fh1_win_t *w = fh1_win_create(g.name, 230, 210);
    if (!w) return;
    memset(&s_pad, 0, sizeof(s_pad));
    s_pad.win = w;
    snprintf(s_pad.id, sizeof(s_pad.id), "%s", id);
    const fh1_hint_t h[] = { { FH1_KEY_A, T("Choose") }, { FH1_KEY_B, T("Back") } };
    fh1_win_set_hints(w, h, 2);
    fh1_win_on_close(w, pad_closed, NULL);
    lv_obj_t *body = fh1_win_body(w);
    s_pad.icon = fh1_ui_lead_pad(fh1_ui_button(w, body, T("Test inputs"), test_open, NULL), g.kind, g.rgb);
    if (g.caps & FH1_PAD_CAP_RUMBLE) fh1_ui_button(w, body, T("Vibrate"), pad_rumble, NULL);
    if (g.caps & FH1_PAD_CAP_LIGHT) fh1_ui_button(w, body, T("Light colour"), pad_light, NULL);
    if (g.caps & FH1_PAD_CAP_TRIGGERS) {
        s_pad.trig_btn = fh1_ui_button(w, body, "", pad_trigger, NULL);
        trig_text();
    }
    for (int p = 0; p < FH1_PLAYERS; p++) {
        char t[32];
        snprintf(t, sizeof(t), T("Use as player %d"), p + 1);
        fh1_ui_button(w, body, t, pad_as_player, (void *)(intptr_t)p);
    }
    s_pad.owner = fh1_ui_button(w, body, "", pad_owner_cb, NULL);     // whose it is (System > Users)
    s_pad.player = fh1_ui_value(body, T("Player"));
    s_pad.battery = fh1_ui_value(body, T("Battery"));
    s_pad.via = fh1_ui_value(body, T("Via"));
    s_pad.caps = fh1_ui_value(body, T("Has"));
    s_pad.model = fh1_ui_value(body, T("Model"));
    s_pad.addr = fh1_ui_value(body, FH1_SYM_BT "MAC");
    pad_update(NULL);
    s_pad.timer = lv_timer_create(pad_update, 1000, NULL);
}

// The live test: everything the controller reports, twenty times a second, with its motion drawn: a tilt
// bubble (gravity from the accelerometer), the three rotation rates as bars, a heading needle (the yaw rate
// summed up; it drifts), and the strongest swing of the last second. Meanwhile its keys do not drive the
// interface (when it is player 1) and motion is streamed. Holding Start or Select (a Joy-Con's + or -) for
// 1.5 s closes it, as does PS / Home or the FH1's B.
// Axes as Bluepad32 gives them: gyro X pitch, Y yaw, Z roll (rad/s); accelerometer X right, Y up (9.81 at
// rest), Z back (m/s^2).
enum { TS_BTN, TS_LS, TS_RS, TS_L2, TS_R2, TS_SWING, TS_N };
#define TS_HOLD_MS 1500
#define TS_BOX 56                      // a motion box's side, in units
static struct {
    fh1_win_t *win;
    char id[18];
    lv_obj_t *v[TS_N];
    lv_obj_t *dot, *bar[3], *needle, *hold;
    lv_point_precise_t needle_pts[2];
    float yaw;                         // radians, summed
    uint32_t last_ms, hold_since, swing_at;
    float swing;                       // rad/s, the strongest lately
    lv_timer_t *timer;
    bool closing;
} s_test;

static lv_obj_t *ts_box(lv_obj_t *parent, const char *title)
{
    const int u = fh1_ui_scale();
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, TS_BOX * u, (TS_BOX + 10) * u);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    fh1_ui_label(col, title);
    lv_obj_t *box = lv_obj_create(col);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, TS_BOX * u, TS_BOX * u);
    lv_obj_set_style_border_width(box, u, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x707880), 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x10141a), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_OFF);
    return box;
}

static void ts_motion(lv_obj_t *body, uint32_t rgb)
{
    const int u = fh1_ui_scale(), half = TS_BOX * u / 2;
    lv_obj_t *row = lv_obj_create(body);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    // Tilt: a crosshair and the bubble.
    lv_obj_t *tilt = ts_box(row, T("Tilt"));
    for (int k = 0; k < 2; k++) {
        lv_obj_t *l = lv_obj_create(tilt);
        lv_obj_remove_style_all(l);
        lv_obj_set_style_bg_color(l, lv_color_hex(0x30363e), 0);
        lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
        if (k) lv_obj_set_size(l, u, TS_BOX * u), lv_obj_set_pos(l, half, 0);
        else lv_obj_set_size(l, TS_BOX * u, u), lv_obj_set_pos(l, 0, half);
    }
    s_test.dot = lv_obj_create(tilt);
    lv_obj_remove_style_all(s_test.dot);
    lv_obj_set_size(s_test.dot, 8 * u, 8 * u);
    lv_obj_set_style_radius(s_test.dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_test.dot, lv_color_hex(rgb ? rgb : 0x26a7c7), 0);
    lv_obj_set_style_bg_opa(s_test.dot, LV_OPA_COVER, 0);
    lv_obj_set_pos(s_test.dot, half - 4 * u, half - 4 * u);
    // Rotation rates: pitch, yaw, roll as bars from the middle.
    lv_obj_t *rates = ts_box(row, T("Rotation"));
    static const uint32_t colours[3] = { 0xe5302c, 0x2fb36a, 0x2a6cf0 };
    for (int k = 0; k < 3; k++) {
        lv_obj_t *mid = lv_obj_create(rates);
        lv_obj_remove_style_all(mid);
        lv_obj_set_size(mid, u, 10 * u);
        lv_obj_set_style_bg_color(mid, lv_color_hex(0x30363e), 0);
        lv_obj_set_style_bg_opa(mid, LV_OPA_COVER, 0);
        lv_obj_set_pos(mid, half, (8 + k * 16) * u);
        s_test.bar[k] = lv_obj_create(rates);
        lv_obj_remove_style_all(s_test.bar[k]);
        lv_obj_set_style_bg_color(s_test.bar[k], lv_color_hex(colours[k]), 0);
        lv_obj_set_style_bg_opa(s_test.bar[k], LV_OPA_COVER, 0);
        lv_obj_set_size(s_test.bar[k], 0, 8 * u);
        lv_obj_set_pos(s_test.bar[k], half, (9 + k * 16) * u);
    }
    // Heading: a needle from the middle.
    lv_obj_t *head = ts_box(row, T("Heading"));
    lv_obj_set_style_radius(head, LV_RADIUS_CIRCLE, 0);
    s_test.needle = lv_line_create(head);
    lv_obj_set_style_line_width(s_test.needle, 2 * u, 0);
    lv_obj_set_style_line_color(s_test.needle, lv_color_hex(0xf08a24), 0);
    lv_obj_set_style_line_rounded(s_test.needle, true, 0);
    s_test.needle_pts[0].x = half;
    s_test.needle_pts[0].y = half;
    s_test.needle_pts[1].x = half;
    s_test.needle_pts[1].y = 4 * u;
    lv_line_set_points(s_test.needle, s_test.needle_pts, 2);
}

static void ts_motion_update(const fh1_pad_state_t *st)
{
    const int u = fh1_ui_scale(), half = TS_BOX * u / 2, room = half - 5 * u;
    // Tilt: gravity across the controller (X right, Z back), 1 g to the edge.
    float x = st->accel[0] / 981.0f, z = st->accel[2] / 981.0f;
    x = x > 1 ? 1 : x < -1 ? -1 : x;
    z = z > 1 ? 1 : z < -1 ? -1 : z;
    lv_obj_set_pos(s_test.dot, half - 4 * u + (int)(x * room), half - 4 * u + (int)(z * room));
    // Rates: +-10 rad/s to the edge.
    for (int k = 0; k < 3; k++) {
        float r = st->gyro[k] / 10000.0f;
        r = r > 1 ? 1 : r < -1 ? -1 : r;
        const int w = (int)(r * (half - 2 * u));
        lv_obj_set_width(s_test.bar[k], w < 0 ? -w : w);
        lv_obj_set_x(s_test.bar[k], w < 0 ? half + w : half);
    }
    // Heading: the yaw rate summed over the time between readings.
    const uint32_t t = st->time_ms;
    if (s_test.last_ms && t > s_test.last_ms && t - s_test.last_ms < 500) s_test.yaw += st->gyro[1] / 1000.0f * (t - s_test.last_ms) / 1000.0f;
    s_test.last_ms = t;
    const float len = half - 4 * u;
    s_test.needle_pts[1].x = half + len * sinf(s_test.yaw);
    s_test.needle_pts[1].y = half - len * cosf(s_test.yaw);
    lv_line_set_points(s_test.needle, s_test.needle_pts, 2);
    // The swing: how fast it turns, the strongest of the last second.
    const float g = sqrtf((float)st->gyro[0] * st->gyro[0] + (float)st->gyro[1] * st->gyro[1] + (float)st->gyro[2] * st->gyro[2]) / 1000.0f;
    const uint32_t now = lv_tick_get();
    if (g > s_test.swing || now - s_test.swing_at > 1000) {
        s_test.swing = g;
        s_test.swing_at = now;
    }
    char b[48];
    snprintf(b, sizeof(b), "%.1f rad/s (%.0f\xC2\xB0/s)", s_test.swing, s_test.swing * 57.3f);
    fh1_ui_value_set(s_test.v[TS_SWING], b);
}

static void test_update(lv_timer_t *t)
{
    fh1_pad_state_t st;
    if (!fh1_gamepad_state(s_test.id, &st)) {
        fh1_ui_value_set(s_test.v[TS_BTN], T("disconnected"));
        return;
    }
    // Leaving: PS / Home at once; Start or Select held (every model has one of them).
    const uint32_t now = lv_tick_get();
    if (st.buttons & (FH1_PAD_BTN_START | FH1_PAD_BTN_SELECT)) {
        if (!s_test.hold_since) s_test.hold_since = now ? now : 1;
    } else {
        s_test.hold_since = 0;
    }
    const uint32_t held = s_test.hold_since ? now - s_test.hold_since : 0;
    if (((st.buttons & FH1_PAD_BTN_SYSTEM) || held >= TS_HOLD_MS) && !s_test.closing) {
        s_test.closing = true;
        fh1_win_close(s_test.win);
        return;
    }
    char b[96] = "";
    if (held) snprintf(b, sizeof(b), T("Leaving... %u%%"), (unsigned)(held * 100 / TS_HOLD_MS));
    else snprintf(b, sizeof(b), "%s", T("Hold Start or Select (+ or -) to leave."));
    if (strcmp(lv_label_get_text(s_test.hold), b) != 0) lv_label_set_text(s_test.hold, b);
    static const char *const names[] = { "A", "B", "X", "Y", "L1", "R1", "L2", "R2", "L3", "R3", "PS", "Sel", "Start", "Cap",
                                         "\xE2\x86\x91", "\xE2\x86\x93", "\xE2\x86\x90", "\xE2\x86\x92" };
    int n = 0;
    b[0] = 0;
    for (int i = 0; i < 18; i++) {
        if (st.buttons & (1u << i)) n += snprintf(b + n, sizeof(b) - n, "%s ", names[i]);
    }
    fh1_ui_value_set(s_test.v[TS_BTN], n ? b : "-");
    snprintf(b, sizeof(b), "%+4d %+4d", st.lx, st.ly);
    fh1_ui_value_set(s_test.v[TS_LS], b);
    snprintf(b, sizeof(b), "%+4d %+4d", st.rx, st.ry);
    fh1_ui_value_set(s_test.v[TS_RS], b);
    snprintf(b, sizeof(b), "%u", st.l2);
    fh1_ui_meter_set(s_test.v[TS_L2], st.l2 * 100 / 1023, b, 0x26a7c7);
    snprintf(b, sizeof(b), "%u", st.r2);
    fh1_ui_meter_set(s_test.v[TS_R2], st.r2 * 100 / 1023, b, 0x26a7c7);
    ts_motion_update(&st);
}

static void test_closed(fh1_win_t *w, void *u)
{
    if (s_test.timer) lv_timer_delete(s_test.timer);
    fh1_gamepad_mute_keys(false);
    fh1_gamepad_want_motion(false);
    memset(&s_test, 0, sizeof(s_test));
}

static void test_open(void *u)
{
    if (s_test.win && fh1_win_is_open(s_test.win)) fh1_win_close(s_test.win);
    char title[48];
    fh1_gamepad_info_t g = { 0 };
    for (int i = 0; fh1_gamepad_at(i, &g, NULL); i++) {
        if (strcmp(g.id, s_pad.id) == 0) break;
    }
    snprintf(title, sizeof(title), "%s: %s", g.name, T("Test"));
    fh1_win_t *w = fh1_win_create(title, 250, 210);
    if (!w) return;
    memset(&s_test, 0, sizeof(s_test));
    s_test.win = w;
    snprintf(s_test.id, sizeof(s_test.id), "%s", s_pad.id);
    const fh1_hint_t h[] = { { FH1_KEY_B, T("Back") } };
    fh1_win_set_hints(w, h, 1);
    fh1_win_on_close(w, test_closed, NULL);
    lv_obj_t *body = fh1_win_body(w);
    s_test.hold = fh1_ui_line(body, "");
    ts_motion(body, g.rgb);
    s_test.v[TS_SWING] = fh1_ui_value(body, T("Swing"));
    s_test.v[TS_BTN] = fh1_ui_value(body, T("Buttons"));
    s_test.v[TS_LS] = fh1_ui_value(body, T("Left stick"));
    s_test.v[TS_RS] = fh1_ui_value(body, T("Right stick"));
    s_test.v[TS_L2] = fh1_ui_meter(body, "L2");
    s_test.v[TS_R2] = fh1_ui_meter(body, "R2");
    fh1_gamepad_mute_keys(true);
    fh1_gamepad_want_motion(true);
    test_update(NULL);
    s_test.timer = lv_timer_create(test_update, 50, NULL);
}
