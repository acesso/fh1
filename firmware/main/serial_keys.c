// Keys over the serial console, the dev board's fallback input (tools/keys.py sends them).
// Single characters are key taps; a line starting with '!' is a command (see console_cmd()).
#include <string.h>
#include "serial_keys.h"
#include "fh1_input.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static console_cmd_fn s_cmd;

static int key_for(char c)
{
    switch (c) {
    case 'w': return FH1_KEY_UP;
    case 's': return FH1_KEY_DOWN;
    case 'a': return FH1_KEY_LEFT;
    case 'd': return FH1_KEY_RIGHT;
    case 'j': case 'z': return FH1_KEY_A;
    case 'k': case 'x': return FH1_KEY_B;
    case 'u': return FH1_KEY_X;
    case 'i': return FH1_KEY_Y;
    case '\r': case '\n': return FH1_KEY_START;
    case ' ': return FH1_KEY_SELECT;
    case 'm': case '\t': return FH1_KEY_MENU;
    }
    return -1;
}

static void tap(int key)
{
    fh1_input_post(key, true);
    vTaskDelay(pdMS_TO_TICKS(20));
    fh1_input_post(key, false);
}

static void serial_task(void *arg)
{
    char line[160];
    int n = 0, esc = 0;
    bool in_cmd = false;
    for (;;) {
        uint8_t c;
        if (uart_read_bytes(UART_NUM_0, &c, 1, portMAX_DELAY) != 1) continue;
        if (in_cmd) {
            if (c == '\r' || c == '\n') {
                line[n] = 0;
                in_cmd = false;
                if (s_cmd) s_cmd(line);
            } else if (n < (int)sizeof(line) - 1) {
                line[n++] = c;
            }
            continue;
        }
        if (c == '!') { in_cmd = true; n = 0; continue; }
        // Arrow keys arrive as ESC [ A..D
        if (esc == 0 && c == 0x1b) { esc = 1; continue; }
        if (esc == 1) { esc = (c == '[') ? 2 : 0; continue; }
        if (esc == 2) {
            esc = 0;
            int k = c == 'A' ? FH1_KEY_UP : c == 'B' ? FH1_KEY_DOWN : c == 'C' ? FH1_KEY_RIGHT : c == 'D' ? FH1_KEY_LEFT : -1;
            if (k >= 0) tap(k);
            continue;
        }
        int k = key_for((char)c);
        if (k >= 0) tap(k);
    }
}

void serial_keys_start(console_cmd_fn on_command)
{
    s_cmd = on_command;
    uart_driver_install(UART_NUM_0, 512, 0, 0, NULL, 0);
    xTaskCreatePinnedToCore(serial_task, "serial_keys", 4096, NULL, 2, NULL, 0);
}
