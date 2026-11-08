// Small NVS wrappers. Values live in named namespaces ("devices", "wifi", "console", ...).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

void fh1_settings_init(void);
// Removes every key of a namespace; false if it had none.
bool fh1_settings_erase(const char *ns);
uint8_t fh1_settings_get_u8(const char *ns, const char *key, uint8_t def);
void fh1_settings_set_u8(const char *ns, const char *key, uint8_t val);
uint32_t fh1_settings_get_u32(const char *ns, const char *key, uint32_t def);
void fh1_settings_set_u32(const char *ns, const char *key, uint32_t val);
// Copies the value into buf (NUL-terminated); returns false and leaves buf empty if missing.
bool fh1_settings_get_str(const char *ns, const char *key, char *buf, size_t len);
void fh1_settings_set_str(const char *ns, const char *key, const char *val);
// Binary values. get: *len is the buffer's size in, the value's size out (0 and false if missing or too big).
bool fh1_settings_get_blob(const char *ns, const char *key, void *buf, size_t *len);
bool fh1_settings_set_blob(const char *ns, const char *key, const void *buf, size_t len);
void fh1_settings_erase_key(const char *ns, const char *key);
