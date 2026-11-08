// Extension boards (Bluetooth bridges), the FH1's side (fh1_bridge.h). Two links, one at a time per bridge:
// Bluetooth LE (NimBLE as a central, started only while the device "bridge0" is enabled: it costs internal
// RAM) and Wi-Fi (UDP on the FH1's network, the device "bridge1"). Wi-Fi first: a bridge that beacons is
// taken over Wi-Fi (moved off BLE if it was there), and the FH1's Bluetooth is only needed to add a bridge
// and to give it a network. With Bluetooth on, the FH1's internal RAM fell to 2 KB at times and BLE links
// stalled; the A1S's Wi-Fi link measured 8-30 ms of delay spread with no losses (firmware/PLAN.md).
//
// Bridges are remembered (NVS "bridges": address and alias). While Bluetooth runs, the FH1 scans when no
// bridge is connected, for 30 s after one drops, and while the user searches; a remembered bridge it sees is
// connected (7.5 ms interval, MTU 185), its characteristics found, input, devices and events subscribed,
// info read and its status asked for. A bridge it does not know is added when none is known yet, or during a
// search. Each bridge's controllers become gamepads (fh1_gamepad.h) named after it; the gamepad module decides
// which player uses which. A ping a second measures each link; every 5 s the links are logged.
//
// Over Wi-Fi (fh1_bridge_proto.h, "Over Wi-Fi"): a bridge with Wi-Fi on broadcasts a beacon; a remembered
// one is linked by a hello (a new one is added as over BLE: none known yet, or during a search). A bridge on
// BLE is moved (its BLE link dropped; BLE leaves it alone while it beacons). The link drops after
// FH1B_UDP_TIMEOUT_MS without a datagram; a BLE link after BLE_SILENT_MS without a message.
#include <string.h>
#include <stdio.h>
#include "fh1_bridge.h"
#include "fh1_gamepad.h"
#include "fh1_input.h"
#include "fh1_audio.h"
#include "fh1_adpcm.h"
#include "fh1_settings.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_netif.h"

static const char *TAG = "bridge";

static const ble_uuid128_t s_uuid[6] = {
    BLE_UUID128_INIT(FH1B_UUID_BYTES(0)), BLE_UUID128_INIT(FH1B_UUID_BYTES(1)), BLE_UUID128_INIT(FH1B_UUID_BYTES(2)),
    BLE_UUID128_INIT(FH1B_UUID_BYTES(3)), BLE_UUID128_INIT(FH1B_UUID_BYTES(4)), BLE_UUID128_INIT(FH1B_UUID_BYTES(5)),
};

typedef struct {
    // Remembered
    bool known;
    uint8_t link;                      // FH1_BRIDGE_LINK_*
    uint32_t ip;                       // over Wi-Fi: its address (network order)
    uint32_t rx_ms;                    // over Wi-Fi: its last datagram
    uint32_t beacon_ms;                // its last Wi-Fi beacon (0: never)
    uint32_t rx_ble_ms;                // over BLE: its last message (a stalled link is dropped)
    int64_t ble_until;                 // us: kept on BLE until then (to send Wi-Fi credentials)
    volatile bool drop_wifi;           // asked of the Wi-Fi link's task
    ble_addr_t addr;                   // NimBLE's: val[] least significant byte first
    char alias[24];
    // Live
    uint16_t conn;
    uint16_t svc_start, svc_end;
    uint16_t h[6];                     // value handles by FH1B_CHR_*; the CCCD follows each value (BTstack's layout)
    int pad[FH1B_MAX_DEVICES];         // gamepad handle per bridge slot, -1 for none
    int keys_pad;                      // the board's keys as a gamepad, -1 for none
    fh1b_ctrl_t ctrls[FH1B_CTRLS_MAX]; // its board devices' controls, as last reported (protocol 7)
    uint8_t nctrls;
    fh1_bridge_view_t v;
    int last_seq;
    uint32_t msgs, msgs_at;
    uint32_t d_min, d_max;             // arrival minus the bridge's clock, this window: the spread is the one-way delay variation
    // Wi-Fi credentials waiting for the link to be encrypted (only then; the link drops right after).
    uint8_t creds[3 + 32 + 64];
    uint16_t creds_len;
} br_t;

typedef struct {
    ble_addr_t addr;
    char alias[24];
} saved_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;     // the views
static br_t s_br[FH1_BRIDGES_KNOWN];
static fh1_device_t *s_dev;
static volatile bool s_running;
static uint8_t s_own_addr_type;
static bool s_connecting;
static int64_t s_search_until, s_reconnect_until;              // us
static esp_timer_handle_t s_timer;
static volatile bool s_udp_on;         // the Wi-Fi link's task runs
static TaskHandle_t s_udp_task;
static int s_sock = -1;
static fh1_device_t *s_udp_dev;

static void scan_update(void);
static int gap_event(struct ble_gap_event *ev, void *arg);
static void send_creds(br_t *b);

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

#define BLE_SILENT_MS 3000              // input flows at 125 Hz over BLE: this long without anything is a stall

// Seen beaconing over Wi-Fi lately: reachable that way, so BLE leaves it alone (unless held for BLE).
static bool on_wifi(const br_t *b)
{
    return s_udp_on && b->beacon_ms && now_ms() - b->beacon_ms < 6000 && esp_timer_get_time() >= b->ble_until;
}

// Linked over BLE (the controller's connections are few).
static int ble_count(void)
{
    int n = 0;
    for (int i = 0; i < FH1_BRIDGES_KNOWN; i++) n += s_br[i].known && s_br[i].link == FH1_BRIDGE_LINK_BLE;
    return n;
}




static int known_count(void)
{
    int n = 0;
    for (int i = 0; i < FH1_BRIDGES_KNOWN; i++) n += s_br[i].known;
    return n;
}

static br_t *by_conn(uint16_t conn)
{
    for (int i = 0; i < FH1_BRIDGES_KNOWN; i++) {
        if (s_br[i].known && s_br[i].link == FH1_BRIDGE_LINK_BLE && s_br[i].conn == conn) return &s_br[i];
    }
    return NULL;
}

static br_t *by_ip(uint32_t ip)
{
    for (int i = 0; i < FH1_BRIDGES_KNOWN; i++) {
        if (s_br[i].known && s_br[i].link == FH1_BRIDGE_LINK_WIFI && s_br[i].ip == ip) return &s_br[i];
    }
    return NULL;
}

// Takes a bridge with no link for this one; false when it has one.
static bool claim(br_t *b, uint8_t link)
{
    bool ok = false;
    portENTER_CRITICAL(&s_mux);
    if (b->link == FH1_BRIDGE_LINK_NONE) {
        b->link = link;
        ok = true;
    }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

static void device_info(void)
{
    int ble = 0, wifi = 0, ble_pads = 0, wifi_pads = 0;
    for (int i = 0; i < FH1_BRIDGES_KNOWN; i++) {
        if (!s_br[i].known) continue;
        if (s_br[i].link == FH1_BRIDGE_LINK_BLE) ble++, ble_pads += s_br[i].v.ndev;
        if (s_br[i].link == FH1_BRIDGE_LINK_WIFI) wifi++, wifi_pads += s_br[i].v.ndev;
    }
    if (s_dev && s_running) {
        if (ble) snprintf(s_dev->info, sizeof(s_dev->info), "%d bridge(s), %d controller(s)", ble, ble_pads);
        else snprintf(s_dev->info, sizeof(s_dev->info), "searching for bridges");
    }
    if (s_udp_dev && s_udp_on) {
        if (wifi) snprintf(s_udp_dev->info, sizeof(s_udp_dev->info), "UDP %d: %d bridge(s), %d controller(s)", FH1B_UDP_PORT, wifi, wifi_pads);
        else snprintf(s_udp_dev->info, sizeof(s_udp_dev->info), "UDP %d: waiting for bridges", FH1B_UDP_PORT);
    }
}

// ── Remembered bridges ──

static void save(void)
{
    saved_t list[FH1_BRIDGES_KNOWN];
    int n = 0;
    for (int i = 0; i < FH1_BRIDGES_KNOWN; i++) {
        if (!s_br[i].known) continue;
        list[n].addr = s_br[i].addr;
        memcpy(list[n].alias, s_br[i].alias, sizeof(list[n].alias));
        n++;
    }
    nvs_handle_t h;
    if (nvs_open("bridges", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "list", list, n * sizeof(saved_t));
    nvs_commit(h);
    nvs_close(h);
}

static void load(void)
{
    saved_t list[FH1_BRIDGES_KNOWN];
    size_t len = sizeof(list);
    nvs_handle_t h;
    if (nvs_open("bridges", NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_blob(h, "list", list, &len) == ESP_OK) {
        for (size_t i = 0; i < len / sizeof(saved_t) && i < FH1_BRIDGES_KNOWN; i++) {
            s_br[i].known = true;
            s_br[i].addr = list[i].addr;
            memcpy(s_br[i].alias, list[i].alias, sizeof(s_br[i].alias));
            s_br[i].alias[sizeof(s_br[i].alias) - 1] = 0;
            // Aliases were "Ponte CBA2" / "Bridge CBA2" before the bridge symbol: keep the code only.
            char *sp = strrchr(s_br[i].alias, ' ');
            if (sp && strlen(sp + 1) == 4 && (strncmp(s_br[i].alias, "Ponte ", 6) == 0 || strncmp(s_br[i].alias, "Bridge ", 7) == 0))
                memmove(s_br[i].alias, sp + 1, 5);
        }
    }
    nvs_close(h);
}

static void reset_live(br_t *b)
{
    b->conn = BLE_HS_CONN_HANDLE_NONE;
    b->svc_start = b->svc_end = 0;
    memset(b->h, 0, sizeof(b->h));
    for (int i = 0; i < FH1B_MAX_DEVICES; i++) {
        if (b->pad[i] >= 0) fh1_gamepad_remove(b->pad[i]);
        b->pad[i] = -1;
    }
    if (b->keys_pad >= 0) fh1_gamepad_remove(b->keys_pad);
    b->keys_pad = -1;
    portENTER_CRITICAL(&s_mux);
    b->nctrls = 0;
    char alias[24];
    memcpy(alias, b->alias, sizeof(alias));
    uint8_t addr[6];
    memcpy(addr, b->v.addr, 6);
    memset(&b->v, 0, sizeof(b->v));
    memcpy(b->v.alias, alias, sizeof(alias));
    memcpy(b->v.addr, addr, 6);
    b->link = FH1_BRIDGE_LINK_NONE;
    b->ip = 0;
    portEXIT_CRITICAL(&s_mux);
    b->last_seq = -1;
}

static br_t *add_known(const ble_addr_t *addr)
{
    for (int i = 0; i < FH1_BRIDGES_KNOWN; i++) {
        br_t *b = &s_br[i];
        if (b->known) continue;
        memset(b, 0, sizeof(*b));
        b->known = true;
        b->addr = *addr;
        snprintf(b->alias, sizeof(b->alias), "%02X%02X", addr->val[1], addr->val[0]);     // the UI shows a bridge symbol before it
        for (int k = 0; k < FH1B_MAX_DEVICES; k++) b->pad[k] = -1;
        b->keys_pad = -1;
        reset_live(b);
        for (int k = 0; k < 6; k++) b->v.addr[k] = addr->val[5 - k];
        save();
        ESP_LOGI(TAG, "new bridge %s", b->alias);
        return b;
    }
    return NULL;
}

// ── The system keys and players: a controller's state as FH1 keys ──

static uint32_t keys_of(const fh1b_pad_t *p, uint8_t *stick)
{
    // The stick as a D-pad, with hysteresis (state per controller).
    const bool was_l = *stick & 1, was_r = *stick & 2, was_u = *stick & 4, was_d = *stick & 8;
    const bool sl = p->lx < (was_l ? -200 : -300), sr = p->lx > (was_r ? 200 : 300);
    const bool su = p->ly < (was_u ? -200 : -300), sd = p->ly > (was_d ? 200 : 300);
    *stick = sl | sr << 1 | su << 2 | sd << 3;
    uint32_t k = 0;
    if ((p->dpad & FH1B_DPAD_UP) || su) k |= 1u << FH1_KEY_UP;
    if ((p->dpad & FH1B_DPAD_DOWN) || sd) k |= 1u << FH1_KEY_DOWN;
    if ((p->dpad & FH1B_DPAD_LEFT) || sl) k |= 1u << FH1_KEY_LEFT;
    if ((p->dpad & FH1B_DPAD_RIGHT) || sr) k |= 1u << FH1_KEY_RIGHT;
    // Positional, as RetroArch maps a PlayStation pad: circle A, cross B, triangle X, square Y.
    if (p->buttons & FH1B_BTN_B) k |= 1u << FH1_KEY_A;
    if (p->buttons & FH1B_BTN_A) k |= 1u << FH1_KEY_B;
    if (p->buttons & FH1B_BTN_Y) k |= 1u << FH1_KEY_X;
    if (p->buttons & FH1B_BTN_X) k |= 1u << FH1_KEY_Y;
    if (p->misc & FH1B_MISC_START) k |= 1u << FH1_KEY_START;
    if (p->misc & FH1B_MISC_SELECT) k |= 1u << FH1_KEY_SELECT;
    if (p->misc & FH1B_MISC_SYSTEM) k |= 1u << FH1_KEY_MENU;
    return k;
}

// ── Messages from a bridge ──

static void on_input(br_t *b, const uint8_t *buf, int n)
{
    if (n < 6) return;
    const fh1b_input_t *in = (const fh1b_input_t *)buf;
    int count = in->count;
    if (FH1B_INPUT_SIZE(count) > n || count > FH1B_MAX_DEVICES) count = (n - 6) / 30;
    b->msgs++;
    const uint32_t d = now_ms() - in->time_ms;
    if (!b->d_min || d < b->d_min) b->d_min = d ? d : 1;
    if (d > b->d_max) b->d_max = d;
    if (b->last_seq >= 0) {
        uint8_t gap = (uint8_t)(in->seq - b->last_seq - 1);
        portENTER_CRITICAL(&s_mux);
        b->v.lost += gap;
        portEXIT_CRITICAL(&s_mux);
    }
    b->last_seq = in->seq;
    static uint8_t sticks[FH1_BRIDGES_KNOWN][FH1B_MAX_DEVICES];
    const uint32_t t = now_ms();
    for (int i = 0; i < count; i++) {
        const fh1b_pad_t *p = &in->pad[i];
        if (p->id >= FH1B_MAX_DEVICES || b->pad[p->id] < 0) continue;
        fh1_pad_state_t st = { .keys = keys_of(p, &sticks[b - s_br][p->id]), .lx = p->lx, .ly = p->ly, .rx = p->rx,
                               .ry = p->ry, .l2 = p->l2, .r2 = p->r2, .time_ms = t };
        // Bluepad32's buttons are positional with A at the bottom, as ours.
        st.buttons = p->buttons & 0x3ff;
        if (p->misc & FH1B_MISC_SYSTEM) st.buttons |= FH1_PAD_BTN_SYSTEM;
        if (p->misc & FH1B_MISC_SELECT) st.buttons |= FH1_PAD_BTN_SELECT;
        if (p->misc & FH1B_MISC_START) st.buttons |= FH1_PAD_BTN_START;
        if (p->misc & FH1B_MISC_CAPTURE) st.buttons |= FH1_PAD_BTN_CAPTURE;
        if (p->dpad & FH1B_DPAD_UP) st.buttons |= FH1_PAD_BTN_UP;
        if (p->dpad & FH1B_DPAD_DOWN) st.buttons |= FH1_PAD_BTN_DOWN;
        if (p->dpad & FH1B_DPAD_LEFT) st.buttons |= FH1_PAD_BTN_LEFT;
        if (p->dpad & FH1B_DPAD_RIGHT) st.buttons |= FH1_PAD_BTN_RIGHT;
        memcpy(st.gyro, p->gyro, sizeof(st.gyro));
        memcpy(st.accel, p->accel, sizeof(st.accel));
        fh1_gamepad_set_state(b->pad[p->id], &st);
    }
}

static void pad_name(const fh1b_device_t *d, char *out, size_t n)
{
    char name[21];
    memcpy(name, d->name, 20);
    name[20] = 0;
    char *w = strstr(name, " Wireless");               // "DualSense Wireless Controller" -> "DualSense"
    if (w && w != name) *w = 0;
    snprintf(out, n, "%s", name[0] ? name : "Gamepad");
}

// Bluepad32's controller type (fh1b_device_t.model) as an icon shape, and that kind's usual colour for a
// controller without a light the bridge set.
static void kind_of(uint8_t model, const uint8_t rgb[3], uint8_t *kind, uint32_t *colour)
{
    uint32_t def;
    switch (model) {
    case 33: case 34: case 45: case 46: case 56: *kind = FH1_PAD_PLAYSTATION; def = 0x2a6cf0; break;   // PS3/4/5, Move
    case 38: case 41: case 42: case 44: *kind = FH1_PAD_PRO; def = 0x3c4046; break;                // Switch Pro, Joy-Con pair
    case 31: case 32: *kind = FH1_PAD_PRO; def = 0xdcdcdc; break;                                  // Xbox
    case 39: *kind = FH1_PAD_JOYCON_L; def = 0x0ab9e6; break;                                      // neon blue
    case 40: *kind = FH1_PAD_JOYCON_R; def = 0xff3c28; break;                                      // neon red
    case 35: *kind = FH1_PAD_REMOTE; def = 0xf2f2f2; break;                                        // Wii remote
    default: *kind = FH1_PAD_FLAT; def = 0xb0b4ba; break;                                          // 8BitDo, generic
    }
    *colour = rgb[0] | rgb[1] | rgb[2] ? (uint32_t)(rgb[0] << 16 | rgb[1] << 8 | rgb[2]) : def;
}

void fh1_bridge_pad_look(const fh1b_device_t *d, uint8_t *kind, uint32_t *rgb) { kind_of(d->model, d->rgb, kind, rgb); }

// ── A controller's outputs, for the gamepad module: ctx is the bridge's slot << 4 | the controller's ──

static bool send_cmd(int i, const uint8_t *buf, uint16_t n);
#define CTX_BRIDGE(c) ((int)((intptr_t)(c) >> 4))
#define CTX_PAD(c) ((uint8_t)((intptr_t)(c) & 15))

static void op_rumble(void *c, uint8_t weak, uint8_t strong, uint16_t ms)
{
    uint8_t m[6] = { FH1B_CMD_RUMBLE, CTX_PAD(c), weak, strong, (uint8_t)ms, (uint8_t)(ms >> 8) };
    send_cmd(CTX_BRIDGE(c), m, sizeof(m));
}
static void op_light(void *c, uint32_t rgb)
{
    uint8_t m[5] = { FH1B_CMD_LIGHTBAR, CTX_PAD(c), (uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb };
    send_cmd(CTX_BRIDGE(c), m, sizeof(m));
}
static void op_leds(void *c, uint8_t mask)
{
    uint8_t m[3] = { FH1B_CMD_LEDS, CTX_PAD(c), mask };
    send_cmd(CTX_BRIDGE(c), m, sizeof(m));
}
static void op_trigger(void *c, int side, uint8_t effect, uint8_t a, uint8_t b, uint8_t cc)
{
    uint8_t m[7] = { FH1B_CMD_TRIGGER, CTX_PAD(c), (uint8_t)side, effect, a, b, cc };
    send_cmd(CTX_BRIDGE(c), m, sizeof(m));
}
static void op_motion(void *c, bool on)
{
    uint8_t m[2] = { FH1B_CMD_MOTION, on };
    send_cmd(CTX_BRIDGE(c), m, sizeof(m));
}
static const fh1_gamepad_ops_t s_pad_ops = { .rumble = op_rumble, .light = op_light, .leds = op_leds,
                                             .trigger = op_trigger, .motion = op_motion };

static uint8_t caps_of(uint8_t features)
{
    return (features & FH1B_CAP_RUMBLE ? FH1_PAD_CAP_RUMBLE : 0) | (features & FH1B_CAP_LIGHTBAR ? FH1_PAD_CAP_LIGHT : 0) |
           (features & FH1B_CAP_LEDS ? FH1_PAD_CAP_LEDS : 0) | (features & FH1B_CAP_MOTION ? FH1_PAD_CAP_MOTION : 0) |
           (features & FH1B_CAP_TRIGGERS ? FH1_PAD_CAP_TRIGGERS : 0);
}

static void on_devices(br_t *b, const uint8_t *buf, int n)
{
    if (n < 1) return;
    int count = buf[0];
    // Protocol 2 entries are 40 bytes, protocol 1 entries 36 (no colour): tell by the length.
    const int esz = count && (n - 1) / count >= (int)sizeof(fh1b_device_t) ? (int)sizeof(fh1b_device_t) : FH1B_DEVICE_V1_SIZE;
    if (count > FH1B_MAX_DEVICES || 1 + count * esz > n) count = (n - 1) / esz;
    if (count > FH1B_MAX_DEVICES) count = FH1B_MAX_DEVICES;
    fh1b_device_t dev[FH1B_MAX_DEVICES];
    memset(dev, 0, sizeof(dev));
    for (int i = 0; i < count; i++) memcpy(&dev[i], buf + 1 + i * esz, esz);
    portENTER_CRITICAL(&s_mux);
    memset(b->v.dev, 0, sizeof(b->v.dev));
    memcpy(b->v.dev, dev, count * sizeof(fh1b_device_t));
    b->v.ndev = (uint8_t)count;
    portEXIT_CRITICAL(&s_mux);
    // Gamepads: add the new, remove the gone, update the batteries.
    bool present[FH1B_MAX_DEVICES] = { 0 };
    for (int i = 0; i < count; i++) {
        const fh1b_device_t *d = &dev[i];
        if (d->id >= FH1B_MAX_DEVICES || d->cls != FH1B_CLASS_GAMEPAD) continue;
        present[d->id] = true;
        if (b->pad[d->id] < 0) {
            fh1_gamepad_info_t g = { 0 };
            snprintf(g.id, sizeof(g.id), "%02X:%02X:%02X:%02X:%02X:%02X", d->addr[0], d->addr[1], d->addr[2], d->addr[3],
                     d->addr[4], d->addr[5]);
            pad_name(d, g.name, sizeof(g.name));
            snprintf(g.via, sizeof(g.via), "%s", b->alias);
            g.battery = d->battery;
            kind_of(d->model, d->rgb, &g.kind, &g.rgb);
            g.caps = caps_of(d->features);
            // Bridges older than protocol 4 do not list LEDs but set them: the core keeps them by player.
            if (b->v.info.proto < 4) g.caps |= FH1_PAD_CAP_LEDS;
            g.vid = d->vid;
            g.pid = d->pid;
            b->pad[d->id] = fh1_gamepad_add(&g);
            fh1_gamepad_set_ops(b->pad[d->id], &s_pad_ops, (void *)(intptr_t)((b - s_br) << 4 | d->id));
            ESP_LOGI(TAG, "%s: controller %u: %s (%s), battery %u%%", b->alias, d->id, g.name, g.id, d->battery * 100 / 255);
        } else {
            fh1_gamepad_set_battery(b->pad[d->id], d->battery);
            uint8_t kind;
            uint32_t rgb;
            kind_of(d->model, d->rgb, &kind, &rgb);
            fh1_gamepad_set_colour(b->pad[d->id], rgb);
        }
    }
    for (int i = 0; i < FH1B_MAX_DEVICES; i++) {
        if (!present[i] && b->pad[i] >= 0) {
            fh1_gamepad_remove(b->pad[i]);
            b->pad[i] = -1;
        }
    }
    device_info();
}

// The board's own devices. Its keys become a gamepad that only a player who chooses it gets (manual), named
// after the board; the rest is listed in the view.
static void on_board(br_t *b, const uint8_t *buf, int n)
{
    int count = buf[0];
    if (count > FH1B_BOARD_MAX) count = FH1B_BOARD_MAX;
    if (1 + count * (int)sizeof(fh1b_board_dev_t) > n) count = (n - 1) / (int)sizeof(fh1b_board_dev_t);
    fh1b_board_dev_t list[FH1B_BOARD_MAX];
    memcpy(list, buf + 1, count * sizeof(fh1b_board_dev_t));
    bool keys = false;
    portENTER_CRITICAL(&s_mux);
    b->v.nboard = (uint8_t)count;
    memcpy(b->v.board, list, count * sizeof(fh1b_board_dev_t));
    portEXIT_CRITICAL(&s_mux);
    for (int i = 0; i < count; i++) keys |= list[i].cls == FH1B_CLASS_KEYS;
    // Their controls (protocol 7): asked for each device; they come as FH1B_EVT_CTRL.
    if (b->v.have_info && b->v.info.proto >= 7 && b->nctrls == 0) {     // once per link (the list comes again often)
        for (int i = 0; i < count; i++) {
            uint8_t c[2] = { FH1B_CMD_CTRLS, list[i].id };
            send_cmd(b - s_br, c, sizeof(c));
        }
    }
    if (keys && b->keys_pad < 0) {
        fh1_gamepad_info_t g = { .kind = FH1_PAD_KEYS, .rgb = 0x3a8f5c, .manual = true };
        snprintf(g.id, sizeof(g.id), "K:%02X%02X%02X%02X%02X%02X", b->v.addr[0], b->v.addr[1], b->v.addr[2], b->v.addr[3],
                 b->v.addr[4], b->v.addr[5]);
        snprintf(g.name, sizeof(g.name), "Audio Kit keys");
        snprintf(g.via, sizeof(g.via), "%s", b->alias);
        b->keys_pad = fh1_gamepad_add(&g);
        ESP_LOGI(TAG, "%s: its keys are a gamepad (%s)", b->alias, g.id);
    } else if (!keys && b->keys_pad >= 0) {
        fh1_gamepad_remove(b->keys_pad);
        b->keys_pad = -1;
    }
}

// The audio kit's keys (REC MODE PLAY SET VOL- VOL+) as a pad: PLAY A, SET B, MODE Select, REC Start,
// VOL- left, VOL+ right.
static void on_keys(br_t *b, uint32_t k)
{
    static const struct { uint32_t key; uint32_t btn; } map[6] = {
        { 1u << FH1_KEY_START, FH1_PAD_BTN_START }, { 1u << FH1_KEY_SELECT, FH1_PAD_BTN_SELECT }, { 1u << FH1_KEY_A, FH1_PAD_BTN_A },
        { 1u << FH1_KEY_B, FH1_PAD_BTN_B }, { 1u << FH1_KEY_LEFT, FH1_PAD_BTN_LEFT }, { 1u << FH1_KEY_RIGHT, FH1_PAD_BTN_RIGHT },
    };
    fh1_pad_state_t st = { .time_ms = now_ms() };
    for (int i = 0; i < 6; i++) {
        if (k & (1u << i)) {
            st.keys |= map[i].key;
            st.buttons |= map[i].btn;
        }
    }
    fh1_gamepad_set_state(b->keys_pad, &st);
}

static void on_event(br_t *b, const uint8_t *buf, int n)
{
    if (n < 1) return;
    if (buf[0] == FH1B_EVT_PONG && n >= 5) {
        uint32_t t;
        memcpy(&t, buf + 1, 4);
        uint16_t rtt = (uint16_t)(now_ms() - t);
        portENTER_CRITICAL(&s_mux);
        b->v.rtt_ms = rtt;
        if (rtt > b->v.rtt_max_ms) b->v.rtt_max_ms = rtt;
        if (b->link == FH1_BRIDGE_LINK_WIFI && n >= 6) b->v.rssi = (int8_t)buf[5];   // its Wi-Fi signal
        portEXIT_CRITICAL(&s_mux);
    } else if (buf[0] == FH1B_EVT_STATUS && n >= 1 + FH1B_STATUS_V1_SIZE) {
        const int len = n - 1 < (int)sizeof(fh1b_status_t) ? n - 1 : (int)sizeof(fh1b_status_t);
        portENTER_CRITICAL(&s_mux);
        memset(&b->v.status, 0, sizeof(b->v.status));
        memcpy(&b->v.status, buf + 1, len);
        b->v.have_status = true;
        portEXIT_CRITICAL(&s_mux);
    } else if (buf[0] == FH1B_EVT_BOARD && n >= 2) {
        on_board(b, buf + 1, n - 1);
    } else if (buf[0] == FH1B_EVT_CTRL && n >= 1 + (int)sizeof(fh1b_ctrl_t)) {
        fh1b_ctrl_t c;
        memcpy(&c, buf + 1, sizeof(c));
        c.name[sizeof(c.name) - 1] = 0;
        c.opts[sizeof(c.opts) - 1] = 0;
        portENTER_CRITICAL(&s_mux);
        int k = 0;
        while (k < b->nctrls && !(b->ctrls[k].dev == c.dev && b->ctrls[k].id == c.id)) k++;
        if (k < FH1B_CTRLS_MAX) {
            b->ctrls[k] = c;
            if (k == b->nctrls) b->nctrls++;
        }
        portEXIT_CRITICAL(&s_mux);
    } else if (buf[0] == FH1B_EVT_KEYS && n >= 6 && b->keys_pad >= 0) {
        uint32_t k;
        memcpy(&k, buf + 2, 4);
        on_keys(b, k);
    } else if (buf[0] == FH1B_EVT_PAIRING && n >= 2) {
        ESP_LOGI(TAG, "%s %s scanning for controllers", b->alias, buf[1] ? "started" : "stopped");
    }
}

// ── Commands ──

static bool udp_send(uint32_t ip, uint8_t type, const void *data, uint16_t len)
{
    uint8_t buf[FH1B_UDP_HDR + 128];
    if (s_sock < 0 || !ip || len > sizeof(buf) - FH1B_UDP_HDR) return false;
    buf[0] = 'F';
    buf[1] = '1';
    buf[2] = type;
    if (len) memcpy(buf + FH1B_UDP_HDR, data, len);
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(FH1B_UDP_PORT), .sin_addr.s_addr = ip };
    return sendto(s_sock, buf, FH1B_UDP_HDR + len, 0, (struct sockaddr *)&to, sizeof(to)) == FH1B_UDP_HDR + len;
}

static bool send_cmd(int i, const uint8_t *buf, uint16_t n)
{
    if (i < 0 || i >= FH1_BRIDGES_KNOWN) return false;
    br_t *b = &s_br[i];
    if (b->link == FH1_BRIDGE_LINK_WIFI) return udp_send(b->ip, FH1B_CHR_COMMAND, buf, n);
    uint16_t conn = b->conn, h = b->h[FH1B_CHR_COMMAND];
    if (!s_running || b->link != FH1_BRIDGE_LINK_BLE || conn == BLE_HS_CONN_HANDLE_NONE || !h) return false;
    return ble_gattc_write_no_rsp_flat(conn, h, buf, n) == 0;
}

// The views are indexed by position among the remembered bridges.
static int slot_of_view(int i)
{
    for (int k = 0, n = 0; k < FH1_BRIDGES_KNOWN; k++) {
        if (s_br[k].known && n++ == i) return k;
    }
    return -1;
}

bool fh1_bridge_pair(int i, int seconds)
{
    uint8_t c[2] = { FH1B_CMD_PAIR, (uint8_t)(seconds > 255 ? 255 : seconds < 0 ? 0 : seconds) };
    return send_cmd(slot_of_view(i), c, sizeof(c));
}
int fh1_bridge_pair_all(int seconds)
{
    int n = 0;
    for (int i = 0; i < fh1_bridge_count(); i++) n += fh1_bridge_pair(i, seconds);
    return n;
}

bool fh1_bridge_forget_controllers(int i)
{
    uint8_t c[1] = { FH1B_CMD_FORGET_ALL };
    return send_cmd(slot_of_view(i), c, sizeof(c));
}
bool fh1_bridge_rumble(int i, int pad, uint8_t weak, uint8_t strong, uint16_t ms)
{
    uint8_t c[6] = { FH1B_CMD_RUMBLE, (uint8_t)pad, weak, strong, (uint8_t)ms, (uint8_t)(ms >> 8) };
    return send_cmd(slot_of_view(i), c, sizeof(c));
}
bool fh1_bridge_lightbar(int i, int pad, uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t c[5] = { FH1B_CMD_LIGHTBAR, (uint8_t)pad, r, g, b };
    return send_cmd(slot_of_view(i), c, sizeof(c));
}
// Credentials are secrets: the link is encrypted for them only (Just Works pairing, nothing stored), they are
// written with a response, and then the link is dropped so it comes back unencrypted: encryption is not
// kept on for the rest of the session (it costs a key exchange and 4 bytes a packet).
static int creds_written(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    br_t *b = by_conn(conn);
    if (b) ESP_LOGI(TAG, "%s: Wi-Fi network sent (%s); reconnecting without encryption", b->alias, err->status == 0 ? "ok" : "failed");
    if (b) b->ble_until = 0;                   // Wi-Fi may take it again

    ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    return 0;
}

static void send_creds(br_t *b)
{
    if (b->conn != BLE_HS_CONN_HANDLE_NONE && b->h[FH1B_CHR_COMMAND])
        ble_gattc_write_flat(b->conn, b->h[FH1B_CHR_COMMAND], b->creds, b->creds_len, creds_written, NULL);
    memset(b->creds, 0, sizeof(b->creds));
    b->creds_len = 0;
}

bool fh1_bridge_wifi_set(int i, const char *ssid, const char *password)
{
    int k = slot_of_view(i);
    // Over BLE only: the link is encrypted for them (Wi-Fi's link is not). A bridge on Wi-Fi is moved to BLE
    // for them: its Wi-Fi link is dropped, BLE connects (Wi-Fi leaves it alone for 30 s), and they go once
    // the services are found.
    if (k < 0 || !s_running) return false;
    br_t *b = &s_br[k];
    const bool moving = b->link == FH1_BRIDGE_LINK_WIFI;
    if (!moving && (b->link != FH1_BRIDGE_LINK_BLE || b->conn == BLE_HS_CONN_HANDLE_NONE)) return false;
    size_t ls = strlen(ssid), lp = strlen(password);
    if (ls > 32 || lp > 64) return false;
    b->creds[0] = FH1B_CMD_WIFI_SET;
    b->creds[1] = (uint8_t)ls;
    memcpy(b->creds + 2, ssid, ls);
    b->creds[2 + ls] = (uint8_t)lp;
    memcpy(b->creds + 3 + ls, password, lp);
    b->creds_len = (uint16_t)(3 + ls + lp);
    if (moving) {
        b->ble_until = esp_timer_get_time() + 30000000;
        b->drop_wifi = true;
        s_reconnect_until = b->ble_until;
        ESP_LOGI(TAG, "%s: moving to BLE to send a Wi-Fi network", b->alias);
        return true;
    }
    if (b->v.encrypted) send_creds(b);
    else if (ble_gap_security_initiate(b->conn) != 0) {      // sent when the encryption is up (ENC_CHANGE)
        memset(b->creds, 0, sizeof(b->creds));
        b->creds_len = 0;
        return false;
    }
    return true;
}
bool fh1_bridge_wifi_enable(int i, bool on)
{
    uint8_t c[2] = { FH1B_CMD_WIFI_ENABLE, on };
    return send_cmd(slot_of_view(i), c, sizeof(c));
}
bool fh1_bridge_wifi_forget(int i)
{
    uint8_t c[1] = { FH1B_CMD_WIFI_FORGET };
    return send_cmd(slot_of_view(i), c, sizeof(c));
}

int fh1_bridge_controls(int i, uint8_t dev, fh1b_ctrl_t *out, int max)
{
    const int k = slot_of_view(i);
    if (k < 0) return 0;
    int n = 0;
    portENTER_CRITICAL(&s_mux);
    for (int j = 0; j < s_br[k].nctrls && n < max; j++) {
        if (s_br[k].ctrls[j].dev == dev) out[n++] = s_br[k].ctrls[j];
    }
    portEXIT_CRITICAL(&s_mux);
    // In their order (by id).
    for (int a = 1; a < n; a++) {
        fh1b_ctrl_t t = out[a];
        int b = a;
        while (b > 0 && out[b - 1].id > t.id) out[b] = out[b - 1], b--;
        out[b] = t;
    }
    return n;
}

bool fh1_bridge_control_set(int i, uint8_t dev, uint8_t id, int16_t value)
{
    uint8_t c[5] = { FH1B_CMD_CTRL_SET, dev, id, (uint8_t)value, (uint8_t)((uint16_t)value >> 8) };
    return send_cmd(slot_of_view(i), c, sizeof(c));
}

bool fh1_bridge_board_set(int i, uint8_t id, uint8_t value)
{
    uint8_t c[3] = { FH1B_CMD_BOARD_SET, id, value };
    return send_cmd(slot_of_view(i), c, sizeof(c));
}

bool fh1_bridge_request_status(int i)
{
    uint8_t c[1] = { FH1B_CMD_STATUS };
    return send_cmd(slot_of_view(i), c, sizeof(c));
}

// Every second: a ping and the signal of each link; every 5 s, their numbers in the log.
static void timer_cb(void *arg)
{
    uint32_t t = now_ms();
    static uint32_t window;
    const bool report = window && t - window >= 5000;
    if (!window || report) window = t;
    for (int i = 0; i < FH1_BRIDGES_KNOWN; i++) {
        br_t *b = &s_br[i];
        if (b->creds_len && b->ble_until && esp_timer_get_time() > b->ble_until) {   // never got to send them
            memset(b->creds, 0, sizeof(b->creds));
            b->creds_len = 0;
            b->ble_until = 0;
        }
        if (!b->known || b->link == FH1_BRIDGE_LINK_NONE) continue;
        uint8_t c[5] = { FH1B_CMD_PING };
        memcpy(c + 1, &t, 4);
        send_cmd(i, c, sizeof(c));
        const bool ble = b->link == FH1_BRIDGE_LINK_BLE;
        if (ble && s_running && b->conn != BLE_HS_CONN_HANDLE_NONE && (int32_t)(t - b->rx_ble_ms) > BLE_SILENT_MS) {
            ESP_LOGW(TAG, "%s: nothing over BLE for %d s, reconnecting", b->alias, BLE_SILENT_MS / 1000);
            b->rx_ble_ms = t + 10000;
            ble_gap_terminate(b->conn, BLE_ERR_REM_USER_CONN_TERM);
            continue;
        }
        int8_t rssi = 0;
        if (ble && s_running) ble_gap_conn_rssi(b->conn, &rssi);
        portENTER_CRITICAL(&s_mux);
        if (ble) b->v.rssi = rssi;                     // over Wi-Fi: the bridge's own signal, in its pongs
        if (report) {
            b->v.msgs_per_s = (b->msgs - b->msgs_at) / 5.0f;
            b->msgs_at = b->msgs;
            b->v.spread_ms = b->d_min ? (uint16_t)(b->d_max - b->d_min) : 0;
            b->d_min = b->d_max = 0;
        }
        fh1_bridge_view_t v = b->v;
        if (report) b->v.rtt_max_ms = 0;
        portEXIT_CRITICAL(&s_mux);
        if (report)
            ESP_LOGI(TAG, "%s (%s): %.1f msgs/s, %lu lost, input delay spread %u ms, round trip %u ms (worst %u), %d dBm, %u controller(s); internal %u KB free, lowest %u KB",
                     v.alias, ble ? "BLE" : "Wi-Fi", v.msgs_per_s, (unsigned long)v.lost, v.spread_ms, v.rtt_ms, v.rtt_max_ms, v.rssi, v.ndev,
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));
    }
}

// ── GATT client: after connecting, one step after another (arg: the bridge) ──

typedef void (*read_fn)(br_t *b, const uint8_t *buf, int n);
typedef struct {
    br_t *b;
    read_fn fn;
} read_ctx_t;
static read_ctx_t s_read_ctx[FH1_BRIDGES_KNOWN][2];

static int read_cb(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    read_ctx_t *c = arg;
    if (err->status == 0 && attr && c->b->conn == conn) {
        uint8_t buf[200];
        int n = OS_MBUF_PKTLEN(attr->om);
        if (n > (int)sizeof(buf)) n = sizeof(buf);
        os_mbuf_copydata(attr->om, 0, n, buf);
        c->fn(c->b, buf, n);
    }
    return 0;
}

static void on_info(br_t *b, const uint8_t *buf, int n)
{
    fh1b_info_t info = { 0 };
    memcpy(&info, buf, n < (int)sizeof(info) ? n : (int)sizeof(info));
    portENTER_CRITICAL(&s_mux);
    b->v.info = info;
    b->v.have_info = true;
    portEXIT_CRITICAL(&s_mux);
    char board[17] = "", fw[17] = "";
    memcpy(board, info.board, 16);
    memcpy(fw, info.firmware, 16);
    ESP_LOGI(TAG, "%s: protocol %u, %s, firmware %s, up to %u controllers, %u Hz", b->alias, info.proto, board, fw,
             info.max_devices, info.rate_hz);
    if (info.proto > FH1B_PROTO_VERSION) ESP_LOGW(TAG, "protocol %u, we speak up to %u", info.proto, FH1B_PROTO_VERSION);
}

static void read_start(br_t *b)
{
    read_ctx_t *c = s_read_ctx[b - s_br];
    c[0] = (read_ctx_t){ b, on_info };
    c[1] = (read_ctx_t){ b, on_devices };
    if (b->h[FH1B_CHR_INFO]) ble_gattc_read(b->conn, b->h[FH1B_CHR_INFO], read_cb, &c[0]);
    if (b->h[FH1B_CHR_DEVICES]) ble_gattc_read(b->conn, b->h[FH1B_CHR_DEVICES], read_cb, &c[1]);
    uint8_t st[1] = { FH1B_CMD_STATUS };
    send_cmd(b - s_br, st, 1);
    uint8_t bd[1] = { FH1B_CMD_BOARD };
    send_cmd(b - s_br, bd, 1);
    // Moved here to send a Wi-Fi network: encrypt now; they go at ENC_CHANGE.
    if (b->creds_len && ble_gap_security_initiate(b->conn) != 0) {
        memset(b->creds, 0, sizeof(b->creds));
        b->creds_len = 0;
    }
}

// Subscribes to events, devices and input in turn (step in the low bits of arg), then reads.
static int subscribe_next(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    static const int order[] = { FH1B_CHR_EVENT, FH1B_CHR_DEVICES, FH1B_CHR_INPUT };
    br_t *b = by_conn(conn);
    if (!b) return 0;
    int step = (int)(intptr_t)arg;
    if (step < 3) {
        static const uint8_t on[2] = { 1, 0 };
        if (b->h[order[step]] && ble_gattc_write_flat(conn, b->h[order[step]] + 1, on, 2, subscribe_next, (void *)(intptr_t)(step + 1)) == 0)
            return 0;
    }
    read_start(b);
    return 0;
}

static int chr_cb(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg)
{
    br_t *b = by_conn(conn);
    if (!b) return 0;
    if (err->status == 0 && chr) {
        for (int i = 1; i <= 5; i++) {
            if (ble_uuid_cmp(&chr->uuid.u, &s_uuid[i].u) == 0) b->h[i] = chr->val_handle;
        }
    } else if (err->status == BLE_HS_EDONE) {
        if (!b->h[FH1B_CHR_INPUT] || !b->h[FH1B_CHR_COMMAND]) {
            ESP_LOGW(TAG, "%s lacks characteristics", b->alias);
            ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        } else {
            subscribe_next(conn, err, NULL, (void *)0);
        }
    }
    return 0;
}

static int svc_cb(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *arg)
{
    br_t *b = by_conn(conn);
    if (!b) return 0;
    if (err->status == 0 && svc) {
        b->svc_start = svc->start_handle;
        b->svc_end = svc->end_handle;
    } else if (err->status == BLE_HS_EDONE) {
        if (b->svc_start) ble_gattc_disc_all_chrs(conn, b->svc_start, b->svc_end, chr_cb, NULL);
        else ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

static int mtu_cb(uint16_t conn, const struct ble_gatt_error *err, uint16_t mtu, void *arg)
{
    br_t *b = by_conn(conn);
    if (!b) return 0;
    portENTER_CRITICAL(&s_mux);
    b->v.mtu = mtu;
    portEXIT_CRITICAL(&s_mux);
    ble_gattc_disc_svc_by_uuid(conn, &s_uuid[0].u, svc_cb, NULL);
    return 0;
}

// ── GAP ──

static void note_interval(br_t *b)
{
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(b->conn, &desc) == 0) {
        portENTER_CRITICAL(&s_mux);
        b->v.interval_x100 = desc.conn_itvl * 125;
        portEXIT_CRITICAL(&s_mux);
    }
}

static bool advertises_bridge(const uint8_t *data, uint8_t len)
{
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, data, len) != 0) return false;
    for (int i = 0; i < f.num_uuids128; i++) {
        if (ble_uuid_cmp(&f.uuids128[i].u, &s_uuid[0].u) == 0) return true;
    }
    return false;
}

static void connect_to(br_t *b)
{
    if (!claim(b, FH1_BRIDGE_LINK_BLE)) return;
    ble_gap_disc_cancel();
    // 7.5 ms, no latency, 2 s supervision timeout.
    const struct ble_gap_conn_params p = { .scan_itvl = 0x10, .scan_window = 0x10, .itvl_min = 6, .itvl_max = 6,
                                           .latency = 0, .supervision_timeout = 200, .min_ce_len = 0, .max_ce_len = 0 };
    int rc = ble_gap_connect(s_own_addr_type, &b->addr, 5000, &p, gap_event, b);
    if (rc) {
        ESP_LOGW(TAG, "connecting to %s failed (%d)", b->alias, rc);
        b->link = FH1_BRIDGE_LINK_NONE;
        scan_update();
    } else {
        s_connecting = true;
    }
}

static void on_disc(const struct ble_gap_disc_desc *d)
{
    if (s_connecting || !advertises_bridge(d->data, d->length_data)) return;
    for (int i = 0; i < FH1_BRIDGES_KNOWN; i++) {
        br_t *b = &s_br[i];
        if (b->known && ble_addr_cmp(&b->addr, &d->addr) == 0) {
            if (b->link == FH1_BRIDGE_LINK_NONE && !on_wifi(b) && ble_count() < FH1_BRIDGES_LINKED) connect_to(b);
            return;
        }
    }
    // A new bridge: taken when none is known yet, or while the user searches.
    if ((known_count() == 0 || esp_timer_get_time() < s_search_until) && ble_count() < FH1_BRIDGES_LINKED) {
        br_t *b = add_known(&d->addr);
        if (b) connect_to(b);
    }
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_DISC: on_disc(&ev->disc); return 0;
    case BLE_GAP_EVENT_CONNECT: {
        br_t *b = arg;
        s_connecting = false;
        if (ev->connect.status != 0 || !b) {
            if (b) b->link = FH1_BRIDGE_LINK_NONE;
            scan_update();
            return 0;
        }
        b->conn = ev->connect.conn_handle;
        b->last_seq = -1;
        b->rx_ble_ms = now_ms() + 5000;                          // time to find the service and subscribe
        portENTER_CRITICAL(&s_mux);
        b->v.connected = true;
        b->v.link = FH1_BRIDGE_LINK_BLE;
        b->v.lost = 0;
        portEXIT_CRITICAL(&s_mux);
        note_interval(b);
        ESP_LOGI(TAG, "%s connected", b->alias);
        device_info();
        ble_gattc_exchange_mtu(b->conn, mtu_cb, NULL);
        scan_update();
        return 0;
    }
    case BLE_GAP_EVENT_DISCONNECT: {
        br_t *b = by_conn(ev->disconnect.conn.conn_handle);
        if (b) {
            ESP_LOGI(TAG, "%s disconnected (reason %d)", b->alias, ev->disconnect.reason);
            reset_live(b);
            s_reconnect_until = esp_timer_get_time() + 30000000;     // look for it for a while
        }
        device_info();
        scan_update();
        return 0;
    }
    case BLE_GAP_EVENT_ENC_CHANGE: {
        br_t *b = by_conn(ev->enc_change.conn_handle);
        if (b) {
            portENTER_CRITICAL(&s_mux);
            b->v.encrypted = ev->enc_change.status == 0;
            portEXIT_CRITICAL(&s_mux);
            ESP_LOGI(TAG, "%s: link %s", b->alias, ev->enc_change.status == 0 ? "encrypted" : "not encrypted");
            if (b->creds_len) {
                if (ev->enc_change.status == 0) send_creds(b);
                else memset(b->creds, 0, sizeof(b->creds)), b->creds_len = 0;
            }
        }
        return 0;
    }
    case BLE_GAP_EVENT_CONN_UPDATE: {
        br_t *b = by_conn(ev->conn_update.conn_handle);
        if (b) note_interval(b);
        return 0;
    }
    case BLE_GAP_EVENT_NOTIFY_RX: {
        br_t *b = by_conn(ev->notify_rx.conn_handle);
        if (!b) return 0;
        uint8_t buf[200];
        int n = OS_MBUF_PKTLEN(ev->notify_rx.om);
        if (n > (int)sizeof(buf)) n = sizeof(buf);
        os_mbuf_copydata(ev->notify_rx.om, 0, n, buf);
        uint16_t h = ev->notify_rx.attr_handle;
        b->rx_ble_ms = now_ms();
        if (h == b->h[FH1B_CHR_INPUT]) on_input(b, buf, n);
        else if (h == b->h[FH1B_CHR_DEVICES]) on_devices(b, buf, n);
        else if (h == b->h[FH1B_CHR_EVENT]) on_event(b, buf, n);
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE: scan_update(); return 0;
    default: return 0;
    }
}

// Scan when nothing is connected, for a while after a bridge dropped, and while searching; never at full
// link count. Scanning takes radio time from the links and from Wi-Fi.
static void scan_update(void)
{
    if (!s_running || s_connecting) return;
    const int64_t now = esp_timer_get_time();
    // Remembered bridges with no link, not reachable over Wi-Fi.
    int missing = 0;
    for (int i = 0; i < FH1_BRIDGES_KNOWN; i++) missing += s_br[i].known && s_br[i].link == FH1_BRIDGE_LINK_NONE && !on_wifi(&s_br[i]);
    const bool want = ble_count() < FH1_BRIDGES_LINKED &&
                      (now < s_search_until || known_count() == 0 ||
                       (missing && (ble_count() == 0 || now < s_reconnect_until)));
    if (!want) {
        if (ble_gap_disc_active()) ble_gap_disc_cancel();
        return;
    }
    if (ble_gap_disc_active()) return;
    const struct ble_gap_disc_params p = { .itvl = 0x50, .window = 0x30, .filter_policy = 0, .limited = 0, .passive = 1,
                                           .filter_duplicates = 0 };
    int rc = ble_gap_disc(s_own_addr_type, 10000, &p, gap_event, NULL);    // 10 s at a time, then decide again
    if (rc && rc != BLE_HS_EALREADY) ESP_LOGW(TAG, "scan failed (%d)", rc);
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    ESP_LOGI(TAG, "Bluetooth LE up; %d bridge(s) remembered", known_count());
    device_info();
    scan_update();
}

static void on_reset(int reason) { ESP_LOGW(TAG, "Bluetooth host reset (%d)", reason); }

static void host_task(void *arg)
{
    nimble_port_run();                 // returns after nimble_port_stop()
    nimble_port_freertos_deinit();
}

// ── The device ──

static esp_err_t bridge_probe(fh1_device_t *d) { return ESP_OK; }    // the radio is always there

// The remembered bridges, loaded once (by whichever link starts first).
static void table_init(void)
{
    static bool done;
    if (done) return;
    done = true;
    for (int i = 0; i < FH1_BRIDGES_KNOWN; i++) {
        for (int k = 0; k < FH1B_MAX_DEVICES; k++) s_br[i].pad[k] = -1;
        s_br[i].keys_pad = -1;
        s_br[i].conn = BLE_HS_CONN_HANDLE_NONE;
    }
    load();
    for (int i = 0; i < FH1_BRIDGES_KNOWN; i++) {
        if (!s_br[i].known) continue;
        reset_live(&s_br[i]);
        for (int k = 0; k < 6; k++) s_br[i].v.addr[k] = s_br[i].addr.val[5 - k];
    }
}

// The ping timer runs while either link does.
static void timer_on(void)
{
    if (!s_timer) {
        const esp_timer_create_args_t ta = { .callback = timer_cb, .name = "bridge" };
        esp_timer_create(&ta, &s_timer);
    }
    if (!esp_timer_is_active(s_timer)) esp_timer_start_periodic(s_timer, 1000000);
}

static void timer_off(void)
{
    if (!s_running && !s_udp_on && s_timer) esp_timer_stop(s_timer);
}

static esp_err_t bridge_start(fh1_device_t *d)
{
    if (s_running) return ESP_OK;
    s_dev = d;
    table_init();
    const size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth did not start: %s", esp_err_to_name(err));
        return err;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_att_set_preferred_mtu(185);
    // Pairing: Just Works with LE Secure Connections, nothing stored (the link is encrypted anew each time).
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = 0;
    ble_hs_cfg.sm_their_key_dist = 0;
    s_connecting = false;
    s_running = true;
    nimble_port_freertos_init(host_task);
    timer_on();
    ESP_LOGI(TAG, "Bluetooth on: %u KB of internal RAM taken", (unsigned)((before - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)) / 1024));
    snprintf(d->info, sizeof(d->info), "starting Bluetooth");
    return ESP_OK;
}

static void bridge_stop(fh1_device_t *d)
{
    if (!s_running) return;
    s_running = false;
    timer_off();
    for (int i = 0; i < FH1_BRIDGES_KNOWN; i++) {
        if (s_br[i].known && s_br[i].link == FH1_BRIDGE_LINK_BLE && s_br[i].conn != BLE_HS_CONN_HANDLE_NONE)
            ble_gap_terminate(s_br[i].conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    ble_gap_disc_cancel();
    if (nimble_port_stop() == 0) nimble_port_deinit();
    for (int i = 0; i < FH1_BRIDGES_KNOWN; i++) {
        if (s_br[i].known && s_br[i].link == FH1_BRIDGE_LINK_BLE) reset_live(&s_br[i]);
    }
    ESP_LOGI(TAG, "Bluetooth off");
}

const fh1_driver_t fh1_bridge_driver = {
    .name = "nimble-bridge",
    .probe = bridge_probe,
    .start = bridge_start,
    .stop = bridge_stop,
};

// ── The Wi-Fi link (the device "bridge1") ──

static void udp_drop(br_t *b, const char *why)
{
    ESP_LOGI(TAG, "%s: Wi-Fi link %s", b->alias, why);
    reset_live(b);
    device_info();
}

// A bridge's beacon: link a remembered one (or add a new one, as over BLE) when BLE does not have it.
static void on_beacon(uint32_t ip, const uint8_t *buf, int n)
{
    if (n < 6 + 8) return;                                     // its address and the start of its info
    const uint8_t *a = buf;
    br_t *b = NULL;
    for (int i = 0; i < FH1_BRIDGES_KNOWN && !b; i++) {
        bool same = s_br[i].known;
        for (int k = 0; k < 6 && same; k++) same = s_br[i].addr.val[k] == a[5 - k];
        if (same) b = &s_br[i];
    }
    if (!b) {
        if (known_count() != 0 && esp_timer_get_time() >= s_search_until) return;
        ble_addr_t addr = { .type = BLE_ADDR_PUBLIC };
        for (int k = 0; k < 6; k++) addr.val[k] = a[5 - k];
        if (!(b = add_known(&addr))) return;
    }
    b->beacon_ms = now_ms();
    if (esp_timer_get_time() < b->ble_until) return;           // held for BLE (to send a network)
    if (b->link == FH1_BRIDGE_LINK_BLE) {
        // Wi-Fi first: drop BLE; the next beacon links it here.
        if (s_running && b->conn != BLE_HS_CONN_HANDLE_NONE) {
            ESP_LOGI(TAG, "%s: on Wi-Fi, leaving BLE", b->alias);
            ble_gap_terminate(b->conn, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }
    if (b->link == FH1_BRIDGE_LINK_WIFI) {
        // It beacons, so it lost us (it restarted, or its address changed): say hello again.
        b->ip = ip;
    } else {
        if (!claim(b, FH1_BRIDGE_LINK_WIFI)) return;
        b->ip = ip;
        b->rx_ms = now_ms();
        b->last_seq = -1;
        portENTER_CRITICAL(&s_mux);
        b->v.connected = true;
        b->v.link = FH1_BRIDGE_LINK_WIFI;
        b->v.lost = 0;
        b->v.mtu = 0;
        b->v.interval_x100 = 0;
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGI(TAG, "%s linked over Wi-Fi (" IPSTR ")", b->alias, IP2STR((esp_ip4_addr_t *)&ip));
        on_info(b, buf + 6, n - 6);
        device_info();
    }
    udp_send(ip, FH1B_UDP_HELLO, NULL, 0);
    uint8_t st[1] = { FH1B_CMD_STATUS };
    udp_send(ip, FH1B_CHR_COMMAND, st, 1);
    uint8_t bd[1] = { FH1B_CMD_BOARD };
    udp_send(ip, FH1B_CHR_COMMAND, bd, 1);
}

static void udp_task(void *arg)
{
    // The socket needs the network stack, which the Wi-Fi device brings up.
    while (s_udp_on && !esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) vTaskDelay(pdMS_TO_TICKS(500));
    int sock = s_udp_on ? socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP) : -1;
    if (sock >= 0) {
        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = htons(FH1B_UDP_PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
        if (bind(sock, (struct sockaddr *)&me, sizeof(me)) != 0) {
            ESP_LOGE(TAG, "Wi-Fi link: port %d is taken", FH1B_UDP_PORT);
            close(sock);
            sock = -1;
        }
    }
    s_sock = sock;
    if (sock >= 0) ESP_LOGI(TAG, "Wi-Fi link: listening on UDP %d", FH1B_UDP_PORT);
    device_info();
    static uint8_t buf[FH1B_UDP_HDR + 200];
    while (s_udp_on && sock >= 0) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        const int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n >= FH1B_UDP_HDR && buf[0] == 'F' && buf[1] == '1') {
            const uint32_t ip = from.sin_addr.s_addr;
            const uint8_t *m = buf + FH1B_UDP_HDR;
            const int len = n - FH1B_UDP_HDR;
            if (buf[2] == FH1B_UDP_BEACON) {
                on_beacon(ip, m, len);
            } else {
                br_t *b = by_ip(ip);
                if (b) {
                    b->rx_ms = now_ms();
                    switch (buf[2]) {
                    case FH1B_CHR_INFO: on_info(b, m, len); break;
                    case FH1B_CHR_DEVICES: on_devices(b, m, len); break;
                    case FH1B_CHR_INPUT: on_input(b, m, len); break;
                    case FH1B_CHR_EVENT: on_event(b, m, len); break;
                    default: break;
                    }
                }
            }
        }
        const uint32_t t = now_ms();
        for (int i = 0; i < FH1_BRIDGES_KNOWN; i++) {
            br_t *b = &s_br[i];
            if (!b->known || b->link != FH1_BRIDGE_LINK_WIFI) continue;
            if (b->drop_wifi) {
                b->drop_wifi = false;
                udp_send(b->ip, FH1B_UDP_BYE, NULL, 0);
                udp_drop(b, "dropped (to BLE)");
                if (s_running) scan_update();
            } else if (t - b->rx_ms > FH1B_UDP_TIMEOUT_MS) {
                udp_drop(b, "lost");
            }
        }
    }
    // Off: say bye, drop the links.
    for (int i = 0; i < FH1_BRIDGES_KNOWN; i++) {
        br_t *b = &s_br[i];
        if (b->known && b->link == FH1_BRIDGE_LINK_WIFI) {
            udp_send(b->ip, FH1B_UDP_BYE, NULL, 0);
            udp_drop(b, "off");
        }
    }
    s_sock = -1;
    if (sock >= 0) close(sock);
    s_udp_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t net_probe(fh1_device_t *d)
{
    snprintf(d->info, sizeof(d->info), "UDP %d on the FH1's Wi-Fi", FH1B_UDP_PORT);
    return ESP_OK;
}

static esp_err_t net_start(fh1_device_t *d)
{
    if (s_udp_on) return ESP_OK;
    for (int i = 0; i < 50 && s_udp_task; i++) vTaskDelay(pdMS_TO_TICKS(20));     // a stop still finishing
    if (s_udp_task) return ESP_ERR_INVALID_STATE;
    s_udp_dev = d;
    table_init();
    s_udp_on = true;
    // Its stack in PSRAM: internal RAM is what the FH1 is short of.
    enum { STACK = 4096 };
    static StaticTask_t tcb;
    static StackType_t *stack;
    if (!stack) stack = heap_caps_malloc(STACK, MALLOC_CAP_SPIRAM);
    if (!stack || !(s_udp_task = xTaskCreateStaticPinnedToCore(udp_task, "bridge_udp", STACK, NULL, 5, stack, &tcb, 0))) {
        s_udp_on = false;
        return ESP_ERR_NO_MEM;
    }
    timer_on();
    return ESP_OK;
}

static void net_stop(fh1_device_t *d)
{
    s_udp_on = false;                  // the task says bye and ends within 200 ms
    timer_off();
}

const fh1_driver_t fh1_bridge_net_driver = {
    .name = "udp-bridge",
    .probe = net_probe,
    .start = net_start,
    .stop = net_stop,
};

// ── Sound to a bridge's audio out (devices "baudio0", "baudio1"; protocol 6, Wi-Fi only) ──
// Device n plays on the n-th bridge (in remembered order) linked over Wi-Fi with an audio out that streams.
// The app's sound goes as 16-bit PCM (ADPCM sounded distant and noisy) in the quality chosen
// (fh1_bridge_audio_quality): 24 kHz stereo (the default), 48 kHz stereo (192 KB/s: too much for the A1S), 24 kHz mono
// (rates above 24 kHz halved through a 31-tap low-pass). It is paced like a codec would: write blocks while
// more than AUD_AHEAD_MS is queued ahead of real time (the bridge buffers up to ~680 ms).
#define AUD_DEVS 2
#define AUD_AHEAD_MS 250
static uint8_t s_aud_quality = 0xFF;  // FH1_BRIDGE_AUDIO_*, loaded on first use

typedef struct {
    int slot;                          // s_br index, -1 none
    uint8_t board_id;                  // its audio out's board device id
    fh1_adpcm_t enc;
    int16_t pcm[FH1B_AUDIO_PCM_SAMPLES];   // mono samples, or stereo pairs (half as many frames)
    int fill;                          // frames
    bool stereo;                       // what pcm holds now
    uint16_t seq;
    int64_t clock_us;                  // when the last queued sample plays
    uint8_t volume;
    int16_t hist[32], hist_r[32];      // decimation: the last inputs (a ring; right channel too) and inputs since an output
    uint8_t hpos;
    int acc_n;
} aud_t;

// Low-pass taps (Q15, Blackman-windowed sinc, 31 taps) for halving (cut 0.235 fs) and quartering (0.1175 fs).
static const int16_t s_lp2[31] = { 0, 3, 5, -32, -37, 110, 154, -258, -464, 475, 1171, -716, -2836, 909, 10200, 15402,
                                   10200, 909, -2836, -716, 1171, 475, -464, -258, 154, 110, -37, -32, 5, 3, 0 };
static const int16_t s_lp4[31] = { 0, -2, -2, 19, 70, 121, 82, -139, -527, -851, -687, 365, 2361, 4830, 6894, 7700,
                                   6894, 4830, 2361, 365, -687, -851, -527, -139, 82, 121, 70, 19, -2, -2, 0 };
static aud_t s_aud[AUD_DEVS] = { { .slot = -1, .volume = 70 }, { .slot = -1, .volume = 70 } };
static uint32_t s_aud_send_fails;      // sendto refused (no buffers): the link cannot take the quality
static uint32_t s_aud_sent, s_aud_send_max_us, s_aud_in, s_aud_wait_ms;
static uint64_t s_aud_send_us;

// The n-th bridge with a streaming audio out over Wi-Fi: its slot and board id, or -1.
static int aud_find(int n, uint8_t *board_id)
{
    for (int i = 0; i < FH1_BRIDGES_KNOWN; i++) {
        br_t *b = &s_br[i];
        if (!b->known || b->link != FH1_BRIDGE_LINK_WIFI) continue;
        for (int k = 0; k < b->v.nboard; k++) {
            const fh1b_board_dev_t *d = &b->v.board[k];
            if (d->cls == FH1B_CLASS_AUDIO_OUT && (d->flags & FH1B_BOARD_STREAMS) && n-- == 0) {
                *board_id = d->id;
                return i;
            }
        }
    }
    return -1;
}

static int aud_index(fh1_device_t *d) { return d->id[6] == '1' ? 1 : 0; }     // "baudio0" / "baudio1"

static void aud_send(aud_t *a, uint32_t rate)
{
    static uint8_t pkt[FH1B_UDP_HDR + sizeof(fh1b_audio_hdr_t) + FH1B_AUDIO_PCM_SAMPLES * 2];
    const br_t *b = a->slot >= 0 ? &s_br[a->slot] : NULL;
    if (!b || b->link != FH1_BRIDGE_LINK_WIFI || s_sock < 0) return;
    fh1b_audio_hdr_t h = { .id = a->board_id, .codec = a->stereo ? FH1B_AUDIO_PCM16_STEREO : FH1B_AUDIO_PCM16, .seq = a->seq++,
                           .rate = (uint16_t)rate, .samples = (uint16_t)a->fill, .volume = a->volume };
    const size_t bytes = (size_t)a->fill * (a->stereo ? 4 : 2);
    pkt[0] = 'F';
    pkt[1] = '1';
    pkt[2] = FH1B_UDP_AUDIO;
    memcpy(pkt + FH1B_UDP_HDR, &h, sizeof(h));
    memcpy(pkt + FH1B_UDP_HDR + sizeof(h), a->pcm, bytes);          // little-endian already
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(FH1B_UDP_PORT), .sin_addr.s_addr = b->ip };
    const int64_t t0 = esp_timer_get_time();
    if (sendto(s_sock, pkt, FH1B_UDP_HDR + sizeof(h) + bytes, 0, (struct sockaddr *)&to, sizeof(to)) < 0) s_aud_send_fails++;
    const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
    s_aud_sent++;
    if (us > s_aud_send_max_us) s_aud_send_max_us = us;
    s_aud_send_us += us;
    // Every 5 s while sending: what went out, and how long sending took.
    static int64_t at;
    if (t0 - at > 5000000) {
        if (at) ESP_LOGI(TAG, "sound to %s: %.1f packets/s, %lu refused, sendto %lu us average, %lu us worst; app frames/s %.0f, paced %lu ms",
                         b->alias, s_aud_sent * 1e6f / (float)(t0 - at), (unsigned long)s_aud_send_fails,
                         (unsigned long)(s_aud_sent ? s_aud_send_us / s_aud_sent : 0), (unsigned long)s_aud_send_max_us,
                         s_aud_in * 1e6f / (float)(t0 - at), (unsigned long)s_aud_wait_ms);
        at = t0;
        s_aud_sent = s_aud_send_fails = s_aud_send_max_us = s_aud_in = s_aud_wait_ms = 0;
        s_aud_send_us = 0;
    }
    a->fill = 0;
}

static uint8_t aud_quality(void)
{
    // 24 kHz stereo by default: 48 kHz stereo lost 25-30 % of its packets on the A1S (its radio shared with
    // Bluetooth took ~100 packets/s of the 144 needed); 24 kHz stereo arrived whole.
    if (s_aud_quality == 0xFF) s_aud_quality = fh1_settings_get_u8("audio", "bridge_q", FH1_BRIDGE_AUDIO_24K_STEREO);
    return s_aud_quality < FH1_BRIDGE_AUDIO_QUALITIES ? s_aud_quality : FH1_BRIDGE_AUDIO_24K_STEREO;
}

// Pace as a codec's FIFO would, so the app plays in real time.
static void aud_pace(aud_t *a, size_t out, uint32_t out_rate)
{
    const int64_t now = esp_timer_get_time();
    if (a->clock_us < now - 100000) a->clock_us = now;          // starting, or after a pause
    a->clock_us += (int64_t)out * 1000000 / out_rate;
    // Whole ticks only, rounded down: rounding up (a 10 ms tick for every 5 ms written) sent at half speed,
    // and the bridge ran dry all the time (heard as crackle).
    const int64_t ahead_ms = (a->clock_us - now) / 1000 - AUD_AHEAD_MS;
    if (ahead_ms >= portTICK_PERIOD_MS) {
        s_aud_wait_ms += (uint32_t)ahead_ms;
        vTaskDelay((TickType_t)(ahead_ms / portTICK_PERIOD_MS));
    }
}

static int16_t aud_filter(const int16_t *lp, const int16_t *hist, uint8_t pos)
{
    int32_t acc = 0;
    for (int k = 0; k < 31; k++) acc += (int32_t)lp[k] * hist[(pos - k) & 31];
    acc >>= 15;
    return (int16_t)(acc > 32767 ? 32767 : acc < -32768 ? -32768 : acc);
}

static int aud_write(fh1_device_t *d, const int16_t *frames, size_t count, uint32_t rate);

static int aud_write_stereo(fh1_device_t *d, const int16_t *lr, size_t count, uint32_t rate)
{
    aud_t *a = &s_aud[aud_index(d)];
    if (a->slot < 0 || s_br[a->slot].link != FH1_BRIDGE_LINK_WIFI || !rate) return FH1_UNAVAILABLE;
    s_aud_in += count;
    const uint8_t q = aud_quality();
    if (q == FH1_BRIDGE_AUDIO_24K_MONO) {
        // Mixed down here, through the mono path.
        int16_t m[128];
        for (size_t i = 0; i < count;) {
            size_t k = count - i > 128 ? 128 : count - i;
            for (size_t j = 0; j < k; j++) m[j] = (int16_t)(((int32_t)lr[2 * (i + j)] + lr[2 * (i + j) + 1]) / 2);
            int r = aud_write(d, m, k, rate);
            if (r < 0) return r;
            i += k;
        }
        return (int)count;
    }
    const uint32_t max = q == FH1_BRIDGE_AUDIO_48K_STEREO ? 48000 : 24000;
    const int f = rate > 2 * max ? 4 : rate > max ? 2 : 1;
    const uint32_t out_rate = rate / f;
    if (!a->stereo && a->fill) aud_send(a, out_rate);              // what mono was queued goes first
    a->stereo = true;
    const int16_t *lp = f == 4 ? s_lp4 : s_lp2;
    size_t out = 0;
    for (size_t i = 0; i < count; i++) {
        a->hpos = (uint8_t)((a->hpos + 1) & 31);
        a->hist[a->hpos] = lr[2 * i];
        a->hist_r[a->hpos] = lr[2 * i + 1];
        if (++a->acc_n < f) continue;
        a->acc_n = 0;
        a->pcm[2 * a->fill] = f > 1 ? aud_filter(lp, a->hist, a->hpos) : lr[2 * i];
        a->pcm[2 * a->fill + 1] = f > 1 ? aud_filter(lp, a->hist_r, a->hpos) : lr[2 * i + 1];
        a->fill++;
        out++;
        if (a->fill == FH1B_AUDIO_STEREO_FRAMES) aud_send(a, out_rate);
    }
    aud_pace(a, out, out_rate);
    return (int)count;
}

static int aud_write(fh1_device_t *d, const int16_t *frames, size_t count, uint32_t rate)
{
    aud_t *a = &s_aud[aud_index(d)];
    if (a->slot < 0 || s_br[a->slot].link != FH1_BRIDGE_LINK_WIFI || !rate) return FH1_UNAVAILABLE;
    const uint32_t max = aud_quality() == FH1_BRIDGE_AUDIO_48K_STEREO ? 48000 : 24000;
    const int f = rate > 2 * max ? 4 : rate > max ? 2 : 1;
    const uint32_t out_rate = rate / f;
    if (a->stereo && a->fill) aud_send(a, out_rate);               // what stereo was queued goes first
    a->stereo = false;
    size_t out = 0;
    const int16_t *lp = f == 4 ? s_lp4 : s_lp2;
    for (size_t i = 0; i < count; i++) {
        a->hist[a->hpos = (uint8_t)((a->hpos + 1) & 31)] = frames[i];
        if (++a->acc_n < f) continue;
        a->acc_n = 0;
        int16_t y = frames[i];
        if (f > 1) y = aud_filter(lp, a->hist, a->hpos);
        a->pcm[a->fill++] = y;
        out++;
        if (a->fill == FH1B_AUDIO_PCM_SAMPLES) aud_send(a, out_rate);
    }
    aud_pace(a, out, out_rate);
    return (int)count;
}

static void aud_volume(fh1_device_t *d, uint8_t pct) { s_aud[aud_index(d)].volume = pct; }

static const fh1_audio_ops_t s_aud_ops = { .write = aud_write, .set_volume = aud_volume, .write_stereo = aud_write_stereo };

uint8_t fh1_bridge_audio_quality(void) { return aud_quality(); }
void fh1_bridge_set_audio_quality(uint8_t q)
{
    if (q >= FH1_BRIDGE_AUDIO_QUALITIES) return;
    s_aud_quality = q;
    fh1_settings_set_u8("audio", "bridge_q", q);
}

static void aud_update(fh1_device_t *d)
{
    aud_t *a = &s_aud[aud_index(d)];
    uint8_t id = 0;
    const int slot = aud_find(aud_index(d), &id);
    a->board_id = id;
    if (slot != a->slot) {
        a->slot = slot;
        a->fill = 0;
        a->enc = (fh1_adpcm_t){ 0 };
    }
    if (slot >= 0) {
        char name[17] = "";
        for (int k = 0; k < s_br[slot].v.nboard; k++) {
            if (s_br[slot].v.board[k].id == id) memcpy(name, s_br[slot].v.board[k].name, 16);
        }
        snprintf(d->info, sizeof(d->info), "%s: %s speaker", s_br[slot].alias, name);
        if (d->state != FH1_DEV_READY) fh1_dev_report(d, FH1_DEV_READY, "");
    } else {
        snprintf(d->info, sizeof(d->info), "no bridge with sound on Wi-Fi");
        if (d->state == FH1_DEV_READY) fh1_dev_report(d, FH1_DEV_ABSENT, "no bridge with sound");
    }
}

static esp_err_t aud_probe(fh1_device_t *d)
{
    aud_update(d);
    return s_aud[aud_index(d)].slot >= 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static void aud_poll(fh1_device_t *d)
{
    aud_update(d);
    if (d->state == FH1_DEV_ABSENT && s_aud[aud_index(d)].slot >= 0) fh1_dev_report(d, FH1_DEV_READY, "");
}

const char *fh1_bridge_audio_device(int i)
{
    const int k = slot_of_view(i);
    for (int n = 0; k >= 0 && n < AUD_DEVS; n++) {
        if (s_aud[n].slot == k) return n ? "baudio1" : "baudio0";
    }
    return NULL;
}

const fh1_driver_t fh1_bridge_audio_driver = {
    .name = "bridge-audio",
    .probe = aud_probe,
    .poll = aud_poll,
    .ops = &s_aud_ops,
};

// ── For the interface ──

bool fh1_bridge_on(void) { return s_running; }

int fh1_bridge_count(void)
{
    table_init();
    return known_count();
}

bool fh1_bridge_get(int i, fh1_bridge_view_t *out)
{
    int k = slot_of_view(i);
    if (k < 0) return false;
    portENTER_CRITICAL(&s_mux);
    *out = s_br[k].v;
    memcpy(out->alias, s_br[k].alias, sizeof(out->alias));
    out->link = s_br[k].link;
    if (out->link == FH1_BRIDGE_LINK_BLE && !s_running) out->link = FH1_BRIDGE_LINK_NONE;
    out->connected = out->connected && out->link != FH1_BRIDGE_LINK_NONE;
    const uint32_t ip = s_br[k].ip;
    memcpy(out->ip, &ip, 4);
    portEXIT_CRITICAL(&s_mux);
    for (int j = 0; j < 6; j++) out->addr[j] = s_br[k].addr.val[5 - j];
    return true;
}

void fh1_bridge_rename(int i, const char *alias)
{
    int k = slot_of_view(i);
    if (k < 0 || !alias || !alias[0]) return;
    portENTER_CRITICAL(&s_mux);
    snprintf(s_br[k].alias, sizeof(s_br[k].alias), "%s", alias);
    memcpy(s_br[k].v.alias, s_br[k].alias, sizeof(s_br[k].alias));
    portEXIT_CRITICAL(&s_mux);
    save();
}

void fh1_bridge_forget(int i)
{
    int k = slot_of_view(i);
    if (k < 0) return;
    br_t *b = &s_br[k];
    if (b->link == FH1_BRIDGE_LINK_BLE && s_running && b->conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(b->conn, BLE_ERR_REM_USER_CONN_TERM);
    if (b->link == FH1_BRIDGE_LINK_WIFI) udp_send(b->ip, FH1B_UDP_BYE, NULL, 0);
    reset_live(b);
    b->known = false;
    b->beacon_ms = 0;
    save();
}

void fh1_bridge_search(int seconds)
{
    s_search_until = esp_timer_get_time() + (int64_t)seconds * 1000000;
    if (s_running) scan_update();
}

bool fh1_bridge_searching(void) { return (s_running || s_udp_on) && esp_timer_get_time() < s_search_until; }

bool fh1_bridge_wifi_link_on(void) { return s_udp_on; }
