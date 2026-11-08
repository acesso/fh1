// Storage: the internal apps partition (LittleFS at /int) and the micro-SD card (FAT at /sd).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "fh1_devices.h"

#define FH1_INT_PATH "/int"
#define FH1_SD_PATH  "/sd"
#define FH1_INT_PACKAGES FH1_INT_PATH "/packages"   // packages staged on the device (uploaded or downloaded)
#define FH1_SD_PACKAGES  FH1_SD_PATH "/packages"
#define FH1_APPS_DIR     FH1_INT_PATH "/apps"       // installed apps, one folder each
#define FH1_DATA_DIR     FH1_INT_PATH "/data"       // app data, one folder each

esp_err_t fh1_storage_init(void);     // mounts /int, formatting it on first boot

typedef struct {
    uint64_t total, used;
} fh1_fs_info_t;
esp_err_t fh1_int_info(fh1_fs_info_t *info);   // cached for up to 10 s
void fh1_int_info_changed(void);                // after writing: refresh on the next call
esp_err_t fh1_int_format(void);       // erases installed apps, their data and staged packages

// The SD card slot, as a device driver (storage.sd). The board passes a fh1_sd_spi_cfg_t as cfg.
typedef struct {
    int host;                         // spi_host_device_t
    int mosi, miso, sclk, cs;
} fh1_sd_spi_cfg_t;
extern const fh1_driver_t fh1_sd_spi_driver;

typedef struct {
    bool mounted;
    bool ejected;                     // unmounted by the user; stays out until "mount"
    char name[12];
    uint32_t speed_khz;
    uint64_t capacity, total, free;   // bytes
} fh1_sd_info_t;
void fh1_sd_info(fh1_sd_info_t *info);
esp_err_t fh1_sd_mount(void);
esp_err_t fh1_sd_eject(void);
esp_err_t fh1_sd_format(void);        // FAT, then recreates /sd/packages

esp_err_t fh1_mkdirs(const char *path);
esp_err_t fh1_mkdirs_parent(const char *file);   // the folders above a file
esp_err_t fh1_rmtree(const char *path);

// App data: <root>/<app>. The root is FH1_DATA_DIR until users exist; then it is the data folder of the user
// playing as player 1 (fh1_users sets the function), so each user keeps their own saves.
typedef void (*fh1_data_root_fn)(char *out, size_t n);
void fh1_set_data_root_fn(fh1_data_root_fn fn);
void fh1_data_dir(const char *app, char *out, size_t n);
