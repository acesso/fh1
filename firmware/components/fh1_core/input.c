#include <string.h>
#include "fh1_input.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static QueueHandle_t s_q;
static volatile uint32_t s_down;
static const char *const s_names[FH1_KEY_COUNT] = {
    "UP", "DOWN", "LEFT", "RIGHT", "A", "B", "X", "Y", "START", "SELECT", "MENU",
};

void fh1_input_init(void)
{
    if (!s_q) s_q = xQueueCreate(32, sizeof(fh1_input_event_t));
}

void fh1_input_post(fh1_key_t key, bool pressed)
{
    if (key >= FH1_KEY_COUNT || !s_q) return;
    if (pressed) s_down |= 1u << key; else s_down &= ~(1u << key);
    fh1_input_event_t ev = { .key = key, .pressed = pressed };
    xQueueSend(s_q, &ev, 0);
}

bool fh1_input_get(fh1_input_event_t *ev)
{
    return s_q && xQueueReceive(s_q, ev, 0) == pdTRUE;
}

void fh1_input_flush(void)
{
    if (s_q) xQueueReset(s_q);
}

bool fh1_input_is_down(fh1_key_t key) { return key < FH1_KEY_COUNT && (s_down & (1u << key)); }
const char *fh1_key_name(fh1_key_t key) { return key < FH1_KEY_COUNT ? s_names[key] : "?"; }

int fh1_key_from_name(const char *name)
{
    for (int i = 0; i < FH1_KEY_COUNT; i++) {
        if (strcasecmp(name, s_names[i]) == 0) return i;
    }
    return -1;
}
