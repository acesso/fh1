// Events: anything can post one (a task, a timer, a driver); subscribers run in the UI task, in order.
// Recurring events can fire at a random interval within a range. Kinds that are not recurring (a LoRa
// message arrives, the battery runs low) use the same post/subscribe path.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

typedef enum {
    FH1_EV_UI_IDLE_FLOURISH,    // decoration: a corner ball shakes (recurring, random interval)
    FH1_EV_BATTERY_LOW,         // data: int, percent
    FH1_EV_MESSAGE,             // data: text, from a network or radio app
    FH1_EV_DEVICE_CHANGED,      // data: int, device index
    FH1_EV_TRANSFER,            // data: fh1_transfer_t; a file or firmware arriving (fh1_transfer_report)
    FH1_EV_SHUTDOWN,            // the system restarts (fh1_system_restart): apps close, windows close
    FH1_EV_COUNT,
} fh1_event_id_t;

typedef void (*fh1_event_cb)(fh1_event_id_t id, const void *data, size_t len, void *user);

void fh1_events_init(void);
esp_err_t fh1_event_post(fh1_event_id_t id, const void *data, size_t len);   // from any task; data copied (<= 64 bytes)
esp_err_t fh1_event_subscribe(fh1_event_id_t id, fh1_event_cb cb, void *user);
// Fire id again and again, each time after a random delay between min_ms and max_ms.
esp_err_t fh1_event_every(fh1_event_id_t id, uint32_t min_ms, uint32_t max_ms);
// Transfers: the UI shows one at a time in an overlay with its progress.
typedef enum { FH1_XFER_UPLOAD, FH1_XFER_FIRMWARE } fh1_xfer_kind_t;
typedef enum { FH1_XFER_RUNNING, FH1_XFER_DONE, FH1_XFER_FAILED } fh1_xfer_state_t;
typedef struct {
    uint8_t kind;               // fh1_xfer_kind_t
    uint8_t state;              // fh1_xfer_state_t
    uint8_t pct;
    char name[48];              // the file, or "" (firmware over the network)
} fh1_transfer_t;
// From the task doing the transfer, as often as it likes: progress is posted when it moves a step (or
// 250 ms later), the end always (it waits for room in the queue).
void fh1_transfer_report(fh1_xfer_kind_t kind, fh1_xfer_state_t state, const char *name, uint64_t done, uint64_t total);

// Called by the UI task: deliver what is queued.
void fh1_events_dispatch(void);
