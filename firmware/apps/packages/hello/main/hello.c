// Hello FH1: the first packaged app. It shows the SDK version and the device's capabilities,
// counts presses into its data folder, and tries to beep: with no sound device it says so and
// carries on, which is the point of the capability model. SDK 0.4: it greets player 1's user with their
// face, keeps the count as a saved state and the visits as a setting (both per user), and notifies.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "fh1_sdk.h"

static lv_obj_t *s_count_label, *s_up_label;
static int s_count;
static uint32_t s_started;

static void save(void) { fh1_state_save("count", &s_count, sizeof(s_count)); }

static void show_count(void)
{
    char t[48];
    snprintf(t, sizeof(t), "Pressed %d time%s (saved)", s_count, s_count == 1 ? "" : "s");
    fh1_ui_label_set(s_count_label, t);
}

static void on_count(void *user)
{
    s_count++;
    save();
    show_count();
}

static void on_beep(void *user)
{
    static int16_t tone[1600];          // 0.1 s at 16 kHz, 800 Hz square wave
    for (int i = 0; i < 1600; i++) tone[i] = (i / 10) % 2 ? 6000 : -6000;
    fh1_audio_t *a = fh1_audio_open(16000);
    int r = fh1_audio_write(a, tone, 1600);
    fh1_audio_close(a);
    fh1_ui_message("Beep", r == FH1_UNAVAILABLE ? "No sound device here, so the app carries on silently."
                                                 : "Beep!");
}

static void on_quit(void *user) { fh1_app_exit(); }

static void on_notify(void *user)
{
    char t[64];
    snprintf(t, sizeof(t), "pressed Count %d time%s here", s_count, s_count == 1 ? "" : "s");
    fh1_notify(t);
}

static void tick(void *user)
{
    char t[40];
    snprintf(t, sizeof(t), "Running for %lu s", (unsigned long)((fh1_millis() - s_started) / 1000));
    fh1_ui_label_set(s_up_label, t);
}

int app_main(int argc, char *argv[])
{
    s_started = fh1_millis();
    if (fh1_state_load("count", &s_count, sizeof(s_count)) != sizeof(s_count)) s_count = 0;
    // Visits, as a setting (text).
    char vs[16] = "0";
    fh1_setting_get("visits", vs, sizeof(vs));
    int visits = atoi(vs) + 1;
    snprintf(vs, sizeof(vs), "%d", visits);
    fh1_setting_set("visits", vs);

    fh1_win_t *w = fh1_app_window("Hello FH1", 240, 200);
    lv_obj_t *body = fh1_win_body(w);
    // Who plays: their face and name (nothing when the FH1 has no users).
    char name[24], hi[64];
    if (fh1_user(0, name, sizeof(name))) {
        fh1_user_face(body, 0, 32);
        snprintf(hi, sizeof(hi), "Hello, %s! Visit %d.", name, visits);
    } else {
        snprintf(hi, sizeof(hi), "Hello! Visit %d.", visits);
    }
    fh1_ui_label(body, hi);
    uint32_t v = fh1_sdk_version();
    char caps[160] = "", t[240];
    for (int i = 0; i < 15; i++) {
        if (fh1_caps() & (1u << i)) {
            strlcat(caps, fh1_cap_name(1u << i), sizeof(caps));
            strlcat(caps, " ", sizeof(caps));
        }
    }
    snprintf(t, sizeof(t), "Packaged app on SDK %lu.%lu\nCapabilities: %s", (unsigned long)(v >> 16),
             (unsigned long)(v & 0xffff), caps);
    fh1_ui_label(body, t);
    s_count_label = fh1_ui_label(body, "");
    show_count();
    s_up_label = fh1_ui_label(body, "");
    fh1_ui_button(w, NULL, "Count", on_count, NULL);
    fh1_ui_button(w, NULL, "Beep", on_beep, NULL);
    fh1_ui_button(w, NULL, "Notify", on_notify, NULL);
    fh1_ui_button(w, NULL, "Quit", on_quit, NULL);
    fh1_app_timer(1000, tick, NULL);
    tick(NULL);
    fh1_log("started, count %d", s_count);
    return 0;
}
