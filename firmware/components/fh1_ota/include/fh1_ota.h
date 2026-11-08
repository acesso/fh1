// Firmware updates (firmware/PLAN.md, "Firmware updates"): the image goes to the spare OTA slot, then the device
// restarts into it. With rollback enabled, a new image must mark itself valid once it is up
// (fh1_ota_mark_valid); if it crashes before that, the bootloader returns to the previous one.
#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct fh1_ota fh1_ota_t;

// Streaming, for uploads. size may be 0 if unknown.
esp_err_t fh1_ota_begin(size_t size, fh1_ota_t **out, char *err, size_t errlen);
esp_err_t fh1_ota_write(fh1_ota_t *o, const void *data, size_t len, char *err, size_t errlen);
esp_err_t fh1_ota_finish(fh1_ota_t *o, char *err, size_t errlen);    // checks the image, sets it to boot next
void fh1_ota_abort(fh1_ota_t *o);

// From a file (the SD card). progress(pct) is called as it goes.
esp_err_t fh1_ota_from_file(const char *path, void (*progress)(int pct, void *user), void *user, char *err, size_t errlen);

// Peek at a firmware file: its version and project name, without writing anything.
esp_err_t fh1_ota_peek_file(const char *path, char *version, size_t vlen, char *err, size_t errlen);

void fh1_ota_mark_valid(void);              // call once the system is up
const char *fh1_ota_running_version(void);
