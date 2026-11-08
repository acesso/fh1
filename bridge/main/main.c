// The FH1 extension board: Bluepad32 holds the Bluetooth controllers; bridge.c is our platform (what we do
// with them). BTstack's run loop runs here and does not return.
#include <btstack_port_esp32.h>
#include <btstack_run_loop.h>
#include <btstack_stdio_esp32.h>
#include <uni.h>

#include "sdkconfig.h"

#ifndef CONFIG_BLUEPAD32_PLATFORM_CUSTOM
#error "Must use BLUEPAD32_PLATFORM_CUSTOM"
#endif

struct uni_platform *bridge_platform(void);

int app_main(void)
{
#ifdef CONFIG_ESP_CONSOLE_UART
#ifndef CONFIG_BLUEPAD32_USB_CONSOLE_ENABLE
    btstack_stdio_init();             // not BTstack's buffered UART: it conflicts with the console
#endif
#endif
    btstack_init();                   // BTstack on the ESP32's controller (VHCI)
    uni_platform_set_custom(bridge_platform());
    uni_init(0, NULL);
    btstack_run_loop_execute();
    return 0;
}
