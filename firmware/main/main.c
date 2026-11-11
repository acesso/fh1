// FH1 start-up: settings → board → devices → storage → UI → launcher (firmware/PLAN.md, "Layers").
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "fh1_mem.h"
#include "fh1_board.h"
#include "fh1_devices.h"
#include "fh1_input.h"
#include "fh1_events.h"
#include "fh1_settings.h"
#include "fh1_app.h"
#include "fh1_i18n.h"
#include "fh1_storage.h"
#include "fh1_ui.h"
#include "fh1_post.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "lvgl.h"
#include "fh1_apps.h"
#include "fh1_ota.h"
#include "esp_timer.h"
#include "fh1_devconsole.h"
#include "serial_keys.h"
#include "fh1_bridge.h"
#include "fh1_gamepad.h"
#include "fh1_users.h"
#include "fh1_audio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "fh1";

static fh1_device_t s_wifi = { .id = "wifi0", .cls = "net.wifi", .caps = FH1_CAP_WIFI, .drv = &fh1_wifi_driver,
                               .default_on = true, .ma = 90 };
// The extension board (Bluetooth controllers over BLE): off until enabled in Devices (Bluetooth costs RAM).
static fh1_device_t s_bridge = { .id = "bridge0", .cls = "link.bridge", .caps = FH1_CAP_BLE, .drv = &fh1_bridge_driver,
                                 .default_on = false, .ma = 10 };
// Bridges over Wi-Fi (UDP on the FH1's network): on by default, it costs a small task and a socket.
static fh1_device_t s_bridge_net = { .id = "bridge1", .cls = "link.bridge.wifi", .caps = 0, .drv = &fh1_bridge_net_driver,
                                     .default_on = true, .ma = 0 };
// A bridge's speaker as a sound output (System > Inputs and outputs > Sound): ready while a bridge with one
// is linked over Wi-Fi. Two, for two such bridges.
static fh1_device_t s_baudio[2] = {
    { .id = "baudio0", .cls = "audio.out", .caps = FH1_CAP_AUDIO_OUT, .drv = &fh1_bridge_audio_driver, .default_on = true },
    { .id = "baudio1", .cls = "audio.out", .caps = FH1_CAP_AUDIO_OUT, .drv = &fh1_bridge_audio_driver, .default_on = true },
};
static fh1_device_t s_devinput = { .id = "devinput0", .cls = "input.buttons", .caps = FH1_CAP_BUTTONS,
                                   .drv = &fh1_devinput_driver, .default_on = true, .essential = true };

// The UI's theme, changed on its own task (no restart: it applies at once).
static void set_theme(void *id) { fh1_ui_set_theme(id); }

// Serial console commands: !wifi <ssid> <password>, !bl <percent>, !mode <native|fh1>, !theme <gen1|gen2|hex|...> (at once),
// !sounds <on|off>
static void on_command(const char *line)
{
    char ssid[33], pass[65];
    int pct;
    // An empty command ("!" then Enter) does nothing: scripts end a line of keys with '!' so the newline
    // after it is not taken as Start (the emulator's UART0 lines, tools/keys.py).
    if (!line[0]) return;
    if (sscanf(line, "wifi %32s %64s", ssid, pass) == 2) {
        fh1_wifi_set_credentials(ssid, pass);
    } else if (sscanf(line, "bl %d", &pct) == 1) {
        fh1_board_backlight(pct < 0 ? 0 : pct);
        ESP_LOGI(TAG, "backlight %d%%", pct);
    } else if (sscanf(line, "mode %15s", ssid) == 1) {
        fh1_settings_set_str("display", "mode", ssid);
        ESP_LOGI(TAG, "screen mode %s, restarting", ssid);
        fh1_system_restart();
    } else if (sscanf(line, "sounds %3s", ssid) == 1) {
        fh1_ui_set_sounds(strcmp(ssid, "on") == 0);
        ESP_LOGI(TAG, "system sounds %s", fh1_ui_sounds() ? "on" : "off");
    } else if (sscanf(line, "theme %15s", ssid) == 1) {
        static char theme[16];                  // read by the UI task, after this returns
        strlcpy(theme, ssid, sizeof(theme));
        fh1_ui_async(set_theme, theme);
        ESP_LOGI(TAG, "theme %s", theme);
    } else {
        ESP_LOGW(TAG, "unknown command: %s (try: !wifi <ssid> <password>, !bl <percent>, !mode <native|fh1>, !theme <gen1|gen2|hex|garden|scifi>, !sounds <on|off>)", line);
    }
}

static void start_apps(void *arg) { fh1_apps_start(); }

// A new firmware counts as good once it has run for 10 s with the UI up; before that, a crash
// sends the bootloader back to the previous firmware.
static void mark_valid(void *arg) { fh1_ota_mark_valid(); }

// What the bottom bar shows.
static void ui_status(fh1_ui_status_t *st)
{
    char ssid[33], ip[16];
    st->wifi_up = fh1_wifi_status(ssid, sizeof(ssid), ip, sizeof(ip), &st->wifi_rssi);
    st->sd = (fh1_caps() & FH1_CAP_SD) != 0;
    st->battery = (fh1_caps() & FH1_CAP_BATTERY) != 0;     // the FH1's battery driver fills in the level
    // The tray: the connected gamepads (shape, colour, battery) and the sound's volume.
    // A board's keys only while someone plays with them.
    fh1_gamepad_info_t g;
    int player;
    for (int i = 0; st->pads < 4 && fh1_gamepad_at(i, &g, &player); i++) {
        if (g.manual && player < 0) continue;
        const int k = st->pads++;
        st->pad[k].kind = g.kind;
        st->pad[k].rgb = g.rgb;
        st->pad[k].battery = g.battery ? (int8_t)(g.battery * 100 / 255) : -1;
    }
    st->sound = fh1_audio_available();
    st->volume = fh1_audio_master_volume();
}

void app_main(void)
{
    fh1_console_log_init();
    fh1_mem_diag_start();                      // where internal RAM goes (the console's "heap" op)
    ESP_LOGI(TAG, "FH1 starting");
    // The boot screen: each step reports what it found (shown once the display is up).
    fh1_post_begin(7);
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    fh1_post(FH1_POST_INFO, "FH1", "BIOS %s", esp_app_get_description()->version);
    fh1_post(FH1_POST_OK, "CPU", "S3 r%d.%d %dx%dMHz", chip.revision / 100, chip.revision % 100, chip.cores,
             CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    fh1_post(FH1_POST_OK, "Flash", "%luMB QIO %s", (unsigned long)(flash >> 20), CONFIG_ESPTOOLPY_FLASHFREQ);
    fh1_post(esp_psram_get_size() ? FH1_POST_OK : FH1_POST_FAIL, "PSRAM", "%uKB oct %dMHz",
             (unsigned)(esp_psram_get_size() / 1024), CONFIG_SPIRAM_SPEED);
    fh1_post(FH1_POST_OK, "RAM", "%uKB free", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    fh1_post_step();
    fh1_settings_init();
    fh1_i18n_init();
    fh1_post(FH1_POST_OK, "Settings", "NVS, lang %s", fh1_lang());
    fh1_post_step();
    // Local time: Brazil (UTC-3, no daylight saving) unless set otherwise.
    char tz[40];
    if (!fh1_settings_get_str("time", "tz", tz, sizeof(tz))) strlcpy(tz, "<-03>3", sizeof(tz));
    setenv("TZ", tz, 1);
    tzset();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    fh1_input_init();
    fh1_events_init();
    esp_err_t st = fh1_storage_init();
    fh1_fs_info_t fi = { 0 };
    if (st == ESP_OK) fh1_int_info(&fi);
    fh1_post(st == ESP_OK ? FH1_POST_OK : FH1_POST_FAIL, "Storage", "/int %llu/%lluKB", fi.used / 1024, fi.total / 1024);
    fh1_post_step();
    fh1_users_init();                  // users and their data folders (after storage and settings)
    fh1_post(FH1_POST_OK, "Users", "%d, unit %.4s", fh1_users_count(), fh1_users_unit_id());
    esp_err_t disp = fh1_board_init();
    fh1_post_display_ready();
    fh1_post(disp == ESP_OK ? FH1_POST_OK : FH1_POST_FAIL, "Board", "%s", fh1_board_name());
    fh1_post(FH1_POST_OK, "Screen", "%dx%d %s", fh1_board_display_mode()->w, fh1_board_display_mode()->h,
             fh1_board_display_mode()->id);
    fh1_post_step();
    fh1_dev_register(&s_devinput);
    fh1_dev_register(&s_wifi);
    fh1_dev_register(&s_bridge);
    fh1_dev_register(&s_bridge_net);
    fh1_dev_register(&s_baudio[0]);
    fh1_dev_register(&s_baudio[1]);
    fh1_post(FH1_POST_INFO, "Devices", "%s", "");
    fh1_dev_start_all();
    for (size_t i = 0; i < fh1_dev_count(); i++) {      // each device as probed: what it found, or why not
        fh1_device_t *d = fh1_dev_at(i);
        fh1_post(d->state == FH1_DEV_READY ? FH1_POST_OK : d->state == FH1_DEV_ABSENT ? FH1_POST_WARN :
                 d->state == FH1_DEV_DISABLED ? FH1_POST_OFF : FH1_POST_FAIL,
                 d->id, "%s", d->state == FH1_DEV_READY ? d->info : d->reason);
    }
    fh1_post_step();
    fh1_post(FH1_POST_OK, "Caps", "0x%04lx", (unsigned long)fh1_caps());
    fh1_post(FH1_POST_INFO, "UI", "LVGL %d.%d, starting", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR);
    fh1_post_step();
    fh1_post_end(900);                 // a moment to read it, then the window system draws over it
    ESP_ERROR_CHECK(fh1_ui_start());
    fh1_ui_set_status_fn(ui_status);
    fh1_ui_async(start_apps, NULL);
    serial_keys_start(on_command);
    esp_timer_handle_t t;
    esp_timer_create_args_t ta = { .callback = mark_valid, .name = "ota_valid" };
    if (esp_timer_create(&ta, &t) == ESP_OK) esp_timer_start_once(t, 10 * 1000000);
    ESP_LOGI(TAG, "board: %s, capabilities 0x%08lx, console code %s", fh1_board_name(), (unsigned long)fh1_caps(),
             fh1_console_token());
}
