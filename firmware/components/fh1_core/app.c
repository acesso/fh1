#include "fh1_app.h"
#include "fh1_events.h"
#include "esp_system.h"
#include "esp_timer.h"

static volatile bool s_yield, s_quit;

void fh1_app_request_yield(bool on) { s_yield = on; }
bool fh1_app_should_yield(void) { return s_yield; }
void fh1_app_request_quit(void) { s_quit = true; }
bool fh1_app_quit_requested(void) { return s_quit; }

static void force_restart(void *arg) { esp_restart(); }

void fh1_system_restart(void)
{
    static esp_timer_handle_t t;
    if (t) return;                                      // already on its way
    esp_timer_create_args_t a = { .callback = force_restart, .name = "restart" };
    if (esp_timer_create(&a, &t) == ESP_OK) esp_timer_start_once(t, 8 * 1000000);
    for (int i = 0; i < 50 && fh1_event_post(FH1_EV_SHUTDOWN, NULL, 0) != ESP_OK; i++) {}
}
