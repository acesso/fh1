// System > Bridges: the extension boards (fh1_bridge.h). A list of the remembered bridges, with Bluetooth's
// switch and a search for new ones; each bridge opens a window titled with its alias that works like the
// System menu for it: Devices (its controllers: pair, forget, test, use as a player), Storage, About, and
// rename / forget. Windows keep the bridge's address, so a bridge forgotten elsewhere closes them.
#include <string.h>
#include <stdio.h>
#include "apps_priv.h"
#include "fh1_ui.h"
#include "fh1_i18n.h"
#include "fh1_input.h"
#include "fh1_bridge.h"
#include "fh1_gamepad.h"
#include "fh1_devices.h"
#include "fh1_settings.h"
#include "fh1_audio.h"

// ── Helpers ──

static int find(const uint8_t addr[6])
{
    fh1_bridge_view_t v;
    for (int i = 0; i < fh1_bridge_count(); i++) {
        if (fh1_bridge_get(i, &v) && memcmp(v.addr, addr, 6) == 0) return i;
    }
    return -1;
}

static void battery_text(uint8_t b, char *out, size_t n)
{
    if (b) snprintf(out, n, "%u%%", b * 100 / 255);
    else snprintf(out, n, "-");
}

static void pad_name(const fh1b_device_t *d, char *out, size_t n)
{
    char name[21];
    memcpy(name, d->name, 20);
    name[20] = 0;
    char *w = strstr(name, " Wireless");
    if (w && w != name) *w = 0;
    snprintf(out, n, "%s", name[0] ? name : "Gamepad");
}

static void pad_id(const fh1b_device_t *d, char *out, size_t n)
{
    snprintf(out, n, "%02X:%02X:%02X:%02X:%02X:%02X", d->addr[0], d->addr[1], d->addr[2], d->addr[3], d->addr[4], d->addr[5]);
}

// Which player uses this controller now (0-based), or -1.
static int player_of(const fh1b_device_t *d)
{
    char id[18];
    pad_id(d, id, sizeof(id));
    fh1_gamepad_info_t g;
    for (int p = 0; p < FH1_PLAYERS; p++) {
        if (fh1_gamepad_of_player(p, &g) && strcmp(g.id, id) == 0) return p;
    }
    return -1;
}

// The link's symbol: Bluetooth or Wi-Fi.
static const char *link_sym(const fh1_bridge_view_t *v)
{
    return v->link == FH1_BRIDGE_LINK_WIFI ? FH1_SYM_WIFI : v->link == FH1_BRIDGE_LINK_BLE ? FH1_SYM_BT : "";
}

static bool any_link_on(void) { return fh1_bridge_on() || fh1_bridge_wifi_link_on(); }

static void uptime_text(uint32_t s, char *out, size_t n)
{
    if (s >= 86400) snprintf(out, n, "%lud %luh", (unsigned long)(s / 86400), (unsigned long)(s / 3600 % 24));
    else if (s >= 3600) snprintf(out, n, "%luh %02lum", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60));
    else snprintf(out, n, "%lum %02lus", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

// ── A bridge's devices ──

static struct {
    fh1_win_t *win;
    uint8_t bridge[6];
    lv_obj_t *btn[FH1B_MAX_DEVICES], *icon[FH1B_MAX_DEVICES];
    uint8_t slot[FH1B_MAX_DEVICES];
    int shown;                          // controllers listed (-1: rebuild)
    lv_obj_t *empty;
    lv_obj_t *board_title, *bbtn[FH1B_BOARD_MAX], *quality;
    fh1b_board_dev_t board[FH1B_BOARD_MAX];
    int bshown;                         // board devices listed (-1: rebuild)
    lv_timer_t *timer;
} s_dv;

static void open_speaker(const uint8_t bridge[6], const char *alias, const fh1b_board_dev_t *d);

// A board device chosen: its keys open as a controller, its light switches.
static void dv_board_pick(void *u)
{
    const fh1b_board_dev_t *d = &s_dv.board[(int)(intptr_t)u];
    int bi = find(s_dv.bridge);
    if (d->cls == FH1B_CLASS_KEYS) {
        char id[18];
        snprintf(id, sizeof(id), "K:%02X%02X%02X%02X%02X%02X", s_dv.bridge[0], s_dv.bridge[1], s_dv.bridge[2], s_dv.bridge[3],
                 s_dv.bridge[4], s_dv.bridge[5]);
        pad_open(id);
    } else if (d->cls == FH1B_CLASS_LIGHT) {
        if (!fh1_bridge_board_set(bi, d->id, !d->count)) fh1_ui_message(T("Devices"), T("The bridge is not connected."));
    } else if (d->cls == FH1B_CLASS_AUDIO_OUT) {
        fh1_bridge_view_t v;
        if (bi >= 0 && fh1_bridge_get(bi, &v)) open_speaker(s_dv.bridge, v.alias, d);
    } else if (d->cls == FH1B_CLASS_STORAGE) {
        fh1_ui_message(T("Devices"), T("The bridge's SD card: shown for now (files over the link come later)."));
    } else {
        fh1_ui_message(T("Devices"), T("The microphones are listed for now; sound from the bridge comes later."));
    }
}

static void board_text(const fh1b_board_dev_t *d, bool is_sound, char *out, size_t n)
{
    char name[17];
    memcpy(name, d->name, 16);
    name[16] = 0;
    switch (d->cls) {
    case FH1B_CLASS_AUDIO_OUT:
        snprintf(out, n, "%s: %s%s%s%s", name, T("speaker"), d->flags & FH1B_BOARD_PLUGGED ? T(", headphones in") : "",
                 d->flags & FH1B_BOARD_PLAYING ? T(" · playing") : "", is_sound ? T(" · FH1 sound") : "");
        break;
    case FH1B_CLASS_STORAGE:
        if (d->flags & FH1B_BOARD_PLUGGED) snprintf(out, n, "%s", name);
        else snprintf(out, n, "SD: %s", T("no card"));
        break;
    case FH1B_CLASS_AUDIO_IN: snprintf(out, n, "%s: %s", name, T("2 microphones")); break;
    case FH1B_CLASS_KEYS: snprintf(out, n, "%s: %u %s", T("Keys"), d->count, T("(a controller)")); break;
    case FH1B_CLASS_LIGHT: snprintf(out, n, "LED: %s", d->count ? T("switched on") : T("switched off")); break;
    default: snprintf(out, n, "%s", name); break;
    }
}

static void dv_pick(void *u)
{
    fh1_bridge_view_t v;
    int bi = find(s_dv.bridge);
    if (bi < 0 || !fh1_bridge_get(bi, &v)) return;
    int i = (int)(intptr_t)u;
    for (int k = 0; k < v.ndev; k++) {
        if (v.dev[k].id != s_dv.slot[i]) continue;
        char id[18];
        pad_id(&v.dev[k], id, sizeof(id));
        pad_open(id);
    }
}

static void dv_pair(void *u)
{
    if (!fh1_bridge_pair(find(s_dv.bridge), 60)) {
        fh1_ui_message(T("Devices"), T("The bridge is not connected."));
        return;
    }
    fh1_ui_message(T("Devices"), T("Pairing for up to 60 s: the bridge leaves Wi-Fi meanwhile and comes back when a controller is in. DualSense: hold Create and PS until the light bar flashes. Joy-Con: hold its sync button."));
}

static void dv_forget_yes(bool yes, void *u)
{
    if (yes && !fh1_bridge_forget_controllers(find(s_dv.bridge))) fh1_ui_message(T("Devices"), T("The bridge is not connected."));
}
static void dv_forget(void *u)
{
    fh1_ui_confirm(T("Devices"), T("Forget every paired controller? They must be paired again."), dv_forget_yes, NULL);
}

static void dv_update(lv_timer_t *t)
{
    fh1_bridge_view_t v;
    int bi = find(s_dv.bridge);
    if (bi < 0 || !fh1_bridge_get(bi, &v)) {
        fh1_win_close(s_dv.win);
        return;
    }
    const int n = v.connected ? v.ndev : 0;
    if (n != s_dv.shown) {
        // The rows (controllers come and go): rebuilt after the two buttons.
        for (int i = 0; i < FH1B_MAX_DEVICES; i++) {
            if (s_dv.btn[i]) lv_obj_delete(s_dv.btn[i]);
            s_dv.btn[i] = NULL;
        }
        if (s_dv.empty) lv_obj_delete(s_dv.empty);
        s_dv.empty = NULL;
        lv_obj_t *body = fh1_win_body(s_dv.win);
        for (int i = 0; i < n; i++) {
            s_dv.slot[i] = v.dev[i].id;
            s_dv.btn[i] = fh1_ui_button(s_dv.win, body, "", dv_pick, (void *)(intptr_t)i);
            s_dv.icon[i] = fh1_ui_lead_pad(s_dv.btn[i], FH1_PAD_FLAT, 0);
        }
        if (!n) s_dv.empty = fh1_ui_line(body, v.connected ? T("No controllers. Pair one.") : T("The bridge is not connected."));
        s_dv.shown = n;
        s_dv.bshown = -1;                // the board's rows go after these
    }
    // The board's own devices (an audio kit's codec, keys, LED).
    const int nb = v.connected ? v.nboard : 0;
    if (nb != s_dv.bshown) {
        if (s_dv.board_title) lv_obj_delete(s_dv.board_title);
        s_dv.board_title = NULL;
        if (s_dv.quality) lv_obj_delete(s_dv.quality);
        s_dv.quality = NULL;
        for (int i = 0; i < FH1B_BOARD_MAX; i++) {
            if (s_dv.bbtn[i]) lv_obj_delete(s_dv.bbtn[i]);
            s_dv.bbtn[i] = NULL;
        }
        lv_obj_t *body = fh1_win_body(s_dv.win);
        if (nb) s_dv.board_title = fh1_ui_line(body, T("On the board:"));
        for (int i = 0; i < nb; i++) s_dv.bbtn[i] = fh1_ui_button(s_dv.win, body, "", dv_board_pick, (void *)(intptr_t)i);
        s_dv.bshown = nb;
    }
    for (int i = 0; i < nb; i++) {
        char t[64];
        s_dv.board[i] = v.board[i];
        const char *dev = fh1_bridge_audio_device(find(s_dv.bridge));
        board_text(&v.board[i], dev && strcmp(fh1_audio_output(), dev) == 0, t, sizeof(t));
        fh1_ui_button_set_text(s_dv.bbtn[i], t);
    }
    for (int i = 0; i < n; i++) {
        char name[24], bat[8], t[64];
        pad_name(&v.dev[i], name, sizeof(name));
        battery_text(v.dev[i].battery, bat, sizeof(bat));
        int p = player_of(&v.dev[i]);
        if (p >= 0) snprintf(t, sizeof(t), "%s · %s · %dP", name, bat, p + 1);
        else snprintf(t, sizeof(t), "%s · %s", name, bat);
        fh1_ui_button_set_text(s_dv.btn[i], t);
        uint8_t kind;
        uint32_t rgb;
        fh1_bridge_pad_look(&v.dev[i], &kind, &rgb);
        fh1_ui_pad_icon_set(s_dv.icon[i], kind, rgb);
    }
}

static void dv_closed(fh1_win_t *w, void *u)
{
    if (s_dv.timer) lv_timer_delete(s_dv.timer);
    memset(&s_dv, 0, sizeof(s_dv));
}

static void open_devices_of(const uint8_t bridge[6], const char *alias)
{
    if (s_dv.win && fh1_win_is_open(s_dv.win)) fh1_win_close(s_dv.win);
    char title[48];
    snprintf(title, sizeof(title), FH1_SYM_BRIDGE "%s: %s", alias, T("Devices"));
    fh1_win_t *w = fh1_win_create(title, 220, 180);
    if (!w) return;
    memset(&s_dv, 0, sizeof(s_dv));
    s_dv.win = w;
    memcpy(s_dv.bridge, bridge, 6);
    s_dv.shown = -1;
    s_dv.bshown = -1;
    const fh1_hint_t h[] = { { FH1_KEY_A, T("Open") }, { FH1_KEY_B, T("Back") } };
    fh1_win_set_hints(w, h, 2);
    fh1_win_on_close(w, dv_closed, NULL);
    lv_obj_t *body = fh1_win_body(w);
    fh1_ui_button(w, body, T("Pair a controller"), dv_pair, NULL);
    fh1_ui_button(w, body, T("Forget controllers"), dv_forget, NULL);
    dv_update(NULL);
    s_dv.timer = lv_timer_create(dv_update, 1000, NULL);
}

// ── A bridge's speaker: as the FH1's sound, the quality sent, and the codec's own controls ──
// The controls are the bridge's (protocol 7): drawn from their descriptions, as the FH1's own devices'
// settings are (a choice or a switch as a button that steps, a range as a slider).

#define SPK_CTRLS FH1B_CTRLS_MAX
static struct {
    fh1_win_t *win;
    uint8_t bridge[6];
    uint8_t dev;
    lv_obj_t *system, *quality, *state, *ctrl_btn[SPK_CTRLS], *wait;
    fh1b_ctrl_t ctrl[SPK_CTRLS];
    char fmt[SPK_CTRLS][24];           // a slider's format: it keeps the pointer, so it must outlive the build
    int nctrl;                         // drawn (-1: not yet)
    lv_timer_t *timer;
} s_spk;

static const char *const s_quality_names[] = { "48 kHz stereo", "24 kHz stereo", "24 kHz mono" };

// The k-th name in "a|b|c" (translated), into out.
static void opt_name(const char *opts, int k, char *out, size_t n)
{
    const char *p = opts;
    for (int i = 0; i < k && p; i++) {
        p = strchr(p, '|');
        if (p) p++;
    }
    if (!p) {
        snprintf(out, n, "%d", k);
        return;
    }
    const char *e = strchr(p, '|');
    char word[24];
    snprintf(word, sizeof(word), "%.*s", (int)(e ? e - p : (int)strlen(p)), p);
    snprintf(out, n, "%s", T(word));
}

static void spk_ctrl_text(const fh1b_ctrl_t *c, char *out, size_t n)
{
    char v[24];
    if (c->type == FH1B_CTRL_TOGGLE) snprintf(v, sizeof(v), "%s", c->value ? T("on") : T("off"));
    else opt_name(c->opts, c->value, v, sizeof(v));
    snprintf(out, n, "%s: %s", T(c->name), v);
}

static void spk_step(void *u)
{
    const int k = (int)(intptr_t)u;
    if (k < 0 || k >= s_spk.nctrl) return;
    fh1b_ctrl_t *c = &s_spk.ctrl[k];
    const int16_t v = c->value >= c->max ? c->min : (int16_t)(c->value + 1);
    if (!fh1_bridge_control_set(find(s_spk.bridge), s_spk.dev, c->id, v)) fh1_ui_message(T("Devices"), T("The bridge is not connected."));
    else c->value = v;                 // shown now; the bridge's answer confirms it
}

static void spk_slide(int v, bool done, void *u)
{
    const int k = (int)(intptr_t)u;
    if (!done || k < 0 || k >= s_spk.nctrl) return;
    s_spk.ctrl[k].value = (int16_t)v;
    fh1_bridge_control_set(find(s_spk.bridge), s_spk.dev, s_spk.ctrl[k].id, (int16_t)v);
}

static void spk_system(void *u)
{
    const char *dev = fh1_bridge_audio_device(find(s_spk.bridge));
    if (!dev) {
        fh1_ui_message(T("Devices"), T("The bridge's speaker plays when it is linked over Wi-Fi."));
        return;
    }
    fh1_audio_set_output(strcmp(fh1_audio_output(), dev) == 0 ? "" : dev);     // made the FH1's sound, or given back
}

static void spk_quality(void *u)
{
    fh1_bridge_set_audio_quality((uint8_t)((fh1_bridge_audio_quality() + 1) % FH1_BRIDGE_AUDIO_QUALITIES));
}

static void spk_update(lv_timer_t *t)
{
    fh1_bridge_view_t v;
    const int bi = find(s_spk.bridge);
    if (bi < 0 || !fh1_bridge_get(bi, &v)) {
        fh1_win_close(s_spk.win);
        return;
    }
    char b[72];
    const char *dev = fh1_bridge_audio_device(bi);
    const bool is_sound = dev && strcmp(fh1_audio_output(), dev) == 0;
    fh1_ui_button_set_text(s_spk.system, is_sound ? T("The FH1's sound: here (give it back)") : T("Use as the FH1's sound"));
    snprintf(b, sizeof(b), "%s: %s", T("Sound sent"), T(s_quality_names[fh1_bridge_audio_quality() % FH1_BRIDGE_AUDIO_QUALITIES]));
    fh1_ui_button_set_text(s_spk.quality, b);
    const fh1b_board_dev_t *d = NULL;
    for (int i = 0; i < v.nboard; i++) {
        if (v.board[i].id == s_spk.dev) d = &v.board[i];
    }
    if (d) snprintf(b, sizeof(b), "%s%s", d->flags & FH1B_BOARD_PLAYING ? T("playing") : T("idle"), d->flags & FH1B_BOARD_PLUGGED ? T(" · headphones") : "");
    fh1_ui_value_set(s_spk.state, d && v.connected ? b : T("not connected"));
    // The controls: drawn once they have come (the bridge sends them when the link is made).
    fh1b_ctrl_t c[SPK_CTRLS];
    const int n = fh1_bridge_controls(bi, s_spk.dev, c, SPK_CTRLS);
    if (s_spk.nctrl < 0 && n > 0) {
        if (s_spk.wait) lv_obj_delete(s_spk.wait);
        s_spk.wait = NULL;
        lv_obj_t *body = fh1_win_body(s_spk.win);
        for (int k = 0; k < n; k++) {
            s_spk.ctrl[k] = c[k];
            if (c[k].type == FH1B_CTRL_RANGE) {
                snprintf(s_spk.fmt[k], sizeof(s_spk.fmt[k]), c[k].opts[0] ? "%%d %s" : "%%d", c[k].opts);
                fh1_ui_slider(s_spk.win, body, T(c[k].name), s_spk.fmt[k], c[k].value, c[k].min, c[k].max, 1, spk_slide, (void *)(intptr_t)k);
            } else {
                s_spk.ctrl_btn[k] = fh1_ui_button(s_spk.win, body, "", spk_step, (void *)(intptr_t)k);
            }
        }
        s_spk.nctrl = n;
    }
    for (int k = 0; k < s_spk.nctrl && k < n; k++) {
        if (c[k].type == FH1B_CTRL_RANGE || !s_spk.ctrl_btn[k]) continue;
        s_spk.ctrl[k].value = c[k].value;          // as the bridge has it
        spk_ctrl_text(&c[k], b, sizeof(b));
        fh1_ui_button_set_text(s_spk.ctrl_btn[k], b);
    }
}

static void spk_closed(fh1_win_t *w, void *u)
{
    if (s_spk.timer) lv_timer_delete(s_spk.timer);
    memset(&s_spk, 0, sizeof(s_spk));
}

static void open_speaker(const uint8_t bridge[6], const char *alias, const fh1b_board_dev_t *d)
{
    if (s_spk.win && fh1_win_is_open(s_spk.win)) fh1_win_close(s_spk.win);
    char title[48], name[17];
    memcpy(name, d->name, 16);
    name[16] = 0;
    snprintf(title, sizeof(title), FH1_SYM_BRIDGE "%s: %s", alias, name);
    fh1_win_t *w = fh1_win_create(title, 250, 210);
    if (!w) return;
    memset(&s_spk, 0, sizeof(s_spk));
    s_spk.win = w;
    memcpy(s_spk.bridge, bridge, 6);
    s_spk.dev = d->id;
    s_spk.nctrl = -1;
    const fh1_hint_t h[] = { { FH1_KEY_A, T("Choose") }, { FH1_KEY_B, T("Back") } };
    fh1_win_set_hints(w, h, 2);
    fh1_win_on_close(w, spk_closed, NULL);
    lv_obj_t *body = fh1_win_body(w);
    s_spk.system = fh1_ui_button(w, body, "", spk_system, NULL);
    s_spk.quality = fh1_ui_button(w, body, "", spk_quality, NULL);
    s_spk.state = fh1_ui_value(body, T("State"));
    s_spk.wait = fh1_ui_line(body, T("Waiting for the bridge's controls..."));
    spk_update(NULL);
    s_spk.timer = lv_timer_create(spk_update, 500, NULL);
}

// ── A bridge's storage and about (like the FH1's own) ──

// RSSI as a quality bar: -90 dBm empty, -45 dBm full.
static int rssi_pct(int rssi)
{
    if (!rssi) return 0;
    int p = (rssi + 90) * 100 / 45;
    return p < 0 ? 0 : p > 100 ? 100 : p;
}

static void wifi_text(const fh1b_status_t *s, char *out, size_t n)
{
    switch (s->wifi) {
    case FH1B_WIFI_CONNECTED:
        snprintf(out, n, "%u.%u.%u.%u · %s", s->wifi_ip[0], s->wifi_ip[1], s->wifi_ip[2], s->wifi_ip[3], s->wifi_ssid);
        break;
    case FH1B_WIFI_CONNECTING: snprintf(out, n, "%s %s", T("connecting to"), s->wifi_ssid); break;
    case FH1B_WIFI_NO_NETWORK: snprintf(out, n, "%s", T("no network")); break;
    default: snprintf(out, n, "%s", T("switched off")); break;
    }
}

enum { AB_BOARD, AB_CPU, AB_MEMORY, AB_RAM, AB_VERSION, AB_UP, AB_BT, AB_LINK, AB_WIFI, AB_UPDATE, AB_CARRIES, AB_N };
enum { ST_APP, ST_NVS, ST_FLASH, ST_SAVED, ST_N };

static struct {
    fh1_win_t *win;
    uint8_t bridge[6];
    bool about;                        // else storage
    lv_obj_t *v[AB_N];
    lv_timer_t *timer;
    int ticks;
} s_info;

static void info_update(lv_timer_t *t)
{
    fh1_bridge_view_t v;
    int bi = find(s_info.bridge);
    if (bi < 0 || !fh1_bridge_get(bi, &v)) {
        fh1_win_close(s_info.win);
        return;
    }
    if (v.connected && (s_info.ticks++ % 3) == 0) fh1_bridge_request_status(bi);     // fresh numbers every 3 s
    char b[72];
    const fh1b_status_t *s = &v.status;
    const bool st = v.have_status;
    if (s_info.about) {
        char board[17] = "", fw[17] = "";
        memcpy(board, v.info.board, 16);
        memcpy(fw, v.info.firmware, 16);
        fh1_ui_value_set(s_info.v[AB_BOARD], v.have_info ? board : "-");
        if (st) snprintf(b, sizeof(b), "ESP32 · %u MHz", s->cpu_mhz);
        fh1_ui_value_set(s_info.v[AB_CPU], st ? b : "-");
        if (st) snprintf(b, sizeof(b), T("flash %u MB · RAM %lu KB"), s->flash_mb, (unsigned long)(s->heap_total / 1024));
        fh1_ui_value_set(s_info.v[AB_MEMORY], st && s->heap_total ? b : "-");
        if (st && s->heap_total) {
            int used = (int)((s->heap_total - s->heap_free) * 100 / s->heap_total);
            snprintf(b, sizeof(b), T("%lu KB free"), (unsigned long)(s->heap_free / 1024));
            fh1_ui_meter_set(s_info.v[AB_RAM], used, b, fh1_ui_level_colour(100 - used));
        } else {
            fh1_ui_meter_set(s_info.v[AB_RAM], 0, "-", 0);
        }
        snprintf(b, sizeof(b), "%s · %s %u", fw, T("protocol"), v.info.proto);
        fh1_ui_value_set(s_info.v[AB_VERSION], v.have_info ? b : "-");
        if (st) uptime_text(s->uptime_s, b, sizeof(b));
        fh1_ui_value_set(s_info.v[AB_UP], st && v.connected ? b : "-");
        snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", v.addr[0], v.addr[1], v.addr[2], v.addr[3], v.addr[4], v.addr[5]);
        fh1_ui_value_set(s_info.v[AB_BT], b);
        if (v.connected) {
            snprintf(b, sizeof(b), "%s%d dBm · %u ms", link_sym(&v), v.rssi, v.rtt_ms);
            int q = rssi_pct(v.rssi);
            fh1_ui_meter_set(s_info.v[AB_LINK], q, b, fh1_ui_level_colour(q));
        } else {
            fh1_ui_meter_set(s_info.v[AB_LINK], 0, T("not connected"), 0);
        }
        if (st && s->heap_total) wifi_text(s, b, sizeof(b));
        fh1_ui_value_set(s_info.v[AB_WIFI], st && s->heap_total ? b : "-");
        // Updates over Wi-Fi: its address and code (POST /ota).
        if (st && s->ota_code && s->wifi == FH1B_WIFI_CONNECTED)
            snprintf(b, sizeof(b), "%u.%u.%u.%u · %s %06lu", s->wifi_ip[0], s->wifi_ip[1], s->wifi_ip[2], s->wifi_ip[3], T("code"),
                     (unsigned long)s->ota_code);
        fh1_ui_value_set(s_info.v[AB_UPDATE], st && s->ota_code && s->wifi == FH1B_WIFI_CONNECTED ? b : "-");
        if (v.have_info) snprintf(b, sizeof(b), T("%u controllers · %u Hz"), v.info.max_devices, v.info.rate_hz);
        fh1_ui_value_set(s_info.v[AB_CARRIES], v.have_info ? b : "-");
    } else {
        if (st && s->app_part_kb) {
            int used = (int)(s->app_kb * 100 / s->app_part_kb);
            snprintf(b, sizeof(b), "%lu / %lu KB", (unsigned long)s->app_kb, (unsigned long)s->app_part_kb);
            fh1_ui_meter_set(s_info.v[ST_APP], used, b, fh1_ui_level_colour(100 - used));
        } else {
            fh1_ui_meter_set(s_info.v[ST_APP], 0, "-", 0);
        }
        if (st && s->nvs_total) {
            int used = s->nvs_used * 100 / s->nvs_total;
            snprintf(b, sizeof(b), T("%u / %u entries"), s->nvs_used, s->nvs_total);
            fh1_ui_meter_set(s_info.v[ST_NVS], used, b, fh1_ui_level_colour(100 - used));
        } else {
            fh1_ui_meter_set(s_info.v[ST_NVS], 0, "-", 0);
        }
        if (st) snprintf(b, sizeof(b), "%u MB", s->flash_mb);
        fh1_ui_value_set(s_info.v[ST_FLASH], st ? b : T("not connected"));
        if (st) snprintf(b, sizeof(b), "%u " FH1_SYM_PAD, s->paired);
        fh1_ui_value_set(s_info.v[ST_SAVED], st ? b : "-");
    }
}

static void info_closed(fh1_win_t *w, void *u)
{
    if (s_info.timer) lv_timer_delete(s_info.timer);
    memset(&s_info, 0, sizeof(s_info));
}

static void open_info(const uint8_t bridge[6], const char *alias, bool about)
{
    if (s_info.win && fh1_win_is_open(s_info.win)) fh1_win_close(s_info.win);
    char title[48];
    snprintf(title, sizeof(title), FH1_SYM_BRIDGE "%s: %s", alias, about ? T("About") : T("Storage"));
    fh1_win_t *w = fh1_win_create(title, about ? 280 : 240, about ? 220 : 150);
    if (!w) return;
    memset(&s_info, 0, sizeof(s_info));
    s_info.win = w;
    s_info.about = about;
    memcpy(s_info.bridge, bridge, 6);
    const fh1_hint_t h[] = { { FH1_KEY_B, T("Back") } };
    fh1_win_set_hints(w, h, 1);
    fh1_win_on_close(w, info_closed, NULL);
    lv_obj_t *body = fh1_win_body(w);
    if (about) {
        // As the FH1's About: board, CPU, memory, a RAM bar, version, up time; then its links.
        s_info.v[AB_BOARD] = fh1_ui_value(body, T("Board"));
        s_info.v[AB_CPU] = fh1_ui_value(body, "CPU");
        s_info.v[AB_MEMORY] = fh1_ui_value(body, T("Memory"));
        s_info.v[AB_RAM] = fh1_ui_meter(body, "RAM");
        s_info.v[AB_VERSION] = fh1_ui_value(body, T("Version"));
        s_info.v[AB_UP] = fh1_ui_value(body, T("Up"));
        s_info.v[AB_BT] = fh1_ui_value(body, FH1_SYM_BT "MAC");
        s_info.v[AB_LINK] = fh1_ui_meter(body, T("Link"));
        s_info.v[AB_WIFI] = fh1_ui_value(body, FH1_SYM_WIFI "Wi-Fi");
        s_info.v[AB_UPDATE] = fh1_ui_value(body, "OTA");
        s_info.v[AB_CARRIES] = fh1_ui_value(body, T("Carries"));
    } else {
        s_info.v[ST_APP] = fh1_ui_meter(body, "App");
        s_info.v[ST_NVS] = fh1_ui_meter(body, "NVS");
        s_info.v[ST_FLASH] = fh1_ui_value(body, "Flash");
        s_info.v[ST_SAVED] = fh1_ui_value(body, T("Saved"));
    }
    fh1_bridge_request_status(find(bridge));
    info_update(NULL);
    s_info.timer = lv_timer_create(info_update, 1000, NULL);
}

// ── A bridge's connectivity: its Bluetooth and its Wi-Fi, a view each ──

static struct {
    fh1_win_t *win;
    uint8_t bridge[6];
    lv_obj_t *bt_link, *bt_signal, *bt_rtt, *bt_pads;
    lv_obj_t *wifi_btn, *wifi_net, *wifi_state, *wifi_signal, *wifi_link;
    lv_timer_t *timer;
    int ticks;
    char ssid[33];                     // "other network": the name while the password is typed
} s_cn;

static int cn_bridge(void) { return find(s_cn.bridge); }

static void cn_update(lv_timer_t *t)
{
    fh1_bridge_view_t v;
    int bi = cn_bridge();
    if (bi < 0 || !fh1_bridge_get(bi, &v)) {
        fh1_win_close(s_cn.win);
        return;
    }
    if (v.connected && (s_cn.ticks++ % 2) == 0) fh1_bridge_request_status(bi);
    const fh1b_status_t *s = &v.status;
    const bool st = v.have_status && s->heap_total;          // a bridge that reports Wi-Fi
    char b[72];
    // Bluetooth: the BLE link's numbers while it is the link
    const bool ble = v.link == FH1_BRIDGE_LINK_BLE, wifi = v.link == FH1_BRIDGE_LINK_WIFI;
    if (ble) snprintf(b, sizeof(b), "BLE · %u.%02u ms · MTU %u", v.interval_x100 / 100, v.interval_x100 % 100, v.mtu);
    else snprintf(b, sizeof(b), "%s", wifi ? T("not in use (Wi-Fi)") : T("not connected"));
    fh1_ui_value_set(s_cn.bt_link, b);
    int q = ble ? rssi_pct(v.rssi) : 0;
    snprintf(b, sizeof(b), "%d dBm", v.rssi);
    fh1_ui_meter_set(s_cn.bt_signal, q, ble ? b : "-", fh1_ui_level_colour(q));
    if (ble) snprintf(b, sizeof(b), T("%u ms · %.0f msgs/s · %lu lost"), v.rtt_ms, v.msgs_per_s, (unsigned long)v.lost);
    fh1_ui_value_set(s_cn.bt_rtt, ble ? b : "-");
    if (v.have_status) snprintf(b, sizeof(b), "Classic · %u " FH1_SYM_PAD " · %u %s", v.ndev, s->paired, T("saved"));
    fh1_ui_value_set(s_cn.bt_pads, v.have_status ? b : "-");
    // Wi-Fi
    snprintf(b, sizeof(b), "Wi-Fi: %s", st && s->wifi != FH1B_WIFI_OFF ? T("switched on") : T("switched off"));
    fh1_ui_button_set_text(s_cn.wifi_btn, b);
    fh1_ui_value_set(s_cn.wifi_net, st && s->wifi_ssid[0] ? s->wifi_ssid : "-");
    if (st) wifi_text(s, b, sizeof(b));
    fh1_ui_value_set(s_cn.wifi_state, st ? b : "-");
    q = st && s->wifi == FH1B_WIFI_CONNECTED ? rssi_pct(s->wifi_rssi) : 0;
    snprintf(b, sizeof(b), "%d dBm", s->wifi_rssi);
    fh1_ui_meter_set(s_cn.wifi_signal, q, q ? b : "-", fh1_ui_level_colour(q));
    // The FH1's link to it
    if (wifi) {
        char c[56];
        snprintf(c, sizeof(c), T("%u ms · %.0f msgs/s · %lu lost"), v.rtt_ms, v.msgs_per_s, (unsigned long)v.lost);
        snprintf(b, sizeof(b), FH1_SYM_WIFI "%s", c);
    }
    else if (ble) snprintf(b, sizeof(b), FH1_SYM_BT "Bluetooth");
    else snprintf(b, sizeof(b), "%s", T("not connected"));
    fh1_ui_value_set(s_cn.wifi_link, b);
}

static void cn_wifi_off_yes(bool yes, void *u)
{
    if (yes && !fh1_bridge_wifi_enable(cn_bridge(), false)) fh1_ui_message(T("Connectivity"), T("The bridge is not connected."));
}

static void cn_wifi_toggle(void *u)
{
    fh1_bridge_view_t v;
    int bi = cn_bridge();
    if (bi < 0 || !fh1_bridge_get(bi, &v) || !v.connected) {
        fh1_ui_message(T("Connectivity"), T("The bridge is not connected."));
        return;
    }
    const bool on = v.have_status && v.status.wifi != FH1B_WIFI_OFF;
    if (on && v.link == FH1_BRIDGE_LINK_WIFI) {
        // Its link is Wi-Fi: switching it off leaves Bluetooth, if the FH1 has it on.
        fh1_ui_confirm(T("Connectivity"), fh1_bridge_on() ? T("The bridge is linked over Wi-Fi. Switch its Wi-Fi off? It comes back over Bluetooth.")
                                                          : T("The bridge is linked over Wi-Fi and the FH1's Bluetooth is off: switching its Wi-Fi off leaves it unreachable until Bluetooth is on. Switch it off?"),
                       cn_wifi_off_yes, NULL);
        return;
    }
    if (!fh1_bridge_wifi_enable(bi, !on)) fh1_ui_message(T("Connectivity"), T("The bridge is not connected."));
}

static void send_network(const char *ssid, const char *pass)
{
    fh1_bridge_view_t v;
    int bi = cn_bridge();
    if (bi < 0 || !fh1_bridge_get(bi, &v) || !v.connected) {
        fh1_ui_message(T("Connectivity"), T("The bridge is not connected."));
        return;
    }
    // A network is a secret: it goes over an encrypted Bluetooth link only (the Wi-Fi link is not encrypted).
    if (!fh1_bridge_on()) {
        fh1_ui_message(T("Connectivity"), T("A network is sent over Bluetooth (encrypted): switch the FH1's Bluetooth on first (System > Bridges)."));
        return;
    }
    if (!fh1_bridge_wifi_set(bi, ssid, pass)) {
        fh1_ui_message(T("Connectivity"), T("The bridge is not connected."));
        return;
    }
    if (v.link == FH1_BRIDGE_LINK_WIFI)
        fh1_ui_message(T("Connectivity"), T("Moving the bridge to Bluetooth to send the network encrypted; it comes back over Wi-Fi in a few seconds."));
    else
        fh1_ui_message(T("Connectivity"), T("Sending the network over an encrypted link; the bridge reconnects in a second."));
}

static void cn_use_fh1(void *u)
{
    char ssid[33] = "", pass[65] = "";
    fh1_settings_get_str("wifi", "ssid", ssid, sizeof(ssid));
    fh1_settings_get_str("wifi", "pass", pass, sizeof(pass));
    if (!ssid[0]) {
        fh1_ui_message(T("Connectivity"), T("The FH1 has no Wi-Fi network saved."));
        return;
    }
    send_network(ssid, pass);
    memset(pass, 0, sizeof(pass));
}

static void cn_pass_done(const char *pass, void *u)
{
    if (pass && s_cn.ssid[0]) send_network(s_cn.ssid, pass);
    memset(s_cn.ssid, 0, sizeof(s_cn.ssid));
}
static void cn_ssid_done(const char *ssid, void *u)
{
    if (!ssid || !ssid[0]) return;
    snprintf(s_cn.ssid, sizeof(s_cn.ssid), "%s", ssid);
    fh1_ui_keyboard(T("Password"), "", 64, cn_pass_done, NULL);
}
static void cn_other(void *u) { fh1_ui_keyboard(T("Network name"), "", 32, cn_ssid_done, NULL); }

static void cn_forget(void *u)
{
    if (!fh1_bridge_wifi_forget(cn_bridge())) fh1_ui_message(T("Connectivity"), T("The bridge is not connected."));
}

static void cn_pair(void *u)
{
    if (!fh1_bridge_pair(cn_bridge(), 60)) fh1_ui_message(T("Connectivity"), T("The bridge is not connected."));
    else fh1_ui_message(T("Connectivity"), T("Pairing for up to 60 s: the bridge leaves Wi-Fi meanwhile and comes back when a controller is in. DualSense: hold Create and PS until the light bar flashes. Joy-Con: hold its sync button."));
}

static void cn_closed(fh1_win_t *w, void *u)
{
    if (s_cn.timer) lv_timer_delete(s_cn.timer);
    memset(&s_cn, 0, sizeof(s_cn));
}

static void open_connectivity(const uint8_t bridge[6], const char *alias)
{
    if (s_cn.win && fh1_win_is_open(s_cn.win)) fh1_win_close(s_cn.win);
    char title[48];
    snprintf(title, sizeof(title), FH1_SYM_BRIDGE "%s: %s", alias, T("Connectivity"));
    fh1_win_t *w = fh1_win_create(title, 250, 200);
    if (!w) return;
    memset(&s_cn, 0, sizeof(s_cn));
    s_cn.win = w;
    memcpy(s_cn.bridge, bridge, 6);
    const fh1_hint_t h[] = { { FH1_KEY_A, T("Choose") }, { FH1_HINT_LEFTRIGHT, T("Switch") }, { FH1_KEY_B, T("Back") } };
    fh1_win_set_hints(w, h, 3);
    fh1_win_on_close(w, cn_closed, NULL);
    lv_obj_t *bt = fh1_win_add_view(w, FH1_SYM_BT "Bluetooth");
    fh1_ui_button(w, bt, T("Pair a controller"), cn_pair, NULL);
    s_cn.bt_link = fh1_ui_value(bt, T("Link"));
    s_cn.bt_signal = fh1_ui_meter(bt, T("Signal"));
    s_cn.bt_rtt = fh1_ui_value(bt, T("Delay"));
    s_cn.bt_pads = fh1_ui_value(bt, FH1_SYM_PAD);
    lv_obj_t *wf = fh1_win_add_view(w, FH1_SYM_WIFI "Wi-Fi");
    s_cn.wifi_btn = fh1_ui_button(w, wf, "", cn_wifi_toggle, NULL);
    fh1_ui_button(w, wf, T("Use the FH1's network"), cn_use_fh1, NULL);
    fh1_ui_button(w, wf, T("Other network..."), cn_other, NULL);
    fh1_ui_button(w, wf, T("Forget network"), cn_forget, NULL);
    s_cn.wifi_net = fh1_ui_value(wf, T("Network"));
    s_cn.wifi_state = fh1_ui_value(wf, T("State"));
    s_cn.wifi_signal = fh1_ui_meter(wf, T("Signal"));
    s_cn.wifi_link = fh1_ui_value(wf, "FH1");
    fh1_win_show_view(w, 0);
    fh1_bridge_request_status(find(bridge));
    cn_update(NULL);
    s_cn.timer = lv_timer_create(cn_update, 1000, NULL);
}

// ── A bridge: its own System menu ──

static struct {
    fh1_win_t *win;
    uint8_t bridge[6];
    char alias[24];
    lv_obj_t *state;
    lv_timer_t *timer;
} s_br;

static void br_devices(void *u) { open_devices_of(s_br.bridge, s_br.alias); }
static void br_storage(void *u) { open_info(s_br.bridge, s_br.alias, false); }
static void br_about(void *u) { open_info(s_br.bridge, s_br.alias, true); }
static void br_connectivity(void *u) { open_connectivity(s_br.bridge, s_br.alias); }

static void br_renamed(const char *text, void *u)
{
    int bi = find(s_br.bridge);
    if (!text || !text[0] || bi < 0) return;
    fh1_bridge_rename(bi, text);
    snprintf(s_br.alias, sizeof(s_br.alias), "%s", text);
    char title[32];
    snprintf(title, sizeof(title), FH1_SYM_BRIDGE "%s", s_br.alias);
    if (s_br.win) fh1_win_set_title(s_br.win, title);
}
static void br_rename(void *u) { fh1_ui_keyboard(T("Bridge name"), s_br.alias, 23, br_renamed, NULL); }

static void br_forget_yes(bool yes, void *u)
{
    if (!yes) return;
    fh1_bridge_forget(find(s_br.bridge));
    if (s_br.win) fh1_win_close(s_br.win);
}
static void br_forget(void *u)
{
    fh1_ui_confirm(T("Forget bridge"), T("Forget this bridge? It is added again by a search."), br_forget_yes, NULL);
}

static void br_update(lv_timer_t *t)
{
    fh1_bridge_view_t v;
    int bi = find(s_br.bridge);
    if (bi < 0 || !fh1_bridge_get(bi, &v)) {
        fh1_win_close(s_br.win);
        return;
    }
    char b[64];
    if (v.connected) {
        char c[48];
        snprintf(c, sizeof(c), T("connected · %u controller(s)"), v.ndev);
        snprintf(b, sizeof(b), "%s%s", link_sym(&v), c);
    }
    else snprintf(b, sizeof(b), "%s", any_link_on() ? T("not connected") : T("Bluetooth off"));
    fh1_ui_value_set(s_br.state, b);
}

static void br_closed(fh1_win_t *w, void *u)
{
    if (s_br.timer) lv_timer_delete(s_br.timer);
    memset(&s_br, 0, sizeof(s_br));
}

static void open_bridge(int i)
{
    fh1_bridge_view_t v;
    if (!fh1_bridge_get(i, &v)) return;
    if (s_br.win && fh1_win_is_open(s_br.win)) fh1_win_close(s_br.win);
    char title[32];
    snprintf(title, sizeof(title), FH1_SYM_BRIDGE "%s", v.alias);
    fh1_win_t *w = fh1_win_create(title, 200, 190);
    if (!w) return;
    memset(&s_br, 0, sizeof(s_br));
    s_br.win = w;
    memcpy(s_br.bridge, v.addr, 6);
    snprintf(s_br.alias, sizeof(s_br.alias), "%s", v.alias);
    const fh1_hint_t h[] = { { FH1_KEY_A, T("Open") }, { FH1_KEY_B, T("Back") } };
    fh1_win_set_hints(w, h, 2);
    fh1_win_on_close(w, br_closed, NULL);
    lv_obj_t *body = fh1_win_body(w);
    static const struct { const char *name; void (*cb)(void *); fh1_ui_icon_t icon; } items[] = {
        { "Devices", br_devices, FH1_ICON_MENU_DEVICES }, { "Connectivity", br_connectivity, FH1_ICON_MENU_NETWORK },
        { "Storage", br_storage, FH1_ICON_MENU_STORAGE }, { "About", br_about, FH1_ICON_MENU_ABOUT },
        { "Rename", br_rename, FH1_ICON_MENU_RENAME }, { "Forget bridge", br_forget, FH1_ICON_MENU_FORGET },
    };
    for (size_t k = 0; k < sizeof(items) / sizeof(items[0]); k++)
        fh1_ui_button_lead_icon(fh1_ui_button(w, body, T(items[k].name), items[k].cb, NULL), items[k].icon);
    s_br.state = fh1_ui_value(body, T("State"));
    br_update(NULL);
    s_br.timer = lv_timer_create(br_update, 1000, NULL);
}

// ── The list of bridges ──

static struct {
    fh1_win_t *win;
    lv_obj_t *bt, *search, *rows[FH1_BRIDGES_KNOWN], *empty;
    int shown;
    lv_timer_t *timer;
} s_list;

static void list_pick(void *u) { open_bridge((int)(intptr_t)u); }

static void list_bt(void *u)
{
    fh1_device_t *d = fh1_dev_find("bridge0");
    if (d) fh1_dev_set_enabled("bridge0", d->state != FH1_DEV_READY);
}

static void list_search(void *u)
{
    if (!any_link_on()) {
        fh1_ui_message(T("Bridges"), T("Switch Bluetooth on first."));
        return;
    }
    fh1_bridge_search(30);
}

static void list_update(lv_timer_t *t)
{
    char b[80];
    fh1_device_t *d = fh1_dev_find("bridge0");
    const bool on = d && d->state == FH1_DEV_READY;
    snprintf(b, sizeof(b), "Bluetooth: %s", on ? T("switched on") : T("switched off"));
    fh1_ui_button_set_text(s_list.bt, b);
    fh1_ui_button_set_text(s_list.search, fh1_bridge_searching() ? T("Searching...") : T("Search for bridges"));
    int n = fh1_bridge_count();
    if (n != s_list.shown) {
        for (int i = 0; i < FH1_BRIDGES_KNOWN; i++) {
            if (s_list.rows[i]) lv_obj_delete(s_list.rows[i]);
            s_list.rows[i] = NULL;
        }
        if (s_list.empty) lv_obj_delete(s_list.empty);
        s_list.empty = NULL;
        lv_obj_t *body = fh1_win_body(s_list.win);
        for (int i = 0; i < n && i < FH1_BRIDGES_KNOWN; i++) {
            s_list.rows[i] = fh1_ui_button(s_list.win, body, "", list_pick, (void *)(intptr_t)i);
            fh1_ui_button_lead_icon(s_list.rows[i], FH1_ICON_MENU_BRIDGE);
        }
        if (!n) s_list.empty = fh1_ui_label(body, T("No bridges yet. Switch Bluetooth on and power a bridge: the first one is added by itself, more with a search. Once a bridge has Wi-Fi (its Connectivity), Bluetooth can be off."));
        s_list.shown = n;
    }
    for (int i = 0; i < n && i < FH1_BRIDGES_KNOWN; i++) {
        fh1_bridge_view_t v;
        if (!fh1_bridge_get(i, &v)) continue;
        if (v.connected) snprintf(b, sizeof(b), "%s · %u" FH1_SYM_PAD " · %s", v.alias, v.ndev, link_sym(&v));
        else snprintf(b, sizeof(b), "%s · %s", v.alias, T("offline"));
        fh1_ui_button_set_text(s_list.rows[i], b);
    }
}

static void list_closed(fh1_win_t *w, void *u)
{
    if (s_list.timer) lv_timer_delete(s_list.timer);
    memset(&s_list, 0, sizeof(s_list));
}

void bridges_open(void)
{
    if (s_list.win && fh1_win_is_open(s_list.win)) {
        fh1_win_focus(s_list.win);
        return;
    }
    fh1_win_t *w = fh1_win_create(T("Bridges"), 220, 180);
    if (!w) return;
    memset(&s_list, 0, sizeof(s_list));
    s_list.win = w;
    s_list.shown = -1;
    const fh1_hint_t h[] = { { FH1_KEY_A, T("Open") }, { FH1_KEY_B, T("Close") } };
    fh1_win_set_hints(w, h, 2);
    fh1_win_on_close(w, list_closed, NULL);
    lv_obj_t *body = fh1_win_body(w);
    s_list.bt = fh1_ui_button(w, body, "", list_bt, NULL);
    s_list.search = fh1_ui_button(w, body, "", list_search, NULL);
    list_update(NULL);
    s_list.timer = lv_timer_create(list_update, 1000, NULL);
}
