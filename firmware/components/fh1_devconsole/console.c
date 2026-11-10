// The dev console's web server: one static page, a WebSocket for live data and commands,
// and an upload endpoint for packages. Every client must send the 6-digit code first.
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>
#include <stdlib.h>
#include "fh1_devconsole.h"
#include "fh1_devices.h"
#include "fh1_input.h"
#include "fh1_gamepad.h"
#include "fh1_bridge.h"
#include "fh1_settings.h"
#include "fh1_storage.h"
#include "fh1_board.h"
#include "fh1_ota.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "console_priv.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_log.h"
#include "fh1_events.h"
#include "fh1_app.h"
#include "driver/temperature_sensor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/freertos_debug.h"
#include "esp_memory_utils.h"

static const char *TAG = "console";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

#define MAX_CLIENTS 4
#define LOG_RING 6144

static httpd_handle_t s_server;
static int s_fds[MAX_CLIENTS];          // authenticated WebSocket clients, 0 = free
static volatile bool s_need_full;        // a client joined: the next push carries everything
static char s_token[12];
static fh1_console_apps_json_fn s_apps_json;
static fh1_console_apps_cmd_fn s_apps_cmd;
static temperature_sensor_handle_t s_tsens;

// ── Log capture: a ring of recent log text, sent with each stats push ──

static char *s_log;                // LOG_RING bytes in PSRAM: a dev tool keeps no internal RAM
static size_t s_log_head;               // total bytes ever written
static size_t s_log_sent;
static portMUX_TYPE s_log_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_prev_vprintf;

static int log_vprintf(const char *fmt, va_list ap)
{
    char buf[256];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap2);
    va_end(ap2);
    if (n > 0 && s_log) {
        if (n > (int)sizeof(buf) - 1) n = sizeof(buf) - 1;
        portENTER_CRITICAL(&s_log_mux);
        for (int i = 0; i < n; i++) s_log[(s_log_head + i) % LOG_RING] = buf[i];
        s_log_head += n;
        portEXIT_CRITICAL(&s_log_mux);
    }
    return s_prev_vprintf(fmt, ap);
}

void fh1_console_log_init(void)
{
    s_log = heap_caps_calloc(1, LOG_RING, MALLOC_CAP_SPIRAM);
    s_prev_vprintf = esp_log_set_vprintf(log_vprintf);
}

// Copy the log text not yet sent (at most the ring's size) into out.
// A browser or a client refuses a message that is not valid UTF-8: the log ring's start can fall inside a letter
// ("Trovão"), and a task that just ended can show garbage for a name. Broken sequences become '?'.
static void utf8_clean(char *s)
{
    // Strictly (as the PC tools' decoders): no overlong forms (C0, C1, E0 80-9F, F0 80-8F), no surrogates (ED A0-BF),
    // nothing past U+10FFFF (F4 90-, F5-FF).
    unsigned char *p = (unsigned char *)s;
    while (*p) {
        const unsigned c = *p;
        const int n = c < 0x80 ? 1 : c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
        bool ok = n > 0;
        for (int k = 1; ok && k < n; k++) ok = (p[k] & 0xC0) == 0x80;
        if (ok && n > 2) {
            const unsigned d = p[1];
            ok = !(c == 0xE0 && d < 0xA0) && !(c == 0xED && d >= 0xA0) && !(c == 0xF0 && d < 0x90) && !(c == 0xF4 && d >= 0x90);
        }
        if (!ok) {
            *p++ = '?';
            continue;
        }
        p += n;
    }
}

static size_t log_take(char *out, size_t cap)
{
    portENTER_CRITICAL(&s_log_mux);
    size_t head = s_log_head;
    size_t from = s_log_sent;
    if (head - from > LOG_RING) from = head - LOG_RING;
    if (head - from > cap - 1) from = head - (cap - 1);
    size_t n = head - from;
    for (size_t i = 0; s_log && i < n; i++) out[i] = s_log[(from + i) % LOG_RING];
    if (!s_log) n = 0;
    s_log_sent = head;
    portEXIT_CRITICAL(&s_log_mux);
    out[n] = 0;
    return n;
}

const char *fh1_console_token(void)
{
    if (!s_token[0]) {
        uint32_t t = fh1_settings_get_u32("console", "token", 0);
        if (t == 0) {
            t = 100000 + esp_random() % 900000;
            fh1_settings_set_u32("console", "token", t);
        }
        snprintf(s_token, sizeof(s_token), "%06lu", (unsigned long)(t % 1000000));
    }
    return s_token;
}

void fh1_console_set_apps_hooks(fh1_console_apps_json_fn json_fn, fh1_console_apps_cmd_fn cmd_fn)
{
    s_apps_json = json_fn;
    s_apps_cmd = cmd_fn;
}

// ── Stats ──

// Tasks and CPU load. The task list comes from uxTaskGetSystemState, which reads every task under the
// kernel's own lock (both cores): reading task by task after a snapshot crashed twice (a task ending on the
// other core in between), and holding the other core through an IPC call left the stats stuck once. It also
// measures every stack's high-water mark, PSRAM ones too, which competes with the panel's refills (a display
// glitch per call): so the list is read every TASKS_EVERY pushes and repeated in between. The CPU load uses
// only the idle tasks' counters, read each time.
#define TASKS_EVERY 10
static void add_cpu_and_tasks(cJSON *root)
{
    static uint32_t prev_total;
    static uint32_t prev_idle[2];
    static TaskStatus_t *st;
    static UBaseType_t n, cap;
    static int age;
    uint32_t total = 0;
    if (!st || age++ % TASKS_EVERY == 0) {
        const UBaseType_t want = uxTaskGetNumberOfTasks() + 4;
        if (want > cap) {
            TaskStatus_t *grown = realloc(st, want * sizeof(TaskStatus_t));
            if (!grown) return;
            st = grown;
            cap = want;
        }
        n = uxTaskGetSystemState(st, cap, NULL);
    }
    total = (uint32_t)portGET_RUN_TIME_COUNTER_VALUE();
    uint32_t idle[2] = { 0, 0 };
    // The idle tasks' counters, fresh (by handle: safe, idle tasks never end).
    for (int c = 0; c < 2; c++) idle[c] = ulTaskGetIdleRunTimeCounterForCore(c);
    cJSON *tasks = cJSON_AddArrayToObject(root, "tasks");
    for (UBaseType_t i = 0; i < n; i++) {
        const BaseType_t core = st[i].xCoreID;
        cJSON *t = cJSON_CreateArray();
        cJSON_AddItemToArray(t, cJSON_CreateString(st[i].pcTaskName));
        cJSON_AddItemToArray(t, cJSON_CreateNumber(st[i].eCurrentState));
        cJSON_AddItemToArray(t, cJSON_CreateNumber(st[i].uxCurrentPriority));
        cJSON_AddItemToArray(t, cJSON_CreateNumber(st[i].usStackHighWaterMark));
        cJSON_AddItemToArray(t, cJSON_CreateNumber(core < 0 || core > 1 ? -1 : core));
        cJSON_AddItemToArray(tasks, t);
    }
    uint32_t dt = total - prev_total;
    cJSON *cpu = cJSON_AddArrayToObject(root, "cpu");
    for (int c = 0; c < 2; c++) {
        // The total is wall-clock time; each core's idle task runs for up to all of it.
        uint32_t di = idle[c] - prev_idle[c];
        int load = (prev_total && dt) ? 100 - (int)((uint64_t)di * 100 / dt) : 0;
        if (load < 0) load = 0;
        if (load > 100) load = 100;
        cJSON_AddItemToArray(cpu, cJSON_CreateNumber(load));
        prev_idle[c] = idle[c];
    }
    prev_total = total;
}

// ── The web controller: a gamepad of its own ("web", never taken by an automatic player). Chosen as a player
// (Inputs and outputs, or "Take player 1" on the page), its keys are that player's; otherwise they act as the
// FH1's own keys, as before, so the page always reaches the menus.

#define WEB_PAD_ID "web"
static int s_web_pad = -1;
static uint32_t s_web_keys;
static char s_prev_p1[18];             // player 1's choice before the page took it (given back on release)
static bool s_took_p1;

static void web_pad_register(void)
{
    if (s_web_pad >= 0) return;
    fh1_gamepad_info_t g = { .kind = FH1_PAD_FLAT, .rgb = 0x26a7c7, .manual = true };
    snprintf(g.id, sizeof(g.id), WEB_PAD_ID);
    snprintf(g.name, sizeof(g.name), "Web console");
    snprintf(g.via, sizeof(g.via), "Wi-Fi");
    s_web_pad = fh1_gamepad_add(&g);
}

static void web_key(int k, bool pressed)
{
    if (pressed) s_web_keys |= 1u << k;
    else s_web_keys &= ~(1u << k);
    if (s_web_pad >= 0 && fh1_gamepad_player_of_id(WEB_PAD_ID) >= 0) {
        static const uint32_t btn[FH1_KEY_COUNT] = {
            [FH1_KEY_UP] = FH1_PAD_BTN_UP, [FH1_KEY_DOWN] = FH1_PAD_BTN_DOWN, [FH1_KEY_LEFT] = FH1_PAD_BTN_LEFT,
            [FH1_KEY_RIGHT] = FH1_PAD_BTN_RIGHT, [FH1_KEY_A] = FH1_PAD_BTN_B, [FH1_KEY_B] = FH1_PAD_BTN_A,
            [FH1_KEY_X] = FH1_PAD_BTN_Y, [FH1_KEY_Y] = FH1_PAD_BTN_X, [FH1_KEY_START] = FH1_PAD_BTN_START,
            [FH1_KEY_SELECT] = FH1_PAD_BTN_SELECT, [FH1_KEY_MENU] = FH1_PAD_BTN_SYSTEM,
        };
        fh1_pad_state_t st = { .keys = s_web_keys, .time_ms = (uint32_t)(esp_timer_get_time() / 1000) };
        for (int i = 0; i < FH1_KEY_COUNT; i++) {
            if (s_web_keys & (1u << i)) st.buttons |= btn[i];
        }
        fh1_gamepad_set_state(s_web_pad, &st);
    } else {
        fh1_input_post(k, pressed);
    }
}

static char *build_stats(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "t", "stats");
    cJSON_AddNumberToObject(root, "up", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(root, "fw", fh1_ota_running_version());
    cJSON_AddStringToObject(root, "slot", esp_ota_get_running_partition()->label);
    cJSON_AddNumberToObject(root, "webpad", s_web_pad >= 0 ? fh1_gamepad_player_of_id(WEB_PAD_ID) + 1 : 0);   // its player, 0 none
    // The bridges: how each is linked, and where to send its firmware (POST http://<ip>/ota with its code).
    cJSON *brs = cJSON_AddArrayToObject(root, "bridges");
    for (int i = 0; i < fh1_bridge_count(); i++) {
        fh1_bridge_view_t v;
        if (!fh1_bridge_get(i, &v)) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "alias", v.alias);
        cJSON_AddStringToObject(o, "link", v.link == FH1_BRIDGE_LINK_WIFI ? "wifi" : v.link == FH1_BRIDGE_LINK_BLE ? "ble" : "none");
        char fw[17] = "";
        memcpy(fw, v.info.firmware, 16);
        cJSON_AddStringToObject(o, "fw", fw);
        if (v.have_status && v.status.wifi == FH1B_WIFI_CONNECTED) {
            char ip[16];
            snprintf(ip, sizeof(ip), "%u.%u.%u.%u", v.status.wifi_ip[0], v.status.wifi_ip[1], v.status.wifi_ip[2], v.status.wifi_ip[3]);
            cJSON_AddStringToObject(o, "ip", ip);
            cJSON_AddNumberToObject(o, "ota_code", v.status.ota_code);
        }
        cJSON_AddItemToArray(brs, o);
    }
    fh1_display_stats_t ds;
    fh1_board_display_stats(&ds, true);      // per push: each stats message covers the last window
    cJSON *disp = cJSON_AddObjectToObject(root, "disp");
    cJSON_AddNumberToObject(disp, "frames", ds.frames);
    cJSON_AddNumberToObject(disp, "fills", ds.fills);
    cJSON_AddNumberToObject(disp, "late", ds.late_fills);
    cJSON_AddNumberToObject(disp, "odd", ds.odd_frames);
    cJSON_AddNumberToObject(disp, "max_fill_us", ds.max_fill_us);
    cJSON_AddNumberToObject(disp, "late_starts", ds.late_starts);
    cJSON_AddNumberToObject(disp, "max_gap_us", ds.max_gap_us);
    cJSON_AddNumberToObject(disp, "vs_min", ds.vsync_min_us);
    cJSON_AddNumberToObject(disp, "vs_max", ds.vsync_max_us);
    cJSON *heap = cJSON_AddObjectToObject(root, "heap");
    cJSON_AddNumberToObject(heap, "int_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(heap, "int_min", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(heap, "int_big", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(heap, "ps_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(heap, "ps_min", heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(heap, "ps_big", heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    add_cpu_and_tasks(root);

    float temp;
    if (s_tsens && temperature_sensor_get_celsius(s_tsens, &temp) == ESP_OK) cJSON_AddNumberToObject(root, "temp", temp);

    fh1_fs_info_t fi;
    if (fh1_int_info(&fi) == ESP_OK) {
        cJSON *in = cJSON_AddObjectToObject(root, "int");
        cJSON_AddNumberToObject(in, "total", (double)fi.total);
        cJSON_AddNumberToObject(in, "used", (double)fi.used);
    }
    fh1_sd_info_t si;
    fh1_sd_info(&si);
    cJSON *sd = cJSON_AddObjectToObject(root, "sd");
    cJSON_AddBoolToObject(sd, "mounted", si.mounted);
    cJSON_AddBoolToObject(sd, "ejected", si.ejected);
    cJSON_AddStringToObject(sd, "name", si.name);
    cJSON_AddNumberToObject(sd, "total", (double)si.total);
    cJSON_AddNumberToObject(sd, "free", (double)si.free);
    cJSON_AddNumberToObject(sd, "khz", si.speed_khz);

    char ssid[33], ip[16];
    int rssi;
    bool up = fh1_wifi_status(ssid, sizeof(ssid), ip, sizeof(ip), &rssi);
    cJSON *w = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddBoolToObject(w, "up", up);
    cJSON_AddStringToObject(w, "ssid", ssid);
    cJSON_AddStringToObject(w, "ip", ip);
    cJSON_AddNumberToObject(w, "rssi", rssi);

    cJSON *devs = cJSON_AddArrayToObject(root, "devs");
    for (size_t i = 0; i < fh1_dev_count(); i++) {
        fh1_device_t *d = fh1_dev_at(i);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", d->id);
        cJSON_AddStringToObject(o, "cls", d->cls);
        cJSON_AddStringToObject(o, "drv", d->drv->name);
        cJSON_AddStringToObject(o, "state", fh1_dev_state_name(d->state));
        cJSON_AddStringToObject(o, "info", d->info);
        cJSON_AddStringToObject(o, "reason", d->reason);
        cJSON_AddBoolToObject(o, "essential", d->essential);
        cJSON_AddNumberToObject(o, "ma", d->ma);
        cJSON_AddItemToArray(devs, o);
    }
    cJSON *caps = cJSON_AddArrayToObject(root, "caps");
    for (int i = 0; i < FH1_CAP_COUNT; i++) {
        if (fh1_caps() & (1u << i)) cJSON_AddItemToArray(caps, cJSON_CreateString(fh1_cap_name(1u << i)));
    }
    if (s_apps_json) s_apps_json(root);
    // The devices and the apps (3.8 KB of a 4.6 KB push) go only when they change or a client has just joined:
    // every push sits in each client's TCP queue in internal RAM until acknowledged, and four clients' worth of
    // full pushes took it down to 96 bytes.
    static char *s_sig;
    cJSON *pair = cJSON_CreateObject();
    cJSON_AddItemReferenceToObject(pair, "devs", cJSON_GetObjectItem(root, "devs"));
    cJSON_AddItemReferenceToObject(pair, "apps", cJSON_GetObjectItem(root, "apps"));
    char *sig = cJSON_PrintUnformatted(pair);
    cJSON_Delete(pair);
    if (sig && s_sig && !strcmp(sig, s_sig) && !s_need_full) {
        cJSON_DeleteItemFromObject(root, "devs");
        cJSON_DeleteItemFromObject(root, "apps");
        cJSON_free(sig);
    } else if (sig) {
        if (s_sig) cJSON_free(s_sig);
        s_sig = sig;
        s_need_full = false;
    }

    char *logbuf = malloc(LOG_RING + 1);
    if (logbuf) {
        log_take(logbuf, LOG_RING + 1);
        cJSON_AddStringToObject(root, "log", logbuf);
        free(logbuf);
    }
    char *out = cJSON_PrintUnformatted(root);
    if (out) utf8_clean(out);                    // the log's cut start, a task's name read as it ended: '?'.
    cJSON_Delete(root);
    return out;
}

// ── WebSocket ──

static bool has_clients(void)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_fds[i]) return true;
    }
    return false;
}

// A client that cannot take a message is closed, not only forgotten: its socket would keep the unsent data (a
// stats push is several TCP segments, ~1.5 KB of internal RAM each) until TCP gave up, minutes later. Clients that
// vanished (a PC tool killed, a laptop asleep) held internal RAM that way down to 0 bytes free.
static void send_text(int fd, const char *text)
{
    httpd_ws_frame_t f = { .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)text, .len = strlen(text) };
    if (httpd_ws_send_frame_async(s_server, fd, &f) != ESP_OK) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (s_fds[i] == fd) s_fds[i] = 0;
        }
        httpd_sess_trigger_close(s_server, fd);
    }
}

static void push_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));      // every 2 s: each push costs PSRAM bandwidth the panel needs
        if (!s_server || !has_clients()) continue;
        char *json = build_stats();
        if (!json) continue;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (s_fds[i]) send_text(s_fds[i], json);
        }
        cJSON_free(json);
    }
}

static void reply(int fd, const char *msg)
{
    // Valid UTF-8 always (a reply cut at its buffer's end can split a letter: the PC tools then drop the link).
    static char clean[640];
    static char buf[800];
    strlcpy(clean, msg, sizeof(clean));
    utf8_clean(clean);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "reply");
    cJSON_AddStringToObject(o, "msg", clean);
    if (cJSON_PrintPreallocated(o, buf, sizeof(buf), false)) send_text(fd, buf);
    cJSON_Delete(o);
}

static void handle_msg(int fd, const char *text)
{
    cJSON *m = cJSON_Parse(text);
    if (!m) return;
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(m, "t"));
    if (!t) {
        cJSON_Delete(m);
        return;
    }
    char out[160] = "";
    bool authed = false;
    for (int i = 0; i < MAX_CLIENTS; i++) authed |= s_fds[i] == fd;
    if (strcmp(t, "auth") == 0) {
        const char *tok = cJSON_GetStringValue(cJSON_GetObjectItem(m, "token"));
        if (tok && strcmp(tok, fh1_console_token()) == 0) {
            int slot = -1;
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (s_fds[i] == fd || (slot < 0 && s_fds[i] == 0)) slot = i;
            }
            if (slot < 0) {                      // all taken: the oldest client goes (likely one that went away)
                httpd_sess_trigger_close(s_server, s_fds[0]);
                memmove(&s_fds[0], &s_fds[1], (MAX_CLIENTS - 1) * sizeof(s_fds[0]));
                slot = MAX_CLIENTS - 1;
            }
            s_fds[slot] = fd;
            s_need_full = true;
            send_text(fd, "{\"t\":\"auth\",\"ok\":true}");
        } else {
            send_text(fd, "{\"t\":\"auth\",\"ok\":false,\"msg\":\"wrong code\"}");
        }
    } else if (!authed) {
        send_text(fd, "{\"t\":\"auth\",\"ok\":false,\"msg\":\"code needed\"}");
    } else if (strcmp(t, "key") == 0) {
        int k = fh1_key_from_name(cJSON_GetStringValue(cJSON_GetObjectItem(m, "k")) ?: "");
        if (k >= 0) web_key(k, cJSON_IsTrue(cJSON_GetObjectItem(m, "p")));
    } else if (strcmp(t, "pad") == 0) {
        // The web controller as player 1 (taking it from whatever had it), or released.
        const char *op = cJSON_GetStringValue(cJSON_GetObjectItem(m, "op")) ?: "";
        web_pad_register();
        if (strcmp(op, "p1") == 0) {
            char c[18];
            fh1_gamepad_assignment(0, c, sizeof(c));
            if (strcmp(c, WEB_PAD_ID) != 0) {
                memcpy(s_prev_p1, c, sizeof(s_prev_p1));
                s_took_p1 = true;
            }
            fh1_gamepad_assign(0, WEB_PAD_ID);
            snprintf(out, sizeof(out), "web controller is player 1");
        } else if (strcmp(op, "release") == 0) {
            for (int p = 0; p < FH1_PLAYERS; p++) {
                char c[18];
                fh1_gamepad_assignment(p, c, sizeof(c));
                if (strcmp(c, WEB_PAD_ID) != 0) continue;
                // Player 1 goes back to what it had before the page took it; anything else to automatic.
                fh1_gamepad_assign(p, p == 0 && s_took_p1 ? s_prev_p1 : "");
            }
            s_took_p1 = false;
            char c[18];
            fh1_gamepad_assignment(0, c, sizeof(c));
            snprintf(out, sizeof(out), "web controller released; player 1: %s", c[0] ? c : "automatic");
        }
    } else if (strcmp(t, "dev") == 0) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(m, "id")) ?: "";
        const char *op = cJSON_GetStringValue(cJSON_GetObjectItem(m, "op")) ?: "";
        esp_err_t err = ESP_ERR_INVALID_ARG;
        if (strcmp(op, "enable") == 0) err = fh1_dev_set_enabled(id, true);
        else if (strcmp(op, "disable") == 0) err = fh1_dev_set_enabled(id, false);
        else if (strcmp(op, "probe") == 0) err = fh1_dev_reprobe(id);
        snprintf(out, sizeof(out), "%s %s: %s", id, op, err == ESP_OK ? "done" : esp_err_to_name(err));
    } else if (strcmp(t, "sd") == 0) {
        const char *op = cJSON_GetStringValue(cJSON_GetObjectItem(m, "op")) ?: "";
        esp_err_t err = ESP_ERR_INVALID_ARG;
        if (strcmp(op, "mount") == 0) err = fh1_sd_mount();
        else if (strcmp(op, "eject") == 0) err = fh1_sd_eject();
        else if (strcmp(op, "format") == 0) err = fh1_sd_format();
        snprintf(out, sizeof(out), "SD %s: %s", op, err == ESP_OK ? "done" : esp_err_to_name(err));
    } else if (strcmp(t, "app") == 0 && s_apps_cmd) {
        s_apps_cmd(m, out, sizeof(out));
    }
    if (out[0]) reply(fd, out);
    cJSON_Delete(m);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) return ESP_OK;     // handshake
    httpd_ws_frame_t f = { 0 };
    esp_err_t err = httpd_ws_recv_frame(req, &f, 0);
    if (err != ESP_OK || f.len > 2048) return err;
    if (f.type == HTTPD_WS_TYPE_CLOSE) return ESP_OK;
    char *buf = calloc(1, f.len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    f.payload = (uint8_t *)buf;
    err = httpd_ws_recv_frame(req, &f, f.len);
    if (err == ESP_OK && f.type == HTTPD_WS_TYPE_TEXT) handle_msg(httpd_req_to_sockfd(req), buf);
    free(buf);
    return err;
}

static void on_close(httpd_handle_t hd, int fd)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_fds[i] == fd) s_fds[i] = 0;
    }
    close(fd);
}

// ── HTTP ──

// POST /ota with header X-FH1-Code: a firmware image, written to the spare slot; then restart.
static void restart_later(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));     // the screen says "Restarting..." meanwhile
    fh1_system_restart();                // a running game saves, the windows close, then the restart
    vTaskDelete(NULL);
}

static esp_err_t ota_handler(httpd_req_t *req)
{
    char code[12] = "", err[96] = "";
    httpd_req_get_hdr_value_str(req, "X-FH1-Code", code, sizeof(code));
    if (strcmp(code, fh1_console_token()) != 0) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "code needed");
        return ESP_FAIL;
    }
    fh1_ota_t *o;
    if (fh1_ota_begin(req->content_len, &o, err, sizeof(err)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err);
        return ESP_FAIL;
    }
    char *buf = malloc(8192);
    int left = req->content_len;
    esp_err_t e = buf ? ESP_OK : ESP_ERR_NO_MEM;
    fh1_transfer_report(FH1_XFER_FIRMWARE, FH1_XFER_RUNNING, "", 0, req->content_len);
    while (e == ESP_OK && left > 0) {
        int n = httpd_req_recv(req, buf, left < 8192 ? left : 8192);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) {
            ESP_LOGW(TAG, "firmware upload: read returned %d with %d of %d bytes left", n, left, (int)req->content_len);
            snprintf(err, sizeof(err), "upload interrupted");
            e = ESP_FAIL;
            break;
        }
        e = fh1_ota_write(o, buf, n, err, sizeof(err));
        left -= n;
        fh1_transfer_report(FH1_XFER_FIRMWARE, FH1_XFER_RUNNING, "", req->content_len - left, req->content_len);
    }
    free(buf);
    if (e == ESP_OK) e = fh1_ota_finish(o, err, sizeof(err));
    else fh1_ota_abort(o);
    fh1_transfer_report(FH1_XFER_FIRMWARE, e == ESP_OK ? FH1_XFER_DONE : FH1_XFER_FAILED, "", 1, 1);
    if (e != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err);
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "firmware written; restarting");
    xTaskCreate(restart_later, "restart", 2048, NULL, 1, NULL);
    return ESP_OK;
}

// GET /shot.bmp?code=NNNNNN: one still frame of the panel, as a 16-bit BMP. On demand only; the
// console never streams the screen.
static esp_err_t shot_handler(httpd_req_t *req)
{
    char query[48] = "", code[12] = "";
    httpd_req_get_url_query_str(req, query, sizeof(query));
    httpd_query_key_value(query, "code", code, sizeof(code));
    if (strcmp(code, fh1_console_token()) != 0) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "code needed");
        return ESP_FAIL;
    }
    int w, h;
    const uint16_t *fb = fh1_board_framebuffer(&w, &h);
    if (!fb) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no frame buffer");
        return ESP_FAIL;
    }
    uint8_t hdr[66] = { 'B', 'M' };
    uint32_t data = w * h * 2, off = sizeof(hdr);
    const uint32_t fields[] = { off + data, 0, off, 40, (uint32_t)w, (uint32_t)h };
    memcpy(hdr + 2, &fields[0], 4);
    memcpy(hdr + 10, &fields[2], 4);
    memcpy(hdr + 14, &fields[3], 4);
    memcpy(hdr + 18, &fields[4], 4);
    memcpy(hdr + 22, &fields[5], 4);
    hdr[26] = 1;            // planes
    hdr[28] = 16;           // bits per pixel
    hdr[30] = 3;            // BI_BITFIELDS
    memcpy(hdr + 34, &data, 4);
    const uint32_t masks[3] = { 0xF800, 0x07E0, 0x001F };
    memcpy(hdr + 54, masks, 12);
    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_send_chunk(req, (const char *)hdr, sizeof(hdr));
    const int rows = 20;
    uint16_t *buf = malloc(w * 2 * rows);
    for (int y = h - 1; buf && y >= 0; y -= rows) {   // BMP rows go bottom-up
        int n = 0;
        for (int k = 0; k < rows && y - k >= 0; k++, n++) memcpy(buf + k * w, fb + (y - k) * w, w * 2);
        esp_err_t err = httpd_resp_send_chunk(req, (const char *)buf, n * w * 2);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "screenshot: send failed at row %d: %s", y, esp_err_to_name(err));
            break;
        }
    }
    free(buf);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start);
}

static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(h, NULL, 16);
            s += 2;
        } else {
            *o++ = *s == '+' ? ' ' : *s;
        }
    }
    *o = 0;
}

static bool ends_with(const char *s, const char *tail)
{
    size_t a = strlen(s), b = strlen(tail);
    return a >= b && strcasecmp(s + a - b, tail) == 0;
}

static bool is_music(const char *name)
{
    return ends_with(name, ".mp3") || ends_with(name, ".flac") || ends_with(name, ".ogg") || ends_with(name, ".oga") ||
           ends_with(name, ".wav") || ends_with(name, ".mod");
}

// POST /upload?name=<file>.fpk&dest=sd|int with header X-FH1-Code: stores the package in /sd/packages
// (the default, when a card is mounted) or /int/packages. A <file>.bin is firmware: /sd/firmware;
// a game (.gb .gbc .nes .sms .gg .sg): <storage>/roms; music (.mp3 .flac .ogg .oga .wav .mod): <storage>/music.
static esp_err_t upload_handler(httpd_req_t *req)
{
    char code[12] = "", query[256] = "", name[96] = "", dest[8] = "sd";
    httpd_req_get_hdr_value_str(req, "X-FH1-Code", code, sizeof(code));
    if (strcmp(code, fh1_console_token()) != 0) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "code needed");
        return ESP_FAIL;
    }
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "name", name, sizeof(name)) == ESP_OK) {
        url_decode(name);     // "Pokemon%20Red.gb" -> "Pokemon Red.gb"; checked below, after decoding
    }
    if (!name[0] || strchr(name, '/') || strchr(name, '\\') || strstr(name, "..") || strlen(name) < 4 || strlen(name) > 60 ||
        !(ends_with(name, ".fpk") || ends_with(name, ".bin") || ends_with(name, ".gb") || ends_with(name, ".gbc") || ends_with(name, ".nes") ||
          ends_with(name, ".sms") || ends_with(name, ".gg") || ends_with(name, ".sg") || is_music(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "name must end in .fpk, .bin, .gb, .gbc, .nes, .sms, .gg, .sg, .mp3, .flac, .ogg, .oga, .wav or .mod");
        return ESP_FAIL;
    }
    bool firmware = ends_with(name, ".bin");     // goes to /sd/firmware
    bool rom = ends_with(name, ".gb") || ends_with(name, ".gbc") || ends_with(name, ".nes") || ends_with(name, ".sms") ||
               ends_with(name, ".gg") || ends_with(name, ".sg");   // <storage>/roms
    httpd_query_key_value(query, "dest", dest, sizeof(dest));
    fh1_sd_info_t si;
    fh1_sd_info(&si);
    bool to_sd = strcmp(dest, "int") != 0;
    if (to_sd && !si.mounted) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no SD card mounted; choose internal storage");
        return ESP_FAIL;
    }
    if (firmware && !to_sd) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "firmware files go to the SD card");
        return ESP_FAIL;
    }
    const bool music = is_music(name);           // <storage>/music, up to 64 MB (a long FLAC)
    const size_t len = strlen(name);
    const bool package = len > 4 && strcmp(name + len - 4, ".fpk") == 0;   // asset packages are large too
    if (req->content_len > (music || package ? 64 : 4) * 1024 * 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, music || package ? "too big (64 MB max)" : "too big (4 MB max)");
        return ESP_FAIL;
    }
    char path[128];
    const char *dir = firmware ? FH1_SD_PATH "/firmware"
                    : rom ? (to_sd ? FH1_SD_PATH "/roms" : FH1_INT_PATH "/roms")
                    : music ? (to_sd ? FH1_SD_PATH "/music" : FH1_INT_PATH "/music")
                    : to_sd ? FH1_SD_PACKAGES : FH1_INT_PACKAGES;
    fh1_mkdirs(dir);
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot write");
        return ESP_FAIL;
    }
    // Whole 4 KB blocks from one DMA-capable buffer: the storage driver writes straight from it. (Written from
    // PSRAM, or in pieces through stdio, it took a 3.5 KB bounce buffer of internal RAM for each write, while
    // Wi-Fi was taking the rest: allocations failed.)
    char *buf = heap_caps_malloc(4096, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf) buf = malloc(4096);
    int left = req->content_len;
    bool ok = buf != NULL;
    fh1_transfer_report(FH1_XFER_UPLOAD, FH1_XFER_RUNNING, name, 0, req->content_len);
    while (ok && left > 0) {
        int fill = 0;
        const int want = left < 4096 ? left : 4096;
        while (ok && fill < want) {
            const int n = httpd_req_recv(req, buf + fill, want - fill);
            if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
            if (n <= 0) ok = false;
            else fill += n;
        }
        if (ok && write(fd, buf, fill) != fill) ok = false;
        left -= fill;
        if (ok) fh1_transfer_report(FH1_XFER_UPLOAD, FH1_XFER_RUNNING, name, req->content_len - left, req->content_len);
    }
    heap_caps_free(buf);
    if (close(fd) != 0) ok = false;
    fh1_transfer_report(FH1_XFER_UPLOAD, ok ? FH1_XFER_DONE : FH1_XFER_FAILED, name, 1, 1);
    if (!ok) {
        unlink(path);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upload failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "package uploaded: %s (%d bytes)", path, req->content_len);
    if (!to_sd) fh1_int_info_changed();
    httpd_resp_sendstr(req, to_sd ? "stored on the SD card" : "stored in internal storage");
    if (s_apps_cmd) {      // let the apps layer see the new package now
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "op", "rescan");
        char ignored[32];
        s_apps_cmd(m, ignored, sizeof(ignored));
        cJSON_Delete(m);
    }
    return ESP_OK;
}

// The console's JSON (stats every 2 s, commands) is built in PSRAM: it must not take internal RAM from a
// running app.
static void *json_alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }

// Pages other components add (user profiles between units): registered when the server starts.
#define EXTRA_URIS 4
static httpd_uri_t s_extra[EXTRA_URIS];
static int s_nextra;

void fh1_console_add_uri(const httpd_uri_t *u)
{
    if (s_nextra >= EXTRA_URIS) return;
    s_extra[s_nextra] = *u;
    if (s_server) httpd_register_uri_handler(s_server, &s_extra[s_nextra]);
    s_nextra++;
}

void console_server_start(void)
{
    if (s_server) return;
    web_pad_register();
    static cJSON_Hooks hooks = { .malloc_fn = json_alloc, .free_fn = heap_caps_free };
    cJSON_InitHooks(&hooks);
    if (!s_tsens) {
        temperature_sensor_config_t tc = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
        if (temperature_sensor_install(&tc, &s_tsens) == ESP_OK) temperature_sensor_enable(s_tsens);
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = 6;
    cfg.lru_purge_enable = true;
    cfg.close_fn = on_close;
    cfg.stack_size = 8192;
    cfg.core_id = 0;
    cfg.send_wait_timeout = 15;
    cfg.recv_wait_timeout = 15;      // a weak signal stalls uploads for seconds
    // A peer that went away without closing (a tool killed, a laptop asleep) is found in ~10 s and closed: its
    // socket's queued data is internal RAM.
    cfg.keep_alive_enable = true;
    cfg.keep_alive_idle = 5;
    cfg.keep_alive_interval = 2;
    cfg.keep_alive_count = 3;
    cfg.max_uri_handlers = 12;
    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "web server did not start");
        return;
    }
    static const httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = index_handler },
        { .uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true },
        { .uri = "/upload", .method = HTTP_POST, .handler = upload_handler },
        { .uri = "/shot.bmp", .method = HTTP_GET, .handler = shot_handler },
        { .uri = "/ota", .method = HTTP_POST, .handler = ota_handler },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(s_server, &uris[i]);
    for (int i = 0; i < s_nextra; i++) httpd_register_uri_handler(s_server, &s_extra[i]);
    xTaskCreatePinnedToCore(push_task, "console_push", 6144, NULL, 1, NULL, 0);
    ESP_LOGI(TAG, "dev console on port 80, code %s", fh1_console_token());
}
