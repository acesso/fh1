// FH1 device manager: every peripheral is a device in one registry (firmware/PLAN.md, "Devices and capabilities").
// The board registers candidate devices; drivers probe them; the user can enable or disable each one;
// capabilities are derived from the devices that are ready.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FH1_DEV_ABSENT,     // declared by the board, nothing found (or a hot-plug device not connected)
    FH1_DEV_READY,      // probed and started
    FH1_DEV_DISABLED,   // switched off by the user
    FH1_DEV_FAILED,     // found but not working
} fh1_dev_state_t;

// Capability flags. Keep in sync with fh1_cap_names in devices.c and with fh1_sdk.h.
enum {
    FH1_CAP_DISPLAY      = 1u << 0,
    FH1_CAP_BACKLIGHT    = 1u << 1,
    FH1_CAP_BUTTONS      = 1u << 2,
    FH1_CAP_STICK_ANALOG = 1u << 3,
    FH1_CAP_TOUCH        = 1u << 4,
    FH1_CAP_AUDIO_OUT    = 1u << 5,
    FH1_CAP_AUDIO_IN     = 1u << 6,
    FH1_CAP_SD           = 1u << 7,
    FH1_CAP_WIFI         = 1u << 8,
    FH1_CAP_BLE          = 1u << 9,
    FH1_CAP_LORA         = 1u << 10,
    FH1_CAP_IMU          = 1u << 11,
    FH1_CAP_RTC          = 1u << 12,
    FH1_CAP_BATTERY      = 1u << 13,
    FH1_CAP_BUTTON_LIGHTS = 1u << 14,
    FH1_CAP_COUNT        = 15,
};

typedef struct fh1_device fh1_device_t;

typedef struct {
    const char *name;                         // driver name, e.g. "gt911"
    esp_err_t (*probe)(fh1_device_t *d);      // is the chip there? ESP_OK if so
    esp_err_t (*start)(fh1_device_t *d);      // bring it up (optional)
    void (*stop)(fh1_device_t *d);            // shut it down and power it off where possible (optional)
    void (*poll)(fh1_device_t *d);            // called every 2 s while enabled: hot-plug and health (optional)
    const void *ops;                          // the class interface, e.g. fh1_audio_ops_t
} fh1_driver_t;

struct fh1_device {
    const char *id;            // "sd0"
    const char *cls;           // "storage.sd"
    uint32_t caps;             // capabilities it provides while ready
    const fh1_driver_t *drv;
    const void *cfg;           // board-specific configuration
    void *priv;                // driver state
    bool default_on;           // enabled unless the user switched it off
    bool essential;            // cannot be disabled (the display)
    uint16_t ma;               // rough current draw while on, mA
    // Managed by the device manager:
    fh1_dev_state_t state;
    char info[56];             // what the driver found ("I2C 0x5D, GT911")
    char reason[40];           // why absent / failed / disabled
};

ESP_EVENT_DECLARE_BASE(FH1_EVENT);
enum {
    FH1_EVT_DEVICE_CHANGED,    // data: int, the device index
    FH1_EVT_CAPS_CHANGED,      // data: uint32_t, the new capability set
};

void fh1_dev_register(fh1_device_t *d);
void fh1_dev_start_all(void);                  // probe and start every registered device, then start the poll task

esp_err_t fh1_dev_set_enabled(const char *id, bool enabled);
esp_err_t fh1_dev_reprobe(const char *id);
// For drivers: report a state change found while polling (card removed, chip stopped answering).
void fh1_dev_report(fh1_device_t *d, fh1_dev_state_t state, const char *reason);

size_t fh1_dev_count(void);
fh1_device_t *fh1_dev_at(size_t i);
fh1_device_t *fh1_dev_find(const char *id);
fh1_device_t *fh1_dev_ready_of_class(const char *cls);   // first ready device of a class, or NULL
bool fh1_dev_enabled_setting(const fh1_device_t *d);

uint32_t fh1_caps(void);
uint32_t fh1_dev_generation(void);             // changes whenever any device changes state
const char *fh1_cap_name(uint32_t cap_bit);
uint32_t fh1_cap_from_name(const char *name);  // 0 if unknown
const char *fh1_dev_state_name(fh1_dev_state_t s);

#ifdef __cplusplus
}
#endif
