// The bridge's Bluepad32 platform and its BLE service for the FH1 (protocol: fh1_bridge_proto.h; plan and
// measurements: firmware/PLAN.md, "Extension board").
//
// Controllers (Bluetooth Classic) are held by Bluepad32; their latest state per slot is kept here. The FH1
// connects over BLE as a central to our GATT service (fh1_bridge.gatt): it reads info and devices, and gets
// the input notified at a fixed rate (the newest state of every controller, not each of their ~400
// reports a second). Commands: pair, forget, rumble, light bar, LEDs, rate, ping. With Wi-Fi on, the same
// messages also go over UDP to an FH1 on the same network (udp.c), and the FH1 can leave its Bluetooth off.
//
// Scanning for new controllers runs at start for 30 s, on a pair command, and stops as soon as one is
// ready: an inquiry takes the radio from the connected controllers (B0 measured ~24 ms gaps every second
// while it ran). Paired controllers come back by themselves (keys in NVS; a press of PS).
//
// Everything here runs on BTstack's run loop: Bluepad32's callbacks, the ATT server's, the timers.
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include <btstack.h>
#include <uni.h>
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "esp_image_format.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "fh1_bridge_proto.h"
#include "parser/uni_hid_parser_ds5.h"
#include "fh1_bridge.gatt.h"
#include "bridge_wifi.h"
#include "bridge_udp.h"
#include "board.h"
#include "audio_rx.h"
#include "bridge_ota.h"

#define H_INFO ATT_CHARACTERISTIC_F1B10001_5A1E_4C6B_9E2A_6F48315A0001_01_VALUE_HANDLE
#define H_DEVICES ATT_CHARACTERISTIC_F1B10002_5A1E_4C6B_9E2A_6F48315A0001_01_VALUE_HANDLE
#define H_DEVICES_CCC ATT_CHARACTERISTIC_F1B10002_5A1E_4C6B_9E2A_6F48315A0001_01_CLIENT_CONFIGURATION_HANDLE
#define H_INPUT ATT_CHARACTERISTIC_F1B10003_5A1E_4C6B_9E2A_6F48315A0001_01_VALUE_HANDLE
#define H_INPUT_CCC ATT_CHARACTERISTIC_F1B10003_5A1E_4C6B_9E2A_6F48315A0001_01_CLIENT_CONFIGURATION_HANDLE
#define H_COMMAND ATT_CHARACTERISTIC_F1B10004_5A1E_4C6B_9E2A_6F48315A0001_01_VALUE_HANDLE
#define H_EVENT ATT_CHARACTERISTIC_F1B10005_5A1E_4C6B_9E2A_6F48315A0001_01_VALUE_HANDLE
#define H_EVENT_CCC ATT_CHARACTERISTIC_F1B10005_5A1E_4C6B_9E2A_6F48315A0001_01_CLIENT_CONFIGURATION_HANDLE

#define SLOTS CONFIG_BLUEPAD32_MAX_DEVICES
_Static_assert(SLOTS <= FH1B_MAX_DEVICES, "more Bluepad32 slots than the protocol carries");

// ── Controllers ──

typedef struct {
    bool ready;
    uni_hid_device_t *d;
    fh1b_device_t info;
    fh1b_pad_t pad;
    uint32_t reports;                  // received in this stats window
} slot_t;

static slot_t s_slot[SLOTS];
static bool s_devices_changed;         // to notify over BLE
static bool s_udp_devices;             // to send over Wi-Fi
static bool s_scanning;
static btstack_timer_source_t s_scan_timer;

// ── The link ──

static hci_con_handle_t s_con = HCI_CON_HANDLE_INVALID;
static bool s_notify_input, s_notify_devices, s_notify_events;
static uint16_t s_rate_hz = 125;
static uint8_t s_seq;
static btstack_timer_source_t s_tick;
static uint32_t s_sent;                // input notifications in this stats window
static uint8_t s_udp_seq;
static uint32_t s_udp_sent;            // input datagrams in this stats window
static btstack_packet_callback_registration_t s_hci_cb;

// Events waiting to be notified (pongs, pairing); small ring.
#define EVQ 16                         // a device's controls come as several at once
static struct {
    uint8_t len;
    uint8_t data[176];                 // the largest: the board's devices (2 + 8 x 20 bytes)
} s_evq[EVQ];
static int s_evq_head, s_evq_count;

static void advertise_rate(bool slow);

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void request_send(void)
{
    if (s_con != HCI_CON_HANDLE_INVALID) att_server_request_can_send_now_event(s_con);
}

static void devices_changed(void)
{
    s_devices_changed = s_udp_devices = true;
    request_send();
}

static void event_push(const uint8_t *data, uint8_t len)
{
    bridge_udp_send(FH1B_CHR_EVENT, data, len);             // when an FH1 is linked over Wi-Fi
    if (!s_notify_events || s_evq_count == EVQ || len > sizeof(s_evq[0].data)) return;
    int i = (s_evq_head + s_evq_count++) % EVQ;
    memcpy(s_evq[i].data, data, len);
    s_evq[i].len = len;
    request_send();
}

static void scan(bool on, int seconds)
{
    btstack_run_loop_remove_timer(&s_scan_timer);
    if (on) {
        uni_bt_start_scanning_and_autoconnect_unsafe();
        btstack_run_loop_set_timer(&s_scan_timer, seconds * 1000u);
        btstack_run_loop_add_timer(&s_scan_timer);
    } else if (s_scanning) {
        uni_bt_stop_scanning_unsafe();
    }
    if (on != s_scanning) {
        s_scanning = on;
        bridge_wifi_pause(on);
        logi("bridge: scanning %s\n", on ? "on" : "off");
        uint8_t ev[2] = { FH1B_EVT_PAIRING, on };
        event_push(ev, sizeof(ev));
    }
}

static void scan_timeout(btstack_timer_source_t *t)
{
    ARG_UNUSED(t);
    scan(false, 0);
}

// The bridge's own state, for the FH1's About and Storage pages of it.
static void send_status(void)
{
    uint8_t ev[1 + sizeof(fh1b_status_t)] = { FH1B_EVT_STATUS };
    fh1b_status_t st = { 0 };
    st.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    st.heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    st.heap_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    st.cpu_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    st.flash_mb = (uint8_t)(flash >> 20);
    btstack_link_key_iterator_t it;
    if (gap_link_key_iterator_init(&it)) {
        bd_addr_t a;
        link_key_t k;
        link_key_type_t t;
        while (gap_link_key_iterator_get_next(&it, a, k, &t)) st.paired++;
        gap_link_key_iterator_done(&it);
    }
    nvs_stats_t ns;
    if (nvs_get_stats(NULL, &ns) == ESP_OK) {
        st.nvs_used = (uint16_t)ns.used_entries;
        st.nvs_total = (uint16_t)ns.total_entries;
    }
    const esp_partition_t *part = esp_ota_get_running_partition();
    if (part) {
        st.app_part_kb = part->size / 1024;
        esp_image_metadata_t md;
        const esp_partition_pos_t pos = { .offset = part->address, .size = part->size };
        if (esp_image_get_metadata(&pos, &md) == ESP_OK) st.app_kb = md.image_len / 1024;
    }
    bd_addr_t me;
    gap_local_bd_addr(me);
    memcpy(st.bt_addr, me, 6);
    st.heap_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    st.encrypted = s_con != HCI_CON_HANDLE_INVALID && gap_encryption_key_size(s_con) > 0;
    st.ota_code = bridge_ota_code();
    bridge_wifi_status(&st);
    memcpy(ev + 1, &st, sizeof(st));
    event_push(ev, sizeof(ev));
}

// ── The board's own devices (board.h): listed for the FH1, keys sent as events when they change ──

enum { BD_OUT = BOARD_DEV_OUT, BD_IN = BOARD_DEV_IN, BD_KEYS = BOARD_DEV_KEYS, BD_LIGHT = BOARD_DEV_LIGHT, BD_SD = BOARD_DEV_SD };
static bool s_led;
static uint32_t s_keys_sent;
static bool s_hp_sent;

static void send_keys(void)
{
    uint8_t ev[6] = { FH1B_EVT_KEYS, BD_KEYS };
    memcpy(ev + 2, &s_keys_sent, 4);
    event_push(ev, sizeof(ev));
}

static void send_board(void)
{
    uint8_t ev[2 + FH1B_BOARD_MAX * sizeof(fh1b_board_dev_t)] = { FH1B_EVT_BOARD, 0 };
    fh1b_board_dev_t *d = (fh1b_board_dev_t *)(ev + 2);
    int n = 0;
    const uint8_t codec = board_codec();
    if (codec) {
        const char *name = codec == 0x10 ? "ES8388" : "AC101";
        d[n] = (fh1b_board_dev_t){ .id = BD_OUT, .cls = FH1B_CLASS_AUDIO_OUT, .count = 2,
                                   .flags = FH1B_BOARD_STREAMS | (board_headphones() ? FH1B_BOARD_PLUGGED : 0) |
                                            (audio_rx_playing() ? FH1B_BOARD_PLAYING : 0) };
        strncpy(d[n++].name, name, sizeof(d[0].name));
        d[n] = (fh1b_board_dev_t){ .id = BD_IN, .cls = FH1B_CLASS_AUDIO_IN, .count = 2 };
        strncpy(d[n++].name, name, sizeof(d[0].name));
    }
    if (board_key_count()) {
        d[n] = (fh1b_board_dev_t){ .id = BD_KEYS, .cls = FH1B_CLASS_KEYS, .count = (uint8_t)board_key_count() };
        strncpy(d[n++].name, "keys", sizeof(d[0].name));
        d[n] = (fh1b_board_dev_t){ .id = BD_LIGHT, .cls = FH1B_CLASS_LIGHT, .count = s_led };
        strncpy(d[n++].name, "LED", sizeof(d[0].name));
    }
    if (board_has_sd()) {
        const uint32_t mb = board_sd_mb();
        d[n] = (fh1b_board_dev_t){ .id = BD_SD, .cls = FH1B_CLASS_STORAGE, .flags = mb ? FH1B_BOARD_PLUGGED : 0,
                                   .count = (uint8_t)(mb / 1024 > 255 ? 255 : (mb + 512) / 1024) };
        snprintf(d[n++].name, sizeof(d[0].name), mb ? "SD %lu MB" : "SD", (unsigned long)mb);
    }
    ev[1] = (uint8_t)n;
    event_push(ev, (uint8_t)(2 + n * sizeof(fh1b_board_dev_t)));
    if (board_key_count()) send_keys();
}

// From the tick, every 20 ms: keys and the headphone jack.
static void board_poll(void)
{
    static uint32_t at;
    const uint32_t t = now_ms();
    if (t - at < 20) return;
    at = t;
    const uint32_t k = board_keys();
    if (k != s_keys_sent) {
        s_keys_sent = k;
        send_keys();
    }
    // The headphone jack, the SD card and whether sound is playing: the list again when one changes.
    static uint32_t sd_sent;
    static bool play_sent;
    const bool hp = board_headphones(), play = audio_rx_playing();
    const uint32_t sd = board_sd_mb();
    if (hp != s_hp_sent || sd != sd_sent || play != play_sent) {
        s_hp_sent = hp;
        sd_sent = sd;
        play_sent = play;
        send_board();
    }
}

static size_t devices_build(fh1b_devices_t *out)
{
    out->count = 0;
    for (int i = 0; i < SLOTS; i++) {
        if (s_slot[i].ready) out->dev[out->count++] = s_slot[i].info;
    }
    return 1 + out->count * sizeof(fh1b_device_t);
}

static void info_build(fh1b_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->proto = FH1B_PROTO_VERSION;
    info->max_devices = SLOTS;
    info->rate_hz = s_rate_hz;
    info->caps = FH1B_CAP_GAMEPAD | FH1B_CAP_MOTION | FH1B_CAP_RUMBLE | FH1B_CAP_LIGHTBAR | FH1B_CAP_WIFI_LINK |
                 FH1B_CAP_TRIGGERS | FH1B_CAP_LEDS;
    strncpy(info->board, BRIDGE_BOARD_NAME, sizeof(info->board));
    strncpy(info->firmware, "bridge-b11", sizeof(info->firmware));
}

static void beacon_update(void)
{
    fh1b_beacon_t b;
    bd_addr_t me;
    gap_local_bd_addr(me);
    memcpy(b.addr, me, 6);
    info_build(&b.info);
    bridge_udp_set_beacon(&b);
}

static bool s_udp_motion;              // the FH1 wants motion over Wi-Fi (FH1B_CMD_MOTION)

// A change worth sending: buttons, D-pad, or a stick or trigger moved by more than a little; motion too when
// the FH1 asked for it (otherwise it rides along).
static bool input_changed(const fh1b_pad_t *a, const fh1b_pad_t *b)
{
    if (a->id != b->id || a->dpad != b->dpad || a->buttons != b->buttons || a->misc != b->misc) return true;
    const int d = 6;
    if (abs(a->lx - b->lx) > d || abs(a->ly - b->ly) > d || abs(a->rx - b->rx) > d || abs(a->ry - b->ry) > d ||
        abs(a->l2 - b->l2) > 2 * d || abs(a->r2 - b->r2) > 2 * d)
        return true;
    if (!s_udp_motion) return false;
    for (int i = 0; i < 3; i++) {
        if (abs(a->gyro[i] - b->gyro[i]) > 50 || abs(a->accel[i] - b->accel[i]) > 30) return true;   // 0.05 rad/s, 0.3 m/s2
    }
    return false;
}

// Over Wi-Fi: the devices when they changed, then the input. The radio is shared with the controllers
// (Bluetooth Classic), and coexistence leaves Wi-Fi about 50 small datagrams a second on the WROOM
// (measured; more fill its queue and the delay grows to a second). So the input goes when it changes, at
// most every UDP_MIN_MS (UDP_MOTION_MS while motion counts: it changes all the time), and every UDP_IDLE_MS
// regardless (the FH1's link check, and motion).
#define UDP_MIN_MS 16
#define UDP_MOTION_MS 25
#define UDP_IDLE_MS 100                // no controllers
#define UDP_PADS_IDLE_MS 50            // with controllers: a lost datagram costs at most this
// A button or D-pad change is sent twice (UDP_MIN_MS apart): ~2-3 % of datagrams are lost over this Wi-Fi,
// and a lost press would otherwise wait for the next change or heartbeat.
static bool buttons_changed(const fh1b_pad_t *a, const fh1b_pad_t *b)
{
    return a->id != b->id || a->dpad != b->dpad || a->buttons != b->buttons || a->misc != b->misc;
}
static void send_udp(void)
{
    static fh1b_input_t last;
    static uint32_t last_ms;
    static bool repeat;
    if (!bridge_udp_linked()) return;
    const uint32_t t = now_ms();
    if (t - last_ms < (s_udp_motion ? UDP_MOTION_MS : UDP_MIN_MS)) return;
    if (s_udp_devices) {
        fh1b_devices_t dv;
        size_t len = devices_build(&dv);
        if (bridge_udp_send(FH1B_CHR_DEVICES, &dv, (uint16_t)len)) s_udp_devices = false;
    }
    fh1b_input_t in;
    in.count = 0;
    for (int i = 0; i < SLOTS; i++) {
        if (s_slot[i].ready) in.pad[in.count++] = s_slot[i].pad;
    }
    bool changed = in.count != last.count || repeat || t - last_ms >= (in.count ? UDP_PADS_IDLE_MS : UDP_IDLE_MS);
    bool pressed = in.count != last.count;
    for (int i = 0; i < in.count; i++) {
        if (buttons_changed(&in.pad[i], &last.pad[i])) pressed = true;
        if (!changed) changed = input_changed(&in.pad[i], &last.pad[i]);
    }
    if (!changed) return;
    in.seq = s_udp_seq;
    in.time_ms = t;
    if (bridge_udp_send(FH1B_CHR_INPUT, &in, FH1B_INPUT_SIZE(in.count))) {
        s_udp_seq++;                   // the FH1 counts gaps as losses: only what went out counts
        s_udp_sent++;
        repeat = pressed;              // once more, next time (a repeat changes nothing, so it ends there)
        last = in;
        last_ms = t;
    }
}

// An FH1 said hello over Wi-Fi: what it reads first over BLE.
static void udp_hello(void)
{
    fh1b_info_t info;
    info_build(&info);
    bridge_udp_send(FH1B_CHR_INFO, &info, sizeof(info));
    s_udp_devices = true;
    s_udp_motion = false;              // a new link: the FH1 asks again if it wants it
}

// The newest state of every ready controller, notified at the set rate.
static void send_input(void)
{
    fh1b_input_t in;
    in.seq = s_seq++;
    in.time_ms = now_ms();
    in.count = 0;
    for (int i = 0; i < SLOTS; i++) {
        if (s_slot[i].ready) in.pad[in.count++] = s_slot[i].pad;
    }
    uint16_t len = FH1B_INPUT_SIZE(in.count);
    if (len + 3 > att_server_get_mtu(s_con)) len = att_server_get_mtu(s_con) - 3;   // never: the FH1 asks for 185
    if (att_server_notify(s_con, H_INPUT, (const uint8_t *)&in, len) == ERROR_CODE_SUCCESS) s_sent++;
}

// One message per chance to send: events first, then a changed device list, then the input.
static void on_can_send(void)
{
    if (s_con == HCI_CON_HANDLE_INVALID) return;
    if (s_evq_count) {
        att_server_notify(s_con, H_EVENT, s_evq[s_evq_head].data, s_evq[s_evq_head].len);
        s_evq_head = (s_evq_head + 1) % EVQ;
        s_evq_count--;
    } else if (s_devices_changed && s_notify_devices) {
        fh1b_devices_t dv;
        size_t len = devices_build(&dv);
        att_server_notify(s_con, H_DEVICES, (const uint8_t *)&dv, (uint16_t)len);
        s_devices_changed = false;
    } else if (s_notify_input) {
        send_input();
        return;
    }
    if (s_evq_count || (s_devices_changed && s_notify_devices)) request_send();
}

// The run loop's watchdog: the A1S once went silent on every link with two controllers on it (no crash,
// no restart). If the loop stops ticking for RUNLOOP_STALL_MS, abort: the core dump keeps every task's
// backtrace (idf.py coredump-info) and the bridge restarts.
#define RUNLOOP_STALL_MS 5000
static volatile uint32_t s_tick_ms;
static void runloop_watch(void *arg)
{
    const uint32_t t = now_ms(), last = s_tick_ms;
    if (last && t - last > RUNLOOP_STALL_MS) {
        ESP_LOGE("bridge", "run loop stalled for %lu ms: aborting for a core dump", (unsigned long)(t - last));
        abort();
    }
}

static void tick(btstack_timer_source_t *t)
{
    s_tick_ms = now_ms();
    btstack_run_loop_set_timer(t, 1000u / s_rate_hz);
    btstack_run_loop_add_timer(t);
    if (s_notify_input) request_send();
    send_udp();
    board_poll();
    advertise_rate(bridge_udp_linked() && s_con == HCI_CON_HANDLE_INVALID);

    // Every 5 s: what came in and what went out.
    static uint32_t window;
    uint32_t now = now_ms();
    if (!window) window = now;
    if (now - window >= 5000) {
        float secs = (now - window) / 1000.0f;
        char line[160];
        int n = snprintf(line, sizeof(line), "bridge: BLE %s %.0f/s, Wi-Fi %s %.0f/s;", s_con != HCI_CON_HANDLE_INVALID ? "up" : "down",
                         s_sent / secs, bridge_udp_linked() ? "up" : "down", s_udp_sent / secs);
        uint32_t fails;
        int err;
        bridge_udp_fails(&fails, &err);
        if (fails) n += snprintf(line + n, sizeof(line) - n, " %lu failed (errno %d);", (unsigned long)fails, err);
        n += snprintf(line + n, sizeof(line) - n, " heap %u KB (min %u);", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                      (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));
        for (int i = 0; i < SLOTS; i++) {
            if (s_slot[i].ready) n += snprintf(line + n, sizeof(line) - n, " pad%d %.0f/s", i, s_slot[i].reports / secs);
            s_slot[i].reports = 0;
        }
        logi("%s\n", line);
        s_sent = s_udp_sent = 0;
        window = now;
    }
}

// ── ATT server ──

static uint16_t att_read(hci_con_handle_t con, uint16_t handle, uint16_t offset, uint8_t *buffer, uint16_t size)
{
    ARG_UNUSED(con);
    if (handle == H_INFO) {
        fh1b_info_t info;
        info_build(&info);
        return att_read_callback_handle_blob((const uint8_t *)&info, sizeof(info), offset, buffer, size);
    }
    if (handle == H_DEVICES) {
        fh1b_devices_t dv;
        size_t len = devices_build(&dv);
        return att_read_callback_handle_blob((const uint8_t *)&dv, (uint16_t)len, offset, buffer, size);
    }
    return 0;
}

// encrypted: it came over an encrypted BLE link (secrets are taken only then).
static void command(const uint8_t *b, uint16_t n, bool encrypted)
{
    if (!n) return;
    const uint8_t id = n > 1 ? b[1] : 0xFF;
    uni_hid_device_t *d = id < SLOTS && s_slot[id].ready ? s_slot[id].d : NULL;
    switch (b[0]) {
    case FH1B_CMD_PAIR: scan(n > 1 && b[1], n > 1 ? b[1] : 0); break;
    case FH1B_CMD_FORGET_ALL:
        for (int i = 0; i < SLOTS; i++) {
            if (s_slot[i].ready) uni_hid_device_disconnect(s_slot[i].d);
        }
        uni_bt_del_keys_unsafe();
        logi("bridge: controllers forgotten\n");
        break;
    case FH1B_CMD_RUMBLE:
        if (d && n >= 6 && d->report_parser.play_dual_rumble)
            d->report_parser.play_dual_rumble(d, 0, (uint16_t)(b[4] | b[5] << 8), b[2], b[3]);
        break;
    case FH1B_CMD_LIGHTBAR:
        if (d && n >= 5 && d->report_parser.set_lightbar_color) {
            d->report_parser.set_lightbar_color(d, b[2], b[3], b[4]);
            memcpy(s_slot[id].info.rgb, b + 2, 3);
            devices_changed();
        }
        break;
    case FH1B_CMD_LEDS:
        if (d && n >= 3 && d->report_parser.set_player_leds) d->report_parser.set_player_leds(d, b[2]);
        break;
    case FH1B_CMD_RATE:
        if (n >= 3) {
            uint16_t hz = (uint16_t)(b[1] | b[2] << 8);
            s_rate_hz = hz < 25 ? 25 : hz > 250 ? 250 : hz;
            logi("bridge: input rate %u Hz\n", s_rate_hz);
            beacon_update();
        }
        break;
    case FH1B_CMD_STATUS: send_status(); break;
    case FH1B_CMD_BOARD: send_board(); break;
    case FH1B_CMD_CTRLS:
    case FH1B_CMD_CTRL_SET: {
        // A device's controls: described (all), or one set and described again.
        if (n < 2) break;
        const uint8_t dev = b[1];
        if (b[0] == FH1B_CMD_CTRL_SET && n >= 5) board_ctrl_set(dev, b[2], (int16_t)(b[3] | b[4] << 8));
        uint8_t ev[1 + sizeof(fh1b_ctrl_t)] = { FH1B_EVT_CTRL };
        for (int i = 0; i < board_ctrl_count(dev); i++) {
            if (b[0] == FH1B_CMD_CTRL_SET && i != b[2]) continue;
            fh1b_ctrl_t c;
            if (!board_ctrl_get(dev, i, &c)) continue;
            memcpy(ev + 1, &c, sizeof(c));
            event_push(ev, sizeof(ev));
        }
        break;
    }
    case FH1B_CMD_BOARD_SET:
        if (n >= 3 && b[1] == BD_LIGHT) {
            s_led = b[2];
            board_led(s_led);
            send_board();
        }
        break;
    case FH1B_CMD_MOTION:
        if (n >= 2) s_udp_motion = b[1];
        break;
    case FH1B_CMD_TRIGGER:
        // DualSense only (Bluepad32's experimental API).
        if (d && n >= 7 && d->controller_type == CONTROLLER_TYPE_PS5Controller) {
            ds5_adaptive_trigger_effect_t e;
            switch (b[3]) {
            case FH1B_TRIG_FEEDBACK: e = ds5_new_adaptive_trigger_effect_feedback(b[4], b[5]); break;
            case FH1B_TRIG_WEAPON: e = ds5_new_adaptive_trigger_effect_weapon(b[4], b[5], b[6]); break;
            case FH1B_TRIG_VIBRATION: e = ds5_new_adaptive_trigger_effect_vibration(b[4], b[5], b[6]); break;
            default: e = ds5_new_adaptive_trigger_effect_off(); break;
            }
            ds5_set_adaptive_trigger_effect(d, b[2] ? UNI_ADAPTIVE_TRIGGER_TYPE_RIGHT : UNI_ADAPTIVE_TRIGGER_TYPE_LEFT, &e);
        }
        break;
    case FH1B_CMD_WIFI_SET: {
        // ssid and password, each after its length; only over an encrypted link (they are secrets).
        if (!encrypted) {
            logi("bridge: Wi-Fi credentials refused: the link is not encrypted\n");
            break;
        }
        if (n < 3) break;
        uint8_t ls = b[1];
        if (ls > 32 || 2 + ls >= n) break;
        uint8_t lp = b[2 + ls];
        if (lp > 64 || 3 + ls + lp > n) break;
        char ssid[33], pass[65];
        memcpy(ssid, b + 2, ls);
        ssid[ls] = 0;
        memcpy(pass, b + 3 + ls, lp);
        pass[lp] = 0;
        bridge_wifi_set(ssid, pass);
        logi("bridge: Wi-Fi network %s saved\n", ssid);
        send_status();
        break;
    }
    case FH1B_CMD_WIFI_ENABLE:
        if (n >= 2) bridge_wifi_enable(b[1]);
        send_status();
        break;
    case FH1B_CMD_WIFI_FORGET:
        bridge_wifi_forget();
        send_status();
        break;
    case FH1B_CMD_PING:
        if (n >= 5) {
            uint8_t ev[6] = { FH1B_EVT_PONG, b[1], b[2], b[3], b[4], (uint8_t)bridge_wifi_rssi() };
            event_push(ev, sizeof(ev));
        }
        break;
    }
}

static int att_write(hci_con_handle_t con, uint16_t handle, uint16_t mode, uint16_t offset, uint8_t *buffer, uint16_t size)
{
    ARG_UNUSED(con);
    ARG_UNUSED(mode);
    ARG_UNUSED(offset);
    if (handle == H_INPUT_CCC || handle == H_DEVICES_CCC || handle == H_EVENT_CCC) {
        bool on = size >= 2 && (little_endian_read_16(buffer, 0) & GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
        if (handle == H_INPUT_CCC) s_notify_input = on;
        else if (handle == H_DEVICES_CCC) s_notify_devices = on;
        else s_notify_events = on;
        return 0;
    }
    if (handle == H_COMMAND) command(buffer, size, s_con != HCI_CON_HANDLE_INVALID && gap_encryption_key_size(s_con) > 0);
    return 0;
}

static void att_events(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    ARG_UNUSED(channel);
    ARG_UNUSED(size);
    if (type != HCI_EVENT_PACKET) return;
    switch (hci_event_packet_get_type(packet)) {
    case ATT_EVENT_CONNECTED:
        s_con = att_event_connected_get_handle(packet);
        s_notify_input = s_notify_devices = s_notify_events = false;
        s_evq_count = 0;
        logi("bridge: FH1 connected (handle %#x)\n", s_con);
        // 7.5 ms: the shortest interval BLE has; the FH1 asks for it too.
        gap_request_connection_parameter_update(s_con, 6, 6, 0, 200);
        break;
    case ATT_EVENT_MTU_EXCHANGE_COMPLETE:
        logi("bridge: MTU %u\n", att_event_mtu_exchange_complete_get_MTU(packet));
        break;
    case ATT_EVENT_DISCONNECTED:
        logi("bridge: FH1 disconnected\n");
        s_con = HCI_CON_HANDLE_INVALID;
        s_notify_input = s_notify_devices = s_notify_events = false;
        break;
    case ATT_EVENT_CAN_SEND_NOW: on_can_send(); break;
    default: break;
    }
}

// The connection's parameters, as the central sets them: the interval matters for latency.
static void hci_events(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    ARG_UNUSED(channel);
    ARG_UNUSED(size);
    if (type != HCI_EVENT_PACKET || hci_event_packet_get_type(packet) != HCI_EVENT_LE_META) return;
    if (hci_event_le_meta_get_subevent_code(packet) == HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE &&
        hci_subevent_le_connection_update_complete_get_connection_handle(packet) == s_con) {
        uint16_t iv = hci_subevent_le_connection_update_complete_get_conn_interval(packet);
        logi("bridge: connection interval %u.%02u ms\n", iv * 125 / 100, iv * 125 % 100);
    }
}

static void udp_command(const uint8_t *b, uint16_t n) { command(b, n, false); }

// While an FH1 is linked over Wi-Fi, advertising slows to about a second (the FH1 can still find the
// bridge to move it to BLE): fast advertising takes radio time from Wi-Fi.
static void advertise_rate(bool slow)
{
    static bool now_slow;
    if (slow == now_slow) return;
    now_slow = slow;
    bd_addr_t none = { 0 };
    gap_advertisements_enable(false);
    if (slow) gap_advertisements_set_params(0x0640, 0x0800, 0, 0, none, 0x07, 0x00);   // 1-1.28 s
    else gap_advertisements_set_params(0x0030, 0x0060, 0, 0, none, 0x07, 0x00);       // 30-60 ms
    gap_advertisements_enable(true);
    logi("bridge: advertising %s\n", slow ? "slow (Wi-Fi link)" : "fast");
}

static void advertise(void)
{
    // Advertising: flags and the service's UUID; the name in the scan response.
    static uint8_t adv[] = { 0x02, BLUETOOTH_DATA_TYPE_FLAGS, 0x06, 0x11, BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_128_BIT_SERVICE_CLASS_UUIDS,
                             FH1B_UUID_BYTES(FH1B_CHR_SERVICE) };
    static uint8_t rsp[] = { 0x0B, BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME, 'F', 'H', '1', ' ', 'B', 'r', 'i', 'd', 'g', 'e' };
    bd_addr_t none = { 0 };
    gap_advertisements_set_params(0x0030, 0x0060, 0, 0, none, 0x07, 0x00);   // 30-60 ms
    gap_advertisements_set_data(sizeof(adv), adv);
    gap_scan_response_set_data(sizeof(rsp), rsp);
    gap_advertisements_enable(true);
}

// ── Bluepad32 platform ──

static int slot_of(uni_hid_device_t *d)
{
    int i = uni_hid_device_get_idx_for_instance(d);
    return i >= 0 && i < SLOTS ? i : -1;
}

static void greet(uni_hid_device_t *d, int slot)
{
    static const uint8_t colours[4][3] = { { 0, 0, 255 }, { 255, 0, 0 }, { 0, 255, 0 }, { 255, 0, 255 } };
    if (d->report_parser.set_player_leds) d->report_parser.set_player_leds(d, 1u << slot);
    if (d->report_parser.set_lightbar_color) {
        d->report_parser.set_lightbar_color(d, colours[slot & 3][0], colours[slot & 3][1], colours[slot & 3][2]);
        memcpy(s_slot[slot].info.rgb, colours[slot & 3], 3);     // the FH1 shows it in its tray
    }
    if (d->report_parser.play_dual_rumble) d->report_parser.play_dual_rumble(d, 0, 150, 128, 40);
}

static void bridge_init(int argc, const char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    logi("bridge: protocol %d\n", FH1B_PROTO_VERSION);
    board_init();
    if (board_codec()) audio_rx_init();
}

static void bridge_on_init_complete(void)
{
    uni_bt_list_keys_unsafe();
    uni_bt_allow_incoming_connections(true);
    static esp_timer_handle_t watch;
    const esp_timer_create_args_t wa = { .callback = runloop_watch, .name = "runloop_watch" };
    if (esp_timer_create(&wa, &watch) == ESP_OK) esp_timer_start_periodic(watch, 1000000);
    bridge_ota_init();
    bridge_udp_init(udp_hello, udp_command);
    beacon_update();
    bridge_wifi_init();
    att_server_init(profile_data, att_read, att_write);
    att_server_register_packet_handler(att_events);
    s_hci_cb.callback = hci_events;
    hci_add_event_handler(&s_hci_cb);
    advertise();
    btstack_run_loop_set_timer_handler(&s_scan_timer, scan_timeout);
    btstack_run_loop_set_timer_handler(&s_tick, tick);
    btstack_run_loop_set_timer(&s_tick, 1000u / s_rate_hz);
    btstack_run_loop_add_timer(&s_tick);
    // Paired controllers come back by themselves (they connect to us): scan at start only when none is paired.
    int paired = 0;
    btstack_link_key_iterator_t it;
    if (gap_link_key_iterator_init(&it)) {
        bd_addr_t a;
        link_key_t k;
        link_key_type_t t;
        while (gap_link_key_iterator_get_next(&it, a, k, &t)) paired++;
        gap_link_key_iterator_done(&it);
    }
    if (!paired) scan(true, 30);
    logi("bridge: advertising as \"FH1 Bridge\". DualSense: hold Create + PS until the light bar flashes fast\n");
}

static void bridge_on_device_connected(uni_hid_device_t *d) { logi("bridge: connected %s\n", bd_addr_to_str(d->conn.btaddr)); }

static void bridge_on_device_disconnected(uni_hid_device_t *d)
{
    int s = slot_of(d);
    logi("bridge: slot %d disconnected\n", s);
    if (s >= 0 && s_slot[s].ready) {
        s_slot[s].ready = false;
        devices_changed();
    }
}

static uni_error_t bridge_on_device_ready(uni_hid_device_t *d)
{
    int s = slot_of(d);
    if (s < 0) return UNI_ERROR_NO_SLOTS;
    slot_t *sl = &s_slot[s];
    memset(sl, 0, sizeof(*sl));
    sl->d = d;
    sl->ready = true;
    fh1b_device_t *i = &sl->info;
    i->id = (uint8_t)s;
    i->cls = FH1B_CLASS_GAMEPAD;
    i->state = FH1B_STATE_READY;
    i->model = (uint8_t)d->controller_type;
    i->vid = d->vendor_id;
    i->pid = d->product_id;
    i->features = FH1B_CAP_GAMEPAD | (d->report_parser.play_dual_rumble ? FH1B_CAP_RUMBLE : 0) |
                  (d->report_parser.set_lightbar_color ? FH1B_CAP_LIGHTBAR : 0) |
                  (d->report_parser.set_player_leds ? FH1B_CAP_LEDS : 0) |
                  (d->controller_type == CONTROLLER_TYPE_PS5Controller ? FH1B_CAP_TRIGGERS : 0) | FH1B_CAP_MOTION;
    memcpy(i->addr, d->conn.btaddr, 6);
    memcpy(i->name, d->name, strnlen(d->name, sizeof(i->name)));   // not NUL-terminated when 20 long
    sl->pad.id = (uint8_t)s;
    greet(d, s);
    logi("bridge: slot %d ready: \"%s\", type %d, vid %04x pid %04x\n", s, d->name, d->controller_type, d->vendor_id,
         d->product_id);
    scan(false, 0);
    devices_changed();
    return UNI_ERROR_SUCCESS;
}

static int16_t clamp16(float v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

static void bridge_on_controller_data(uni_hid_device_t *d, uni_controller_t *ctl)
{
    int s = slot_of(d);
    if (s < 0 || !s_slot[s].ready || ctl->klass != UNI_CONTROLLER_CLASS_GAMEPAD) return;
    const uni_gamepad_t *gp = &ctl->gamepad;
    fh1b_pad_t *p = &s_slot[s].pad;
    p->dpad = gp->dpad;
    p->buttons = gp->buttons;
    p->misc = gp->misc_buttons;
    p->lx = (int16_t)gp->axis_x;
    p->ly = (int16_t)gp->axis_y;
    p->rx = (int16_t)gp->axis_rx;
    p->ry = (int16_t)gp->axis_ry;
    p->l2 = (uint16_t)gp->brake;
    p->r2 = (uint16_t)gp->throttle;
    for (int i = 0; i < 3; i++) {
        p->gyro[i] = clamp16(gp->gyro[i] * 1000);
        p->accel[i] = clamp16(gp->accel[i] * 100);
    }
    // The device list is notified again when the battery moves by a sixteenth.
    if (ctl->battery / 16 != s_slot[s].info.battery / 16) s_devices_changed = s_udp_devices = true;
    s_slot[s].info.battery = ctl->battery;
    p->battery = ctl->battery;
    s_slot[s].reports++;
}

static const uni_property_t *bridge_get_property(uni_property_idx_t idx)
{
    ARG_UNUSED(idx);
    return NULL;
}

static void bridge_on_oob_event(uni_platform_oob_event_t event, void *data)
{
    if (event == UNI_PLATFORM_OOB_BLUETOOTH_ENABLED) logi("bridge: Bluetooth %s\n", data ? "on" : "off");
}

struct uni_platform *bridge_platform(void)
{
    static struct uni_platform plat = {
        .name = "fh1-bridge",
        .init = bridge_init,
        .on_init_complete = bridge_on_init_complete,
        .on_device_connected = bridge_on_device_connected,
        .on_device_disconnected = bridge_on_device_disconnected,
        .on_device_ready = bridge_on_device_ready,
        .on_oob_event = bridge_on_oob_event,
        .on_controller_data = bridge_on_controller_data,
        .get_property = bridge_get_property,
    };
    return &plat;
}
