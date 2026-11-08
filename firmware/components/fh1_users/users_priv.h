// Shared between users.c and users_social.c (not for other components).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "fh1_users.h"

#define USERS_DIR "/int/users"
#define USERS_NS "users"

// Canonical CBOR, written and read.
typedef struct {
    uint8_t *p;
    size_t n, max;
    bool ok;
} wbuf_t;
typedef struct {
    const uint8_t *p;
    size_t n, i;
} rbuf_t;
void users_w_raw(wbuf_t *w, const void *d, size_t k);
void users_w_head(wbuf_t *w, uint8_t major, uint32_t v);
void users_w_data(wbuf_t *w, uint8_t major, const void *d, size_t k);
bool users_r_head(rbuf_t *r, uint8_t major, uint32_t *v);
bool users_r_data(rbuf_t *r, uint8_t major, const uint8_t **d, uint32_t *len, uint32_t max);
bool users_r_key(rbuf_t *r, uint32_t want);

void users_changed(void);
int users_find(const char *id);                           // index in the list, or -1
void users_id_of(const uint8_t pk[32], char out[FH1_USER_ID_LEN + 1], uint8_t raw[10]);
esp_err_t users_seed_get(const char *id, const char *pin, uint8_t seed[32]);
void users_key_pair(const uint8_t seed[32], uint8_t sk[64], uint8_t pk[32]);
bool users_rec_load(const char *id, uint8_t *rec, size_t *n);
bool users_name_ok(const char *s, size_t n);
void users_social_init(void);                             // users_social.c: contacts
