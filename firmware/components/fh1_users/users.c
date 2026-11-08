// Users: identities, records, PINs, controllers, players, backups (fh1_users.h; firmware/docs/users.md).
//
// Where things live:
//   NVS "users"   unit      the unit's key seed (32 bytes)
//                 owner     the owner's id
//                 list      the users' ids, 16 characters each
//                 p<id14>   a user's signed record (CBOR)
//                 m<id14>   what the user is here: kind (1 byte)
//                 k<id14>   the private key seed: 32 bytes, or 89 locked with a PIN (below)
//                 pads      controller links: pad id (18) + user id (17) each
//   /int/users/<id>/data/<app>/   the user's app data (fh1_data_dir)
// Identities are in NVS so formatting the internal storage keeps them; their data goes with the format.
// (<id14>: the id's first 14 characters; NVS keys are at most 15.)
//
// The record, canonical CBOR (shortest heads, keys in order), signed (Ed25519) over the same map without key 6:
//   { 0: version 1, 1: public key (32 bytes), 2: name (text), 3: avatar (bytes), 4: rev, 5: unit id (10 bytes),
//     6: signature (64 bytes) }
// A locked key: [1][salt 16][nonce 24][mac 16][seed 32 encrypted], key = Argon2id(PIN, salt; 256 KB, 3 passes),
// XChaCha20-Poly1305 with the user id as associated data.
// A backup file: "FH1U" [1][KiB u16][passes][salt 16][nonce 24][length u16] [mac 16][encrypted: seed 32, flags 1,
// record]; key = Argon2id(passphrase, salt; 1 MB, 3 passes); the 50-byte header is the associated data.
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "fh1_users.h"
#include "users_priv.h"
#include "fh1_settings.h"
#include "fh1_storage.h"
#include "fh1_gamepad.h"
#include "monocypher.h"
#include "monocypher-ed25519.h"
#include "psa/crypto.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "users";

#define NS "users"
#define REC_VERSION 1
#define REC_MAX 200
#define LOCK_LEN 89
#define PIN_BLOCKS 256                  // KiB of Argon2 memory for a PIN
#define BACKUP_BLOCKS 1024              // and for a backup's passphrase
#define KDF_PASSES 3
#define LINKS_MAX 24
#define BACKUP_HDR 50
#define BACKUP_MAX (BACKUP_HDR + 16 + 33 + REC_MAX)

static fh1_user_t s_users[FH1_USERS_MAX];
static int s_n;
static char s_owner[FH1_USER_ID_LEN + 1], s_use[FH1_USER_ID_LEN + 1], s_unit[FH1_USER_ID_LEN + 1];
static uint8_t s_unit_raw[10];
typedef struct {
    char pad[18];
    char user[FH1_USER_ID_LEN + 1];
} link_t;
static link_t s_links[LINKS_MAX];
static int s_nlinks;
static struct {
    void (*cb)(void *);
    void *arg;
} s_watch[4];

void users_changed(void)
{
    for (int i = 0; i < 4; i++)
        if (s_watch[i].cb) s_watch[i].cb(s_watch[i].arg);
}

void fh1_users_on_change(void (*cb)(void *arg), void *arg)
{
    for (int i = 0; i < 4; i++) {
        if (!s_watch[i].cb) {
            s_watch[i].cb = cb;
            s_watch[i].arg = arg;
            return;
        }
    }
}

// ── Ids ──

static const char B32[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";      // Crockford: no I, L, O, U

void users_id_of(const uint8_t pk[32], char out[FH1_USER_ID_LEN + 1], uint8_t raw[10])
{
    uint8_t h[32];
    size_t hl = 0;
    psa_hash_compute(PSA_ALG_SHA_256, pk, 32, h, sizeof(h), &hl);
    for (int i = 0, bit = 0; i < FH1_USER_ID_LEN; i++) {
        int v = 0;
        for (int b = 0; b < 5; b++, bit++) v = v << 1 | ((h[bit / 8] >> (7 - bit % 8)) & 1);
        out[i] = B32[v];
    }
    out[FH1_USER_ID_LEN] = 0;
    if (raw) memcpy(raw, h, 10);
}

static void nvs_key(char kind, const char *id, char out[16])
{
    out[0] = kind;
    memcpy(out + 1, id, 14);
    out[15] = 0;
}

// ── Keys ──

static bool kdf(const char *secret, const uint8_t salt[16], uint32_t blocks, uint32_t passes, uint8_t key[32])
{
    void *work = heap_caps_malloc((size_t)blocks * 1024, MALLOC_CAP_SPIRAM);
    if (!work) work = malloc((size_t)blocks * 1024);
    if (!work) return false;
    const int64_t t0 = esp_timer_get_time();
    crypto_argon2_config cfg = { .algorithm = CRYPTO_ARGON2_ID, .nb_blocks = blocks, .nb_passes = passes, .nb_lanes = 1 };
    crypto_argon2_inputs in = { .pass = (const uint8_t *)secret, .salt = salt, .pass_size = strlen(secret), .salt_size = 16 };
    crypto_argon2(key, 32, work, cfg, in, crypto_argon2_no_extras);
    free(work);
    ESP_LOGI(TAG, "key derived: %lu KiB, %lu passes, %lld ms", (unsigned long)blocks, (unsigned long)passes,
             (esp_timer_get_time() - t0) / 1000);
    return true;
}

// The private key seed. ESP_ERR_NOT_FOUND: none here; ESP_ERR_INVALID_CRC: a wrong (or missing) PIN.
esp_err_t users_seed_get(const char *id, const char *pin, uint8_t seed[32])
{
    uint8_t blob[LOCK_LEN];
    size_t n = sizeof(blob);
    char k[16];
    nvs_key('k', id, k);
    if (!fh1_settings_get_blob(NS, k, blob, &n)) return ESP_ERR_NOT_FOUND;
    if (n == 32) {
        memcpy(seed, blob, 32);
        crypto_wipe(blob, sizeof(blob));
        return ESP_OK;
    }
    if (n != LOCK_LEN || blob[0] != 1) return ESP_ERR_NOT_FOUND;
    if (!pin || !pin[0]) return ESP_ERR_INVALID_CRC;
    uint8_t key[32];
    if (!kdf(pin, blob + 1, PIN_BLOCKS, KDF_PASSES, key)) return ESP_ERR_NO_MEM;
    const int r = crypto_aead_unlock(seed, blob + 41, key, blob + 17, (const uint8_t *)id, FH1_USER_ID_LEN, blob + 57, 32);
    crypto_wipe(key, sizeof(key));
    return r == 0 ? ESP_OK : ESP_ERR_INVALID_CRC;
}

static bool seed_put(const char *id, const uint8_t seed[32], const char *pin)
{
    char k[16];
    nvs_key('k', id, k);
    if (!pin || !pin[0]) return fh1_settings_set_blob(NS, k, seed, 32);
    uint8_t blob[LOCK_LEN], key[32];
    blob[0] = 1;
    esp_fill_random(blob + 1, 16 + 24);
    if (!kdf(pin, blob + 1, PIN_BLOCKS, KDF_PASSES, key)) return false;
    crypto_aead_lock(blob + 57, blob + 41, key, blob + 17, (const uint8_t *)id, FH1_USER_ID_LEN, seed, 32);
    crypto_wipe(key, sizeof(key));
    return fh1_settings_set_blob(NS, k, blob, sizeof(blob));
}

static bool seed_locked(const char *id)
{
    uint8_t blob[LOCK_LEN];
    size_t n = sizeof(blob);
    char k[16];
    nvs_key('k', id, k);
    bool locked = fh1_settings_get_blob(NS, k, blob, &n) && n == LOCK_LEN;
    crypto_wipe(blob, sizeof(blob));
    return locked;
}

void users_key_pair(const uint8_t seed[32], uint8_t sk[64], uint8_t pk[32])
{
    uint8_t s[32];
    memcpy(s, seed, 32);
    crypto_ed25519_key_pair(sk, pk, s);      // wipes s
}

// ── Records (canonical CBOR) ──


void users_w_raw(wbuf_t *w, const void *d, size_t k)
{
    if (!w->ok || w->n + k > w->max) {
        w->ok = false;
        return;
    }
    memcpy(w->p + w->n, d, k);
    w->n += k;
}

void users_w_head(wbuf_t *w, uint8_t major, uint32_t v)
{
    uint8_t b[5];
    size_t k;
    if (v < 24) {
        b[0] = major << 5 | v;
        k = 1;
    } else if (v < 256) {
        b[0] = major << 5 | 24, b[1] = v;
        k = 2;
    } else if (v < 65536) {
        b[0] = major << 5 | 25, b[1] = v >> 8, b[2] = v;
        k = 3;
    } else {
        b[0] = major << 5 | 26, b[1] = v >> 24, b[2] = v >> 16, b[3] = v >> 8, b[4] = v;
        k = 5;
    }
    users_w_raw(w, b, k);
}

void users_w_data(wbuf_t *w, uint8_t major, const void *d, size_t k)
{
    users_w_head(w, major, k);
    users_w_raw(w, d, k);
}

// The map; without sig, what is signed.
static size_t rec_encode(const fh1_user_t *u, const uint8_t dev[10], const uint8_t *sig, uint8_t *out, size_t max)
{
    wbuf_t w = { out, 0, max, true };
    users_w_head(&w, 5, sig ? 7 : 6);
    users_w_head(&w, 0, 0), users_w_head(&w, 0, REC_VERSION);
    users_w_head(&w, 0, 1), users_w_data(&w, 2, u->pk, 32);
    users_w_head(&w, 0, 2), users_w_data(&w, 3, u->name, strlen(u->name));
    users_w_head(&w, 0, 3), users_w_data(&w, 2, u->avatar, u->avatar_len);
    users_w_head(&w, 0, 4), users_w_head(&w, 0, u->rev);
    users_w_head(&w, 0, 5), users_w_data(&w, 2, dev, 10);
    if (sig) users_w_head(&w, 0, 6), users_w_data(&w, 2, sig, 64);
    return w.ok ? w.n : 0;
}


bool users_r_head(rbuf_t *r, uint8_t major, uint32_t *v)
{
    if (r->i >= r->n) return false;
    const uint8_t b = r->p[r->i++], ai = b & 31;
    if (b >> 5 != major) return false;
    int k = ai < 24 ? 0 : ai == 24 ? 1 : ai == 25 ? 2 : ai == 26 ? 4 : -1;
    if (k < 0 || r->i + k > r->n) return false;
    uint32_t x = k ? 0 : ai;
    for (int j = 0; j < k; j++) x = x << 8 | r->p[r->i++];
    *v = x;
    return true;
}

bool users_r_data(rbuf_t *r, uint8_t major, const uint8_t **d, uint32_t *len, uint32_t max)
{
    if (!users_r_head(r, major, len) || *len > max || r->i + *len > r->n) return false;
    *d = r->p + r->i;
    r->i += *len;
    return true;
}

bool users_r_key(rbuf_t *r, uint32_t want)
{
    uint32_t k;
    return users_r_head(r, 0, &k) && k == want;
}

bool users_name_ok(const char *s, size_t n)
{
    if (n < FH1_USER_NAME_MIN || n > FH1_USER_NAME_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        const char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}

esp_err_t fh1_users_verify_record(const uint8_t *rec, size_t n, fh1_user_t *out)
{
    rbuf_t r = { rec, n, 0 };
    uint32_t cnt, ver, len, rev;
    const uint8_t *pk, *name, *av, *dev, *sig;
    if (!users_r_head(&r, 5, &cnt) || cnt != 7) return ESP_ERR_INVALID_ARG;
    if (!users_r_key(&r, 0) || !users_r_head(&r, 0, &ver) || ver != REC_VERSION) return ESP_ERR_NOT_SUPPORTED;
    if (!users_r_key(&r, 1) || !users_r_data(&r, 2, &pk, &len, 32) || len != 32) return ESP_ERR_INVALID_ARG;
    fh1_user_t u = { 0 };
    memcpy(u.pk, pk, 32);
    if (!users_r_key(&r, 2) || !users_r_data(&r, 3, &name, &len, FH1_USER_NAME_MAX) || !users_name_ok((const char *)name, len))
        return ESP_ERR_INVALID_ARG;
    memcpy(u.name, name, len);
    if (!users_r_key(&r, 3) || !users_r_data(&r, 2, &av, &len, FH1_USER_AVATAR_MAX)) return ESP_ERR_INVALID_ARG;
    memcpy(u.avatar, av, len);
    u.avatar_len = len;
    if (!users_r_key(&r, 4) || !users_r_head(&r, 0, &rev)) return ESP_ERR_INVALID_ARG;
    u.rev = rev;
    if (!users_r_key(&r, 5) || !users_r_data(&r, 2, &dev, &len, 10) || len != 10) return ESP_ERR_INVALID_ARG;
    if (!users_r_key(&r, 6) || !users_r_data(&r, 2, &sig, &len, 64) || len != 64 || r.i != n) return ESP_ERR_INVALID_ARG;
    // Canonical only: the same fields encode to the same bytes.
    uint8_t buf[REC_MAX];
    size_t m = rec_encode(&u, dev, sig, buf, sizeof(buf));
    if (m != n || memcmp(buf, rec, n) != 0) return ESP_ERR_INVALID_ARG;
    m = rec_encode(&u, dev, NULL, buf, sizeof(buf));
    if (!m || crypto_ed25519_check(sig, pk, buf, m) != 0) return ESP_ERR_INVALID_CRC;
    users_id_of(u.pk, u.id, NULL);
    memcpy(u.tag, u.id, 4);
    if (out) *out = u;
    return ESP_OK;
}

bool users_rec_load(const char *id, uint8_t *rec, size_t *n)
{
    char k[16];
    nvs_key('p', id, k);
    return fh1_settings_get_blob(NS, k, rec, n);
}

// Signs the user's record as it is now (made on this unit) and stores it.
static esp_err_t rec_sign_store(const fh1_user_t *u, const uint8_t seed[32])
{
    uint8_t sk[64], pk[32], sig[64], buf[REC_MAX];
    users_key_pair(seed, sk, pk);
    if (memcmp(pk, u->pk, 32) != 0) {
        crypto_wipe(sk, sizeof(sk));
        return ESP_ERR_INVALID_STATE;
    }
    size_t m = rec_encode(u, s_unit_raw, NULL, buf, sizeof(buf));
    if (!m) {
        crypto_wipe(sk, sizeof(sk));
        return ESP_FAIL;
    }
    crypto_ed25519_sign(sig, sk, buf, m);
    crypto_wipe(sk, sizeof(sk));
    m = rec_encode(u, s_unit_raw, sig, buf, sizeof(buf));
    char k[16];
    nvs_key('p', u->id, k);
    return m && fh1_settings_set_blob(NS, k, buf, m) ? ESP_OK : ESP_FAIL;
}

size_t fh1_users_record(const char *id, uint8_t *out, size_t max)
{
    size_t n = max;
    return users_rec_load(id, out, &n) ? n : 0;
}

// ── The list ──

int users_find(const char *id)
{
    for (int i = 0; i < s_n; i++)
        if (strcmp(s_users[i].id, id) == 0) return i;
    return -1;
}

static int order(const void *a, const void *b)
{
    const fh1_user_t *x = a, *y = b;
    if (x->owner != y->owner) return x->owner ? -1 : 1;
    if (x->kind != y->kind) return x->kind < y->kind ? -1 : 1;
    return strcmp(x->name, y->name);
}

static void save_list(void)
{
    char ids[FH1_USERS_MAX * FH1_USER_ID_LEN];
    for (int i = 0; i < s_n; i++) memcpy(ids + i * FH1_USER_ID_LEN, s_users[i].id, FH1_USER_ID_LEN);
    if (s_n) fh1_settings_set_blob(NS, "list", ids, (size_t)s_n * FH1_USER_ID_LEN);
    else fh1_settings_erase_key(NS, "list");
}

static void resort(void)
{
    for (int i = 0; i < s_n; i++) s_users[i].owner = strcmp(s_users[i].id, s_owner) == 0;
    qsort(s_users, s_n, sizeof(s_users[0]), order);
}

static void load_users(void)
{
    char ids[FH1_USERS_MAX * FH1_USER_ID_LEN];
    size_t n = sizeof(ids);
    s_n = 0;
    if (!fh1_settings_get_blob(NS, "list", ids, &n)) return;
    for (size_t i = 0; i + FH1_USER_ID_LEN <= n && s_n < FH1_USERS_MAX; i += FH1_USER_ID_LEN) {
        char id[FH1_USER_ID_LEN + 1];
        memcpy(id, ids + i, FH1_USER_ID_LEN);
        id[FH1_USER_ID_LEN] = 0;
        uint8_t rec[REC_MAX];
        size_t rn = sizeof(rec);
        fh1_user_t u;
        if (!users_rec_load(id, rec, &rn) || fh1_users_verify_record(rec, rn, &u) != ESP_OK || strcmp(u.id, id) != 0) {
            ESP_LOGW(TAG, "user %s: no valid record, skipped", id);
            continue;
        }
        char k[16];
        nvs_key('m', id, k);
        uint8_t meta[1] = { FH1_USER_LOCAL };
        size_t mn = sizeof(meta);
        fh1_settings_get_blob(NS, k, meta, &mn);
        u.kind = meta[0] == FH1_USER_VISITOR ? FH1_USER_VISITOR : FH1_USER_LOCAL;
        u.pin = seed_locked(id);
        s_users[s_n++] = u;
    }
}

int fh1_users_count(void) { return s_n; }

bool fh1_users_at(int i, fh1_user_t *out)
{
    if (i < 0 || i >= s_n) return false;
    *out = s_users[i];
    return true;
}

bool fh1_users_get(const char *id, fh1_user_t *out)
{
    const int i = id ? users_find(id) : -1;
    if (i < 0) return false;
    if (out) *out = s_users[i];
    return true;
}

const char *fh1_users_owner(void) { return s_owner; }
const char *fh1_users_unit_id(void) { return s_unit; }

bool fh1_users_name_check(const char *name, const char *except_id, char *clean, size_t n, const char **why)
{
    char c[FH1_USER_NAME_MAX + 2];
    size_t k = 0;
    for (const char *p = name; *p && k < sizeof(c) - 1; p++) c[k++] = (*p >= 'A' && *p <= 'Z') ? *p - 'A' + 'a' : *p;
    c[k] = 0;
    const char *w = NULL;
    if (k < FH1_USER_NAME_MIN || strlen(name) > FH1_USER_NAME_MAX) w = "A name has 3 to 16 characters.";
    else if (!users_name_ok(c, k)) w = "Use only letters a-z, digits, _ and -.";
    else {
        for (int i = 0; i < s_n; i++) {
            if (strcmp(s_users[i].name, c) == 0 && (!except_id || strcmp(s_users[i].id, except_id) != 0)) {
                w = "Someone here already has that name.";
                break;
            }
        }
    }
    if (why) *why = w;
    if (clean && n) strlcpy(clean, c, n);
    return !w;
}

// ── Data ──

void fh1_users_data_dir(const char *user_id, const char *app, char *out, size_t n)
{
    snprintf(out, n, USERS_DIR "/%s/data/%s", user_id, app);
}

static void data_root(char *out, size_t n)
{
    fh1_user_t u;
    if (s_n && fh1_users_player(0, &u)) snprintf(out, n, USERS_DIR "/%s/data", u.id);
    else strlcpy(out, FH1_DATA_DIR, n);
}

void fh1_users_remove_app_data(const char *app)
{
    char p[96];
    snprintf(p, sizeof(p), FH1_DATA_DIR "/%s", app);
    fh1_rmtree(p);
    for (int i = 0; i < s_n; i++) {
        fh1_users_data_dir(s_users[i].id, app, p, sizeof(p));
        fh1_rmtree(p);
    }
}

// The owner takes over the unit's app data from before users existed.
static void adopt_unit_data(const char *id)
{
    char dst[64];
    snprintf(dst, sizeof(dst), USERS_DIR "/%s/data", id);
    fh1_mkdirs_parent(dst);
    struct stat st;
    if (stat(dst, &st) == 0) return;
    if (rename(FH1_DATA_DIR, dst) == 0) {
        ESP_LOGI(TAG, "the app data before users is now %s's", id);
        fh1_mkdirs(FH1_DATA_DIR);
    } else {
        fh1_mkdirs(dst);
    }
    fh1_int_info_changed();
}

// ── Users ──

static esp_err_t add_user(fh1_user_t *u, const uint8_t seed[32])
{
    char k[16];
    nvs_key('m', u->id, k);
    uint8_t meta[1] = { u->kind };
    if (!fh1_settings_set_blob(NS, k, meta, 1) || !seed_put(u->id, seed, NULL)) return ESP_FAIL;
    u->pin = false;
    s_users[s_n++] = *u;
    if (s_n == 1 || !s_owner[0]) {
        strlcpy(s_owner, u->id, sizeof(s_owner));
        fh1_settings_set_str(NS, "owner", s_owner);
        adopt_unit_data(u->id);
    }
    if (!s_use[0]) strlcpy(s_use, s_owner, sizeof(s_use));
    char dir[64];
    snprintf(dir, sizeof(dir), USERS_DIR "/%s/data", u->id);
    fh1_mkdirs(dir);
    resort();
    save_list();
    return ESP_OK;
}

esp_err_t fh1_users_create(const char *name, fh1_user_kind_t kind, char id_out[FH1_USER_ID_LEN + 1])
{
    char clean[FH1_USER_NAME_MAX + 1];
    if (!fh1_users_name_check(name, NULL, clean, sizeof(clean), NULL)) return ESP_ERR_INVALID_ARG;
    if (s_n >= FH1_USERS_MAX) return ESP_ERR_NO_MEM;
    if (kind == FH1_USER_VISITOR && !s_n) return ESP_ERR_INVALID_STATE;     // the first user is the owner
    uint8_t seed[32], sk[64];
    fh1_user_t u = { 0 };
    esp_fill_random(seed, sizeof(seed));
    users_key_pair(seed, sk, u.pk);
    crypto_wipe(sk, sizeof(sk));
    users_id_of(u.pk, u.id, NULL);
    memcpy(u.tag, u.id, 4);
    strlcpy(u.name, clean, sizeof(u.name));
    u.rev = 1;
    u.kind = kind;
    esp_err_t r = rec_sign_store(&u, seed);
    if (r == ESP_OK) r = add_user(&u, seed);
    crypto_wipe(seed, sizeof(seed));
    if (r != ESP_OK) return r;
    if (id_out) strlcpy(id_out, u.id, FH1_USER_ID_LEN + 1);
    ESP_LOGI(TAG, "new %s %s#%s", kind == FH1_USER_VISITOR ? "visitor" : "user", u.name, u.tag);
    users_changed();
    return ESP_OK;
}

esp_err_t fh1_users_rename(const char *id, const char *name, const char *pin)
{
    const int i = users_find(id);
    if (i < 0) return ESP_ERR_NOT_FOUND;
    char clean[FH1_USER_NAME_MAX + 1];
    if (!fh1_users_name_check(name, id, clean, sizeof(clean), NULL)) return ESP_ERR_INVALID_ARG;
    uint8_t seed[32];
    esp_err_t r = users_seed_get(id, pin, seed);
    if (r != ESP_OK) return r;
    fh1_user_t u = s_users[i];
    strlcpy(u.name, clean, sizeof(u.name));
    u.rev++;
    r = rec_sign_store(&u, seed);
    crypto_wipe(seed, sizeof(seed));
    if (r != ESP_OK) return r;
    s_users[i] = u;
    resort();
    users_changed();
    return ESP_OK;
}

esp_err_t fh1_users_set_avatar(const char *id, const uint8_t *code, size_t n, const char *pin)
{
    const int i = users_find(id);
    if (i < 0) return ESP_ERR_NOT_FOUND;
    if (n > FH1_USER_AVATAR_MAX) return ESP_ERR_INVALID_ARG;
    uint8_t seed[32];
    esp_err_t r = users_seed_get(id, pin, seed);
    if (r != ESP_OK) return r;
    fh1_user_t u = s_users[i];
    memcpy(u.avatar, code, n);
    u.avatar_len = n;
    u.rev++;
    r = rec_sign_store(&u, seed);
    crypto_wipe(seed, sizeof(seed));
    if (r != ESP_OK) return r;
    s_users[i] = u;
    users_changed();
    return ESP_OK;
}

esp_err_t fh1_users_make_owner(const char *id)
{
    const int i = users_find(id);
    if (i < 0) return ESP_ERR_NOT_FOUND;
    if (s_users[i].kind != FH1_USER_LOCAL) return ESP_ERR_INVALID_STATE;
    strlcpy(s_owner, id, sizeof(s_owner));
    fh1_settings_set_str(NS, "owner", s_owner);
    resort();
    users_changed();
    return ESP_OK;
}

static void save_links(void);

esp_err_t fh1_users_remove(const char *id)
{
    const int i = users_find(id);
    if (i < 0) return ESP_ERR_NOT_FOUND;
    if (strcmp(id, s_owner) == 0) return ESP_ERR_INVALID_STATE;
    char k[16], dir[64];
    for (const char *kind = "pmk"; *kind; kind++) {
        nvs_key(*kind, id, k);
        fh1_settings_erase_key(NS, k);
    }
    snprintf(dir, sizeof(dir), USERS_DIR "/%s", id);
    fh1_rmtree(dir);
    fh1_int_info_changed();
    for (int j = 0; j < s_nlinks;) {
        if (strcmp(s_links[j].user, id) == 0) s_links[j] = s_links[--s_nlinks];
        else j++;
    }
    save_links();
    ESP_LOGI(TAG, "removed %s#%s", s_users[i].name, s_users[i].tag);
    s_users[i] = s_users[--s_n];
    if (strcmp(s_use, id) == 0) strlcpy(s_use, s_owner, sizeof(s_use));
    resort();
    save_list();
    users_changed();
    return ESP_OK;
}

static bool pin_ok(const char *pin)
{
    const size_t n = pin ? strlen(pin) : 0;
    if (n < FH1_USER_PIN_MIN || n > FH1_USER_PIN_MAX) return false;
    for (size_t i = 0; i < n; i++)
        if (pin[i] < '0' || pin[i] > '9') return false;
    return true;
}

esp_err_t fh1_users_set_pin(const char *id, const char *old_pin, const char *new_pin)
{
    const int i = users_find(id);
    if (i < 0) return ESP_ERR_NOT_FOUND;
    if (new_pin && new_pin[0] && !pin_ok(new_pin)) return ESP_ERR_INVALID_ARG;
    uint8_t seed[32];
    esp_err_t r = users_seed_get(id, old_pin, seed);
    if (r != ESP_OK) return r;
    const bool ok = seed_put(id, seed, new_pin);
    crypto_wipe(seed, sizeof(seed));
    if (!ok) return ESP_FAIL;
    s_users[i].pin = new_pin && new_pin[0];
    users_changed();
    return ESP_OK;
}

bool fh1_users_check_pin(const char *id, const char *pin)
{
    const int i = users_find(id);
    if (i < 0) return false;
    if (!s_users[i].pin) return true;
    uint8_t seed[32];
    const bool ok = users_seed_get(id, pin, seed) == ESP_OK;
    crypto_wipe(seed, sizeof(seed));
    return ok;
}

// ── Controllers and players ──

static void save_links(void)
{
    if (s_nlinks) fh1_settings_set_blob(NS, "pads", s_links, (size_t)s_nlinks * sizeof(link_t));
    else fh1_settings_erase_key(NS, "pads");
}

static void load_links(void)
{
    size_t n = sizeof(s_links);
    s_nlinks = 0;
    if (!fh1_settings_get_blob(NS, "pads", s_links, &n)) return;
    for (size_t i = 0; i < n / sizeof(link_t); i++) {
        link_t l = s_links[i];
        l.pad[17] = l.user[FH1_USER_ID_LEN] = 0;
        if (users_find(l.user) >= 0) s_links[s_nlinks++] = l;      // drops links to users that are gone
    }
}

esp_err_t fh1_users_link_pad(const char *pad_id, const char *user_id)
{
    if (!pad_id || !pad_id[0]) return ESP_ERR_INVALID_ARG;
    if (user_id && user_id[0] && users_find(user_id) < 0) return ESP_ERR_NOT_FOUND;
    for (int j = 0; j < s_nlinks; j++) {
        if (strcmp(s_links[j].pad, pad_id) == 0) {
            s_links[j] = s_links[--s_nlinks];
            break;
        }
    }
    if (user_id && user_id[0]) {
        if (s_nlinks >= LINKS_MAX) return ESP_ERR_NO_MEM;
        link_t *l = &s_links[s_nlinks++];
        memset(l, 0, sizeof(*l));
        strlcpy(l->pad, pad_id, sizeof(l->pad));
        strlcpy(l->user, user_id, sizeof(l->user));
    }
    save_links();
    users_changed();
    return ESP_OK;
}

bool fh1_users_pad_user(const char *pad_id, char user_id[FH1_USER_ID_LEN + 1])
{
    for (int j = 0; j < s_nlinks; j++) {
        if (strcmp(s_links[j].pad, pad_id) == 0) {
            if (user_id) strlcpy(user_id, s_links[j].user, FH1_USER_ID_LEN + 1);
            return true;
        }
    }
    return false;
}

int fh1_users_pads_of(const char *user_id, char ids[][18], int max)
{
    int k = 0;
    for (int j = 0; j < s_nlinks && k < max; j++)
        if (strcmp(s_links[j].user, user_id) == 0) strlcpy(ids[k++], s_links[j].pad, 18);
    return k;
}

// The user whose controller plays as this player, if any.
static bool linked_player(int player, char uid[FH1_USER_ID_LEN + 1])
{
    fh1_gamepad_info_t g;
    return fh1_gamepad_of_player(player, &g) && fh1_users_pad_user(g.id, uid) && users_find(uid) >= 0;
}

static bool plays_elsewhere(const char *id)
{
    char uid[FH1_USER_ID_LEN + 1];
    for (int p = 1; p < FH1_PLAYERS; p++)
        if (linked_player(p, uid) && strcmp(uid, id) == 0) return true;
    return false;
}

// Player 1 without a controller of their own: the user in use, unless they play as another player with their
// own controller; then the owner, then the first local user who does not.
bool fh1_users_player(int player, fh1_user_t *out)
{
    char uid[FH1_USER_ID_LEN + 1];
    if (linked_player(player, uid)) return fh1_users_get(uid, out);
    if (player != 0 || !s_n) return false;
    if (s_use[0] && !plays_elsewhere(s_use)) return fh1_users_get(s_use, out);
    if (s_owner[0] && !plays_elsewhere(s_owner)) return fh1_users_get(s_owner, out);
    for (int i = 0; i < s_n; i++)
        if (s_users[i].kind == FH1_USER_LOCAL && !plays_elsewhere(s_users[i].id)) return fh1_users_get(s_users[i].id, out);
    return fh1_users_get(s_use[0] ? s_use : s_owner, out);
}

void fh1_users_use(const char *id)
{
    if (users_find(id) < 0) return;
    strlcpy(s_use, id, sizeof(s_use));
    users_changed();
}

const char *fh1_users_in_use(void) { return s_use; }

// ── Backups ──

esp_err_t fh1_users_export(const char *id, const char *pin, const char *pass, char *path_out, size_t n)
{
    const int i = users_find(id);
    if (i < 0) return ESP_ERR_NOT_FOUND;
    if (!pass || strlen(pass) < 6) return ESP_ERR_INVALID_ARG;
    uint8_t plain[33 + REC_MAX];
    size_t rn = REC_MAX;
    if (!users_rec_load(id, plain + 33, &rn)) return ESP_ERR_NOT_FOUND;
    esp_err_t r = users_seed_get(id, pin, plain);
    if (r != ESP_OK) return r;
    plain[32] = 0;                                   // flags, none yet
    const size_t len = 33 + rn;
    uint8_t file[BACKUP_MAX], key[32];
    uint8_t *h = file;
    memcpy(h, "FH1U", 4);
    h[4] = 1;
    h[5] = BACKUP_BLOCKS & 0xff, h[6] = BACKUP_BLOCKS >> 8;
    h[7] = KDF_PASSES;
    esp_fill_random(h + 8, 16 + 24);
    h[48] = len & 0xff, h[49] = len >> 8;
    if (!kdf(pass, h + 8, BACKUP_BLOCKS, KDF_PASSES, key)) {
        crypto_wipe(plain, sizeof(plain));
        return ESP_ERR_NO_MEM;
    }
    crypto_aead_lock(file + BACKUP_HDR + 16, file + BACKUP_HDR, key, h + 24, h, BACKUP_HDR, plain, len);
    crypto_wipe(key, sizeof(key));
    crypto_wipe(plain, sizeof(plain));
    if (fh1_mkdirs(FH1_USERS_BACKUP_DIR) != ESP_OK) return ESP_ERR_INVALID_STATE;     // no card
    char path[96];
    snprintf(path, sizeof(path), FH1_USERS_BACKUP_DIR "/%s-%s.fh1u", s_users[i].name, s_users[i].tag);
    FILE *f = fopen(path, "wb");
    if (!f) return ESP_ERR_INVALID_STATE;
    const bool ok = fwrite(file, BACKUP_HDR + 16 + len, 1, f) == 1;
    fclose(f);
    if (!ok) return ESP_FAIL;
    if (path_out) strlcpy(path_out, path, n);
    ESP_LOGI(TAG, "backup of %s#%s: %s", s_users[i].name, s_users[i].tag, path);
    return ESP_OK;
}

int fh1_users_backups(char names[][40], int max)
{
    DIR *d = opendir(FH1_USERS_BACKUP_DIR);
    if (!d) return 0;
    int k = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && k < max) {
        const size_t l = strlen(e->d_name);
        if (l > 5 && l < 40 && strcasecmp(e->d_name + l - 5, ".fh1u") == 0) strlcpy(names[k++], e->d_name, 40);
    }
    closedir(d);
    return k;
}

esp_err_t fh1_users_import(const char *file, const char *pass, char id_out[FH1_USER_ID_LEN + 1])
{
    char path[96];
    snprintf(path, sizeof(path), FH1_USERS_BACKUP_DIR "/%s", file);
    uint8_t buf[BACKUP_MAX + 1];
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    const size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n < BACKUP_HDR + 16 + 33 || n > BACKUP_MAX || memcmp(buf, "FH1U", 4) != 0 || buf[4] != 1) return ESP_ERR_INVALID_ARG;
    const uint32_t blocks = buf[5] | buf[6] << 8, passes = buf[7];
    const size_t len = buf[48] | buf[49] << 8;
    if (blocks < 8 || blocks > 4096 || passes < 1 || passes > 10 || BACKUP_HDR + 16 + len != n) return ESP_ERR_INVALID_ARG;
    uint8_t key[32], plain[33 + REC_MAX];
    if (!kdf(pass, buf + 8, blocks, passes, key)) return ESP_ERR_NO_MEM;
    const int bad = crypto_aead_unlock(plain, buf + BACKUP_HDR, key, buf + 24, buf, BACKUP_HDR, buf + BACKUP_HDR + 16, len);
    crypto_wipe(key, sizeof(key));
    if (bad) return ESP_ERR_INVALID_CRC;
    fh1_user_t u;
    uint8_t sk[64], pk[32];
    esp_err_t r = fh1_users_verify_record(plain + 33, len - 33, &u);
    users_key_pair(plain, sk, pk);
    crypto_wipe(sk, sizeof(sk));
    if (r == ESP_OK && memcmp(pk, u.pk, 32) != 0) r = ESP_ERR_INVALID_CRC;
    if (r != ESP_OK) {
        crypto_wipe(plain, sizeof(plain));
        return r;
    }
    char k[16];
    const int i = users_find(u.id);
    if (i >= 0) {                                   // already here: a newer profile updates it
        if (u.rev <= s_users[i].rev) r = ESP_ERR_INVALID_STATE;
        else {
            nvs_key('p', u.id, k);
            r = fh1_settings_set_blob(NS, k, plain + 33, len - 33) ? ESP_OK : ESP_FAIL;
            if (r == ESP_OK) {
                u.kind = s_users[i].kind;
                u.pin = s_users[i].pin;
                s_users[i] = u;
                resort();
            }
        }
    } else if (s_n >= FH1_USERS_MAX) {
        r = ESP_ERR_NO_MEM;
    } else if (!fh1_users_name_check(u.name, NULL, NULL, 0, NULL)) {
        r = ESP_ERR_INVALID_SIZE;                   // someone here has the name
    } else {
        nvs_key('p', u.id, k);
        u.kind = FH1_USER_LOCAL;
        r = fh1_settings_set_blob(NS, k, plain + 33, len - 33) ? add_user(&u, plain) : ESP_FAIL;
    }
    crypto_wipe(plain, sizeof(plain));
    if (r != ESP_OK) return r;
    if (id_out) strlcpy(id_out, u.id, FH1_USER_ID_LEN + 1);
    ESP_LOGI(TAG, "restored %s#%s (rev %lu)", u.name, u.tag, (unsigned long)u.rev);
    users_changed();
    return ESP_OK;
}

// Dev: back to no users (the unit key stays). The owner's data goes back to being the unit's; the other users'
// data is deleted. Nothing changes if the owner's data cannot be moved back.
esp_err_t fh1_users_reset(void)
{
    if (s_owner[0]) {
        char src[64];
        snprintf(src, sizeof(src), USERS_DIR "/%s/data", s_owner);
        struct stat st;
        if (stat(src, &st) == 0) {
            rmdir(FH1_DATA_DIR);                     // only when empty
            if (rename(src, FH1_DATA_DIR) != 0) return ESP_ERR_INVALID_STATE;
        }
    }
    char k[16];
    for (int i = 0; i < s_n; i++) {
        for (const char *kind = "pmk"; *kind; kind++) {
            nvs_key(*kind, s_users[i].id, k);
            fh1_settings_erase_key(NS, k);
        }
    }
    fh1_settings_erase_key(NS, "list");
    fh1_settings_erase_key(NS, "owner");
    fh1_settings_erase_key(NS, "pads");
    fh1_rmtree(USERS_DIR);
    fh1_mkdirs(USERS_DIR);
    fh1_mkdirs(FH1_DATA_DIR);
    fh1_int_info_changed();
    s_n = s_nlinks = 0;
    s_owner[0] = s_use[0] = 0;
    users_social_init();                         // the contacts went with the folder
    ESP_LOGW(TAG, "users reset");
    users_changed();
    return ESP_OK;
}

// ── Start ──

void fh1_users_init(void)
{
    psa_crypto_init();
    uint8_t seed[32], sk[64], pk[32];
    size_t n = sizeof(seed);
    if (!fh1_settings_get_blob(NS, "unit", seed, &n) || n != 32) {
        esp_fill_random(seed, sizeof(seed));
        fh1_settings_set_blob(NS, "unit", seed, sizeof(seed));
        ESP_LOGI(TAG, "made this unit's key");
    }
    users_key_pair(seed, sk, pk);
    crypto_wipe(sk, sizeof(sk));
    crypto_wipe(seed, sizeof(seed));
    users_id_of(pk, s_unit, s_unit_raw);
    fh1_settings_get_str(NS, "owner", s_owner, sizeof(s_owner));
    load_users();
    if (s_n && users_find(s_owner) < 0) {                 // repair: the first local user
        for (int i = 0; i < s_n; i++) {
            if (s_users[i].kind == FH1_USER_LOCAL) {
                strlcpy(s_owner, s_users[i].id, sizeof(s_owner));
                fh1_settings_set_str(NS, "owner", s_owner);
                break;
            }
        }
    }
    if (!s_n) s_owner[0] = 0;
    resort();
    load_links();
    strlcpy(s_use, s_owner, sizeof(s_use));
    fh1_mkdirs(USERS_DIR);
    fh1_set_data_root_fn(data_root);
    users_social_init();
    ESP_LOGI(TAG, "unit %s; %d users%s%s", s_unit, s_n, s_n ? ", owner " : "", s_n ? s_users[0].name : "");
}
