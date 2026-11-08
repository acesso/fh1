#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "fh1_storage.h"
#include "esp_littlefs.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "storage";

// ── Internal partition ──

static void make_int_dirs(void)
{
    fh1_mkdirs(FH1_APPS_DIR);
    fh1_mkdirs(FH1_DATA_DIR);
    fh1_mkdirs(FH1_INT_PACKAGES);
}

esp_err_t fh1_storage_init(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = FH1_INT_PATH,
        .partition_label = "apps",
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "internal storage: %s", esp_err_to_name(err));
        return err;
    }
    make_int_dirs();
    fh1_fs_info_t fi;
    if (fh1_int_info(&fi) == ESP_OK) {
        ESP_LOGI(TAG, "internal: %llu of %llu KB used", fi.used / 1024, fi.total / 1024);
    }
    return ESP_OK;
}

// LittleFS counts its used blocks by walking the file system on flash, so the answer is cached:
// the dev console asks every second. Writes through FH1 code call fh1_int_info_changed().
static fh1_fs_info_t s_int_info;
static int64_t s_int_info_at = -1;

void fh1_int_info_changed(void) { s_int_info_at = -1; }

esp_err_t fh1_int_info(fh1_fs_info_t *info)
{
    int64_t now = esp_timer_get_time();
    if (s_int_info_at < 0 || now - s_int_info_at > 10 * 1000000) {
        size_t total = 0, used = 0;
        esp_err_t err = esp_littlefs_info("apps", &total, &used);
        if (err != ESP_OK) return err;
        s_int_info.total = total;
        s_int_info.used = used;
        s_int_info_at = now;
    }
    *info = s_int_info;
    return ESP_OK;
}

esp_err_t fh1_int_format(void)
{
    ESP_LOGW(TAG, "formatting the internal apps partition");
    esp_err_t err = esp_littlefs_format("apps");
    if (err == ESP_OK) make_int_dirs();
    fh1_int_info_changed();
    return err;
}

// ── Helpers ──

esp_err_t fh1_mkdirs(const char *path)
{
    char tmp[128];
    strlcpy(tmp, path, sizeof(tmp));
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0775);
            *p = '/';
        }
    }
    if (mkdir(tmp, 0775) != 0) {
        struct stat st;
        if (stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode)) return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t fh1_mkdirs_parent(const char *file)
{
    char tmp[128];
    strlcpy(tmp, file, sizeof(tmp));
    char *slash = strrchr(tmp, '/');
    if (!slash || slash == tmp) return ESP_OK;
    *slash = 0;
    return fh1_mkdirs(tmp);
}

static fh1_data_root_fn s_data_root;

void fh1_set_data_root_fn(fh1_data_root_fn fn) { s_data_root = fn; }

void fh1_data_dir(const char *app, char *out, size_t n)
{
    char root[64] = FH1_DATA_DIR;
    if (s_data_root) s_data_root(root, sizeof(root));
    snprintf(out, n, "%s/%s", root, app);
}

esp_err_t fh1_rmtree(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return ESP_OK;
    if (!S_ISDIR(st.st_mode)) return unlink(path) == 0 ? ESP_OK : ESP_FAIL;
    DIR *dir = opendir(path);
    if (!dir) return ESP_FAIL;
    struct dirent *e;
    char child[384];
    while ((e = readdir(dir)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (snprintf(child, sizeof(child), "%s/%.255s", path, e->d_name) >= (int)sizeof(child)) continue;
        fh1_rmtree(child);
    }
    closedir(dir);
    return rmdir(path) == 0 ? ESP_OK : ESP_FAIL;
}

// ── SD card ──
// No card-detect line on the dev board: while no card is mounted, the poll tries to mount one
// every few seconds; while mounted, it checks the card still answers.

static SemaphoreHandle_t s_sd_lock;
static sdmmc_card_t *s_card;
static bool s_bus_ready, s_ejected;
static int s_poll_skip;
static fh1_device_t *s_sd_dev;

static void sd_quiet(bool quiet)
{
    static const char *const tags[] = { "sdmmc_common", "sdmmc_init", "sdmmc_sd", "sdspi_host", "sdspi_transaction", "vfs_fat_sdmmc", "gpio" };
    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++) {
        esp_log_level_set(tags[i], quiet ? ESP_LOG_NONE : ESP_LOG_WARN);
    }
}

static esp_err_t sd_try_mount(fh1_device_t *d, bool quiet)
{
    const fh1_sd_spi_cfg_t *cfg = d->cfg;
    if (!s_bus_ready) {
        spi_bus_config_t bus = {
            .mosi_io_num = cfg->mosi, .miso_io_num = cfg->miso, .sclk_io_num = cfg->sclk,
            .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 4096,
        };
        esp_err_t err = spi_bus_initialize(cfg->host, &bus, SPI_DMA_CH_AUTO);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
        s_bus_ready = true;
    }
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = cfg->host;
    host.max_freq_khz = 20000;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = cfg->host;
    slot.gpio_cs = cfg->cs;
    esp_vfs_fat_sdmmc_mount_config_t mc = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };
    if (quiet) sd_quiet(true);
    esp_err_t err = esp_vfs_fat_sdspi_mount(FH1_SD_PATH, &host, &slot, &mc, &s_card);
    if (quiet) sd_quiet(false);
    if (err != ESP_OK) {
        s_card = NULL;
        return ESP_ERR_NOT_FOUND;
    }
    snprintf(d->info, sizeof(d->info), "%s, %llu MB, SPI %lu kHz", s_card->cid.name[0] ? s_card->cid.name : "SD card",
             (unsigned long long)s_card->csd.capacity * s_card->csd.sector_size / (1024 * 1024),
             (unsigned long)s_card->real_freq_khz);
    fh1_mkdirs(FH1_SD_PACKAGES);
    ESP_LOGI(TAG, "SD mounted: %s", d->info);
    return ESP_OK;
}

static void sd_unmount(void)
{
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(FH1_SD_PATH, s_card);
        s_card = NULL;
    }
}

static esp_err_t sd_probe(fh1_device_t *d)
{
    if (!s_sd_lock) s_sd_lock = xSemaphoreCreateMutex();
    s_sd_dev = d;
    xSemaphoreTake(s_sd_lock, portMAX_DELAY);
    esp_err_t err = s_card ? ESP_OK : (s_ejected ? ESP_ERR_NOT_FOUND : sd_try_mount(d, true));
    xSemaphoreGive(s_sd_lock);
    return err;
}

static void sd_stop(fh1_device_t *d)
{
    if (!s_sd_lock) s_sd_lock = xSemaphoreCreateMutex();
    xSemaphoreTake(s_sd_lock, portMAX_DELAY);
    sd_unmount();
    xSemaphoreGive(s_sd_lock);
}

static void sd_poll(fh1_device_t *d)
{
    xSemaphoreTake(s_sd_lock, portMAX_DELAY);
    if (s_card) {
        if (sdmmc_get_status(s_card) != ESP_OK) {
            ESP_LOGW(TAG, "SD card removed");
            sd_unmount();
            xSemaphoreGive(s_sd_lock);
            fh1_dev_report(d, FH1_DEV_ABSENT, "card removed");
            return;
        }
    } else if (!s_ejected && ++s_poll_skip >= 3) {
        s_poll_skip = 0;
        if (sd_try_mount(d, true) == ESP_OK) {
            xSemaphoreGive(s_sd_lock);
            fh1_dev_report(d, FH1_DEV_READY, "");
            return;
        }
    }
    xSemaphoreGive(s_sd_lock);
}

const fh1_driver_t fh1_sd_spi_driver = {
    .name = "sdspi",
    .probe = sd_probe,
    .stop = sd_stop,
    .poll = sd_poll,
};

void fh1_sd_info(fh1_sd_info_t *info)
{
    memset(info, 0, sizeof(*info));
    if (!s_sd_lock) return;
    xSemaphoreTake(s_sd_lock, portMAX_DELAY);
    info->ejected = s_ejected;
    if (s_card) {
        info->mounted = true;
        strlcpy(info->name, s_card->cid.name, sizeof(info->name));
        info->speed_khz = s_card->real_freq_khz;
        info->capacity = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
        esp_vfs_fat_info(FH1_SD_PATH, &info->total, &info->free);
    }
    xSemaphoreGive(s_sd_lock);
}

esp_err_t fh1_sd_mount(void)
{
    if (!s_sd_dev) return ESP_ERR_INVALID_STATE;
    if (s_sd_dev->state == FH1_DEV_DISABLED) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_sd_lock, portMAX_DELAY);
    s_ejected = false;
    esp_err_t err = s_card ? ESP_OK : sd_try_mount(s_sd_dev, false);
    xSemaphoreGive(s_sd_lock);
    fh1_dev_report(s_sd_dev, err == ESP_OK ? FH1_DEV_READY : FH1_DEV_ABSENT, err == ESP_OK ? "" : "no card");
    return err;
}

esp_err_t fh1_sd_eject(void)
{
    if (!s_sd_dev) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_sd_lock, portMAX_DELAY);
    sd_unmount();
    s_ejected = true;
    xSemaphoreGive(s_sd_lock);
    fh1_dev_report(s_sd_dev, FH1_DEV_ABSENT, "ejected");
    ESP_LOGI(TAG, "SD ejected");
    return ESP_OK;
}

esp_err_t fh1_sd_format(void)
{
    if (!s_sd_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_sd_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_card) {
        ESP_LOGW(TAG, "formatting the SD card");
        err = esp_vfs_fat_sdcard_format(FH1_SD_PATH, s_card);
        if (err == ESP_OK) fh1_mkdirs(FH1_SD_PACKAGES);
    }
    xSemaphoreGive(s_sd_lock);
    return err;
}
