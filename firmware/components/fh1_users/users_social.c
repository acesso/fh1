// Contacts, trust statements and shared-content headers (fh1_users.h, U4 and U5; firmware/docs/users.md).
//
// Files (the internal storage; the network code in fh1_apps/apps_net.c moves the bytes):
//   /int/users/contacts/<id>.bin             a contact: [header][their signed profile]
//   /int/users/<local>/trust/<other>.out      what the local user says about other (their statement)
//   /int/users/<local>/trust/<other>.in       what other says about the local user
//   /int/users/<local>/outbox/<other>.msg     a statement message not yet delivered to other's unit
// Statements, canonical CBOR signed with Ed25519 (the signature over the same map without key 5):
//   { 0: version 1, 1: from's public key (32 bytes), 2: to's id (text, 16), 3: level, 4: rev, 5: signature }
// Shared-content headers, the same way:
//   { 0: version 1, 1: author's public key, 2: SHA-256 of the item (32 bytes), 3: size, 4: who, 5: signature }
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "fh1_users.h"
#include "users_priv.h"
#include "fh1_storage.h"
#include "monocypher.h"
#include "monocypher-ed25519.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "users";

#define CONTACTS_DIR USERS_DIR "/contacts"
#define REC_MAX 200
#define ST_MAX 160
#define C_MAGIC 0x43314846u          // "FH1C"

// ── Files ──

static bool write_file(const char *path, const void *a, size_t na, const void *b, size_t nb)
{
    fh1_mkdirs_parent(path);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = (!na || fwrite(a, na, 1, f) == 1) && (!nb || fwrite(b, nb, 1, f) == 1);
    fclose(f);
    fh1_int_info_changed();
    return ok;
}

static size_t read_file(const char *path, void *buf, size_t max)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    const size_t n = fread(buf, 1, max, f);
    fclose(f);
    return n;
}

static uint32_t now_s(void)
{
    const time_t t = time(NULL);
    return t > 1700000000 ? (uint32_t)t : 0;          // 0 until the clock is set (no RTC on the dev board)
}

// ── Contacts ──

typedef struct {
    uint32_t magic, ip, seen;
    char unit[FH1_USER_ID_LEN + 1];
    uint16_t len, port;
} c_hdr_t;

static fh1_contact_t *s_c;            // FH1_CONTACTS_MAX, in PSRAM
static int s_nc;

static int c_find(const char *id)
{
    for (int i = 0; i < s_nc; i++)
        if (strcmp(s_c[i].u.id, id) == 0) return i;
    return -1;
}

static void c_path(const char *id, char *p, size_t n) { snprintf(p, n, CONTACTS_DIR "/%s.bin", id); }

static bool c_load(const char *file, fh1_contact_t *c)
{
    uint8_t buf[sizeof(c_hdr_t) + REC_MAX];
    char p[96];
    snprintf(p, sizeof(p), CONTACTS_DIR "/%s", file);
    const size_t n = read_file(p, buf, sizeof(buf));
    c_hdr_t h;
    if (n < sizeof(h)) return false;
    memcpy(&h, buf, sizeof(h));
    if (h.magic != C_MAGIC || sizeof(h) + h.len != n) return false;
    memset(c, 0, sizeof(*c));
    if (fh1_users_verify_record(buf + sizeof(h), h.len, &c->u) != ESP_OK) return false;
    h.unit[FH1_USER_ID_LEN] = 0;
    strlcpy(c->unit, h.unit, sizeof(c->unit));
    c->ip = h.ip, c->seen = h.seen, c->port = h.port ? h.port : 80;
    return true;
}

static bool c_save(const fh1_contact_t *c, const uint8_t *rec, size_t n)
{
    c_hdr_t h = { .magic = C_MAGIC, .ip = c->ip, .seen = c->seen, .len = (uint16_t)n, .port = c->port };
    strlcpy(h.unit, c->unit, sizeof(h.unit));
    char p[96];
    c_path(c->u.id, p, sizeof(p));
    return write_file(p, &h, sizeof(h), rec, n);
}

void users_social_init(void)
{
    if (!s_c) s_c = heap_caps_calloc(FH1_CONTACTS_MAX, sizeof(fh1_contact_t), MALLOC_CAP_SPIRAM);
    s_nc = 0;
    if (!s_c) return;
    fh1_mkdirs(CONTACTS_DIR);
    DIR *d = opendir(CONTACTS_DIR);
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL && s_nc < FH1_CONTACTS_MAX)
        if (strstr(e->d_name, ".bin") && c_load(e->d_name, &s_c[s_nc])) s_nc++;
    if (d) closedir(d);
    ESP_LOGI(TAG, "%d contacts", s_nc);
}

int fh1_contacts_count(void) { return s_nc; }

bool fh1_contacts_at(int i, fh1_contact_t *out)
{
    if (i < 0 || i >= s_nc) return false;
    *out = s_c[i];
    return true;
}

bool fh1_contacts_get(const char *id, fh1_contact_t *out)
{
    const int i = id ? c_find(id) : -1;
    if (i < 0) return false;
    if (out) *out = s_c[i];
    return true;
}

esp_err_t fh1_contacts_add(const uint8_t *rec, size_t n, const char *unit, uint32_t ip, uint16_t port, bool *is_new)
{
    if (is_new) *is_new = false;
    if (!s_c) return ESP_ERR_NO_MEM;
    fh1_user_t u;
    esp_err_t r = fh1_users_verify_record(rec, n, &u);
    if (r != ESP_OK) return r;
    if (users_find(u.id) >= 0) return ESP_ERR_INVALID_STATE;          // one of ours
    int i = c_find(u.id);
    uint8_t keep[REC_MAX];
    size_t kn = 0;
    if (i < 0) {
        if (s_nc >= FH1_CONTACTS_MAX) {                                 // full: the one seen longest ago goes
            int old = 0;
            for (int j = 1; j < s_nc; j++)
                if (s_c[j].seen < s_c[old].seen) old = j;
            fh1_contacts_remove(s_c[old].u.id);
        }
        i = s_nc++;
        memset(&s_c[i], 0, sizeof(s_c[i]));
        if (is_new) *is_new = true;
    } else if (u.rev < s_c[i].u.rev) {                                  // older: keep ours, note where it was seen
        char p[96];
        c_path(u.id, p, sizeof(p));
        uint8_t buf[sizeof(c_hdr_t) + REC_MAX];
        const size_t fn = read_file(p, buf, sizeof(buf));
        if (fn > sizeof(c_hdr_t)) {
            kn = fn - sizeof(c_hdr_t);
            memcpy(keep, buf + sizeof(c_hdr_t), kn);
        }
        u = s_c[i].u;
    }
    s_c[i].u = u;
    if (unit && unit[0]) strlcpy(s_c[i].unit, unit, sizeof(s_c[i].unit));
    if (ip) s_c[i].ip = ip;
    if (port) s_c[i].port = port;
    if (!s_c[i].port) s_c[i].port = 80;
    s_c[i].seen = now_s();
    c_save(&s_c[i], kn ? keep : rec, kn ? kn : n);
    return ESP_OK;
}

esp_err_t fh1_contacts_remove(const char *id)
{
    const int i = c_find(id);
    if (i < 0) return ESP_ERR_NOT_FOUND;
    char p[96];
    c_path(id, p, sizeof(p));
    unlink(p);
    s_c[i] = s_c[--s_nc];
    users_changed();
    return ESP_OK;
}

// ── Statements ──

static size_t st_encode(const uint8_t pk[32], const char *to, uint8_t level, uint32_t rev, const uint8_t *sig, uint8_t *out, size_t max)
{
    wbuf_t w = { out, 0, max, true };
    users_w_head(&w, 5, sig ? 6 : 5);
    users_w_head(&w, 0, 0), users_w_head(&w, 0, 1);
    users_w_head(&w, 0, 1), users_w_data(&w, 2, pk, 32);
    users_w_head(&w, 0, 2), users_w_data(&w, 3, to, FH1_USER_ID_LEN);
    users_w_head(&w, 0, 3), users_w_head(&w, 0, level);
    users_w_head(&w, 0, 4), users_w_head(&w, 0, rev);
    if (sig) users_w_head(&w, 0, 5), users_w_data(&w, 2, sig, 64);
    return w.ok ? w.n : 0;
}

// Reads a statement; with check, also its canonical form and signature.
static esp_err_t st_decode(const uint8_t *b, size_t n, bool check, uint8_t pk[32], char to[FH1_USER_ID_LEN + 1], uint8_t *level, uint32_t *rev)
{
    rbuf_t r = { b, n, 0 };
    uint32_t cnt, v, len, lv, rv;
    const uint8_t *pkp, *top, *sig;
    if (!users_r_head(&r, 5, &cnt) || cnt != 6) return ESP_ERR_INVALID_ARG;
    if (!users_r_key(&r, 0) || !users_r_head(&r, 0, &v) || v != 1) return ESP_ERR_NOT_SUPPORTED;
    if (!users_r_key(&r, 1) || !users_r_data(&r, 2, &pkp, &len, 32) || len != 32) return ESP_ERR_INVALID_ARG;
    if (!users_r_key(&r, 2) || !users_r_data(&r, 3, &top, &len, FH1_USER_ID_LEN) || len != FH1_USER_ID_LEN) return ESP_ERR_INVALID_ARG;
    if (!users_r_key(&r, 3) || !users_r_head(&r, 0, &lv) || lv > FH1_TRUST_TRUSTED) return ESP_ERR_INVALID_ARG;
    if (!users_r_key(&r, 4) || !users_r_head(&r, 0, &rv)) return ESP_ERR_INVALID_ARG;
    if (!users_r_key(&r, 5) || !users_r_data(&r, 2, &sig, &len, 64) || len != 64 || r.i != n) return ESP_ERR_INVALID_ARG;
    char t[FH1_USER_ID_LEN + 1];
    memcpy(t, top, FH1_USER_ID_LEN);
    t[FH1_USER_ID_LEN] = 0;
    if (check) {
        uint8_t buf[ST_MAX];
        size_t m = st_encode(pkp, t, lv, rv, sig, buf, sizeof(buf));
        if (m != n || memcmp(buf, b, n) != 0) return ESP_ERR_INVALID_ARG;
        m = st_encode(pkp, t, lv, rv, NULL, buf, sizeof(buf));
        if (!m || crypto_ed25519_check(sig, pkp, buf, m) != 0) return ESP_ERR_INVALID_CRC;
    }
    if (pk) memcpy(pk, pkp, 32);
    if (to) strlcpy(to, t, FH1_USER_ID_LEN + 1);
    if (level) *level = lv;
    if (rev) *rev = rv;
    return ESP_OK;
}

static void st_path(const char *local, const char *other, const char *ext, char *p, size_t n)
{
    snprintf(p, n, USERS_DIR "/%s/%s/%s.%s", local, strcmp(ext, "msg") == 0 ? "outbox" : "trust", other, ext);
}

static bool st_read(const char *local, const char *other, const char *ext, uint8_t *level, uint32_t *rev)
{
    uint8_t buf[ST_MAX];
    char p[96];
    st_path(local, other, ext, p, sizeof(p));
    const size_t n = read_file(p, buf, sizeof(buf));
    return n && st_decode(buf, n, false, NULL, NULL, level, rev) == ESP_OK;
}

void fh1_users_relation(const char *local, const char *other, uint8_t *out, uint8_t *in)
{
    uint8_t o = FH1_TRUST_NONE, i = FH1_TRUST_NONE;
    st_read(local, other, "out", &o, NULL);
    st_read(local, other, "in", &i, NULL);
    if (out) *out = o;
    if (in) *in = i;
}

int fh1_users_relations(const char *local, char ids[][FH1_USER_ID_LEN + 1], int max)
{
    char p[64];
    snprintf(p, sizeof(p), USERS_DIR "/%s/trust", local);
    DIR *d = opendir(p);
    int k = 0;
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL && k < max) {
        if (strlen(e->d_name) < FH1_USER_ID_LEN + 3 || e->d_name[FH1_USER_ID_LEN] != '.') continue;
        char id[FH1_USER_ID_LEN + 1];
        memcpy(id, e->d_name, FH1_USER_ID_LEN);
        id[FH1_USER_ID_LEN] = 0;
        bool dup = false;
        for (int j = 0; j < k && !dup; j++) dup = strcmp(ids[j], id) == 0;
        if (!dup) strlcpy(ids[k++], id, FH1_USER_ID_LEN + 1);
    }
    if (d) closedir(d);
    return k;
}

esp_err_t fh1_users_trust(const char *local, const char *other, uint8_t level, const char *pin)
{
    if (users_find(local) < 0 || !other || strlen(other) != FH1_USER_ID_LEN || strcmp(local, other) == 0) return ESP_ERR_INVALID_ARG;
    if (level > FH1_TRUST_TRUSTED) return ESP_ERR_INVALID_ARG;
    uint32_t rev = 0;
    st_read(local, other, "out", NULL, &rev);
    uint8_t seed[32], sk[64], pk[32], sig[64], st[ST_MAX];
    esp_err_t r = users_seed_get(local, pin, seed);
    if (r != ESP_OK) return r;
    users_key_pair(seed, sk, pk);
    crypto_wipe(seed, sizeof(seed));
    size_t m = st_encode(pk, other, level, rev + 1, NULL, st, sizeof(st));
    crypto_ed25519_sign(sig, sk, st, m);
    crypto_wipe(sk, sizeof(sk));
    m = st_encode(pk, other, level, rev + 1, sig, st, sizeof(st));
    char p[96];
    st_path(local, other, "out", p, sizeof(p));
    if (!m || !write_file(p, st, m, NULL, 0)) return ESP_FAIL;
    if (users_find(other) >= 0) {                     // another user of this unit: it is theirs at once
        st_path(other, local, "in", p, sizeof(p));
        write_file(p, st, m, NULL, 0);
    } else {                                          // a contact: to their unit, through the outbox
        uint8_t rec[REC_MAX], msg[FH1_TRUST_MSG_MAX];
        size_t rn = sizeof(rec);
        if (!users_rec_load(local, rec, &rn)) return ESP_FAIL;
        const size_t total = 5 + 2 + rn + 2 + m;
        if (total > sizeof(msg)) return ESP_ERR_INVALID_SIZE;
        memcpy(msg, "FH1T", 4);
        msg[4] = 1;
        msg[5] = rn >> 8, msg[6] = rn;
        memcpy(msg + 7, rec, rn);
        msg[7 + rn] = m >> 8, msg[8 + rn] = m;
        memcpy(msg + 9 + rn, st, m);
        st_path(local, other, "msg", p, sizeof(p));
        if (!write_file(p, msg, total, NULL, 0)) return ESP_FAIL;
    }
    ESP_LOGI(TAG, "%s says %u about %s (rev %lu)", local, level, other, (unsigned long)(rev + 1));
    users_changed();
    return ESP_OK;
}

esp_err_t fh1_users_receive(const uint8_t *msg, size_t n, uint32_t ip, char local[FH1_USER_ID_LEN + 1],
                            char from[FH1_USER_ID_LEN + 1], uint8_t *level)
{
    if (n < 9 || memcmp(msg, "FH1T", 4) != 0 || msg[4] != 1) return ESP_ERR_INVALID_ARG;
    const size_t rn = msg[5] << 8 | msg[6];
    if (7 + rn + 2 > n) return ESP_ERR_INVALID_ARG;
    const size_t sn = msg[7 + rn] << 8 | msg[8 + rn];
    if (9 + rn + sn != n) return ESP_ERR_INVALID_ARG;
    const uint8_t *rec = msg + 7, *st = msg + 9 + rn;
    fh1_user_t u;
    esp_err_t r = fh1_users_verify_record(rec, rn, &u);
    if (r != ESP_OK) return r;
    uint8_t pk[32], lv;
    uint32_t rev;
    char to[FH1_USER_ID_LEN + 1];
    r = st_decode(st, sn, true, pk, to, &lv, &rev);
    if (r != ESP_OK) return r;
    if (memcmp(pk, u.pk, 32) != 0) return ESP_ERR_INVALID_CRC;          // signed by someone else
    if (users_find(to) < 0 || users_find(u.id) >= 0) return ESP_ERR_NOT_FOUND;
    fh1_contacts_add(rec, rn, NULL, ip, 0, NULL);
    uint32_t old = 0;
    if (st_read(to, u.id, "in", NULL, &old) && rev <= old) return ESP_ERR_INVALID_STATE;     // seen already
    char p[96];
    st_path(to, u.id, "in", p, sizeof(p));
    if (!write_file(p, st, sn, NULL, 0)) return ESP_FAIL;
    if (local) strlcpy(local, to, FH1_USER_ID_LEN + 1);
    if (from) strlcpy(from, u.id, FH1_USER_ID_LEN + 1);
    if (level) *level = lv;
    ESP_LOGI(TAG, "%s#%s says %u about %s (rev %lu)", u.name, u.tag, lv, to, (unsigned long)rev);
    users_changed();
    return ESP_OK;
}

int fh1_users_outbox(fh1_outbox_t *out, int max)
{
    int k = 0;
    fh1_user_t u;
    for (int i = 0; fh1_users_at(i, &u) && k < max; i++) {
        char p[96];
        snprintf(p, sizeof(p), USERS_DIR "/%s/outbox", u.id);
        DIR *d = opendir(p);
        struct dirent *e;
        while (d && (e = readdir(d)) != NULL && k < max) {
            if (strlen(e->d_name) != FH1_USER_ID_LEN + 4) continue;
            fh1_outbox_t *o = &out[k];
            memset(o, 0, sizeof(*o));
            strlcpy(o->local, u.id, sizeof(o->local));
            memcpy(o->other, e->d_name, FH1_USER_ID_LEN);
            fh1_contact_t c;
            if (fh1_contacts_get(o->other, &c)) o->ip = c.ip, o->port = c.port;
            st_path(o->local, o->other, "msg", p, sizeof(p));
            o->len = read_file(p, o->msg, sizeof(o->msg));
            if (o->len) k++;
        }
        if (d) closedir(d);
    }
    return k;
}

void fh1_users_outbox_done(const char *local, const char *other)
{
    char p[96];
    st_path(local, other, "msg", p, sizeof(p));
    unlink(p);
    fh1_int_info_changed();
}

// ── Shared content ──

static size_t sh_encode(const uint8_t pk[32], const uint8_t sha[32], uint32_t size, uint8_t who, const uint8_t *sig, uint8_t *out, size_t max)
{
    wbuf_t w = { out, 0, max, true };
    users_w_head(&w, 5, sig ? 6 : 5);
    users_w_head(&w, 0, 0), users_w_head(&w, 0, 1);
    users_w_head(&w, 0, 1), users_w_data(&w, 2, pk, 32);
    users_w_head(&w, 0, 2), users_w_data(&w, 2, sha, 32);
    users_w_head(&w, 0, 3), users_w_head(&w, 0, size);
    users_w_head(&w, 0, 4), users_w_head(&w, 0, who);
    if (sig) users_w_head(&w, 0, 5), users_w_data(&w, 2, sig, 64);
    return w.ok ? w.n : 0;
}

esp_err_t fh1_users_share_sign(const char *author, const char *pin, const uint8_t sha[32], uint32_t size, uint8_t who,
                               uint8_t *out, size_t max, size_t *n)
{
    if (users_find(author) < 0 || who > FH1_SHARE_TRUSTED) return ESP_ERR_INVALID_ARG;
    uint8_t seed[32], sk[64], pk[32], sig[64], buf[ST_MAX];
    esp_err_t r = users_seed_get(author, pin, seed);
    if (r != ESP_OK) return r;
    users_key_pair(seed, sk, pk);
    crypto_wipe(seed, sizeof(seed));
    const size_t m = sh_encode(pk, sha, size, who, NULL, buf, sizeof(buf));
    crypto_ed25519_sign(sig, sk, buf, m);
    crypto_wipe(sk, sizeof(sk));
    const size_t k = sh_encode(pk, sha, size, who, sig, out, max);
    if (!k) return ESP_ERR_INVALID_SIZE;
    if (n) *n = k;
    return ESP_OK;
}

esp_err_t fh1_users_share_check(const uint8_t *hdr, size_t n, const char *viewer, char author[FH1_USER_ID_LEN + 1], bool *allowed)
{
    rbuf_t r = { hdr, n, 0 };
    uint32_t cnt, v, len, size, who;
    const uint8_t *pk, *sha, *sig;
    if (allowed) *allowed = false;
    if (!users_r_head(&r, 5, &cnt) || cnt != 6) return ESP_ERR_INVALID_ARG;
    if (!users_r_key(&r, 0) || !users_r_head(&r, 0, &v) || v != 1) return ESP_ERR_NOT_SUPPORTED;
    if (!users_r_key(&r, 1) || !users_r_data(&r, 2, &pk, &len, 32) || len != 32) return ESP_ERR_INVALID_ARG;
    if (!users_r_key(&r, 2) || !users_r_data(&r, 2, &sha, &len, 32) || len != 32) return ESP_ERR_INVALID_ARG;
    if (!users_r_key(&r, 3) || !users_r_head(&r, 0, &size)) return ESP_ERR_INVALID_ARG;
    if (!users_r_key(&r, 4) || !users_r_head(&r, 0, &who) || who > FH1_SHARE_TRUSTED) return ESP_ERR_INVALID_ARG;
    if (!users_r_key(&r, 5) || !users_r_data(&r, 2, &sig, &len, 64) || len != 64 || r.i != n) return ESP_ERR_INVALID_ARG;
    uint8_t buf[ST_MAX];
    size_t m = sh_encode(pk, sha, size, who, sig, buf, sizeof(buf));
    if (m != n || memcmp(buf, hdr, n) != 0) return ESP_ERR_INVALID_ARG;
    m = sh_encode(pk, sha, size, who, NULL, buf, sizeof(buf));
    if (crypto_ed25519_check(sig, pk, buf, m) != 0) return ESP_ERR_INVALID_CRC;
    char id[FH1_USER_ID_LEN + 1];
    users_id_of(pk, id, NULL);
    if (author) strlcpy(author, id, FH1_USER_ID_LEN + 1);
    if (allowed && viewer) {
        uint8_t out, in;
        fh1_users_relation(viewer, id, &out, &in);
        *allowed = who == FH1_SHARE_EVERYONE || strcmp(viewer, id) == 0 ||
                   (who == FH1_SHARE_FRIENDS && out >= FH1_TRUST_FRIEND && in >= FH1_TRUST_FRIEND) ||
                   (who == FH1_SHARE_TRUSTED && in >= FH1_TRUST_TRUSTED);
    }
    return ESP_OK;
}
