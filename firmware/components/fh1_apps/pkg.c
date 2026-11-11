// .fpk packages: an uncompressed ustar archive with manifest.json, app.elf, SHA256SUMS and optional
// icon and assets (firmware/PLAN.md, "App packages"). Install = check, unpack to a temporary folder,
// verify every checksum, then swap it into /int/apps/<id>.
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/stat.h>
#include <dirent.h>
#include "apps_priv.h"
#include "fh1_storage.h"
#include "fh1_users.h"
#include "fh1_sdk.h"
#include "cJSON.h"
#include "psa/crypto.h"
#include "esp_log.h"

static const char *TAG = "pkg";

#define MAX_FILES 64

typedef struct {
    char name[100];
    uint32_t size;
    char type;
} tar_entry_t;

static uint32_t octal(const char *p, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n && p[i]; i++) {
        if (p[i] >= '0' && p[i] <= '7') v = v * 8 + (p[i] - '0');
    }
    return v;
}

// Reads the next header. Returns 1 for an entry, 0 at the end of the archive, -1 on a bad archive.
static int tar_next(FILE *f, tar_entry_t *e)
{
    uint8_t h[512];
    if (fread(h, 1, 512, f) != 512) return -1;
    bool zero = true;
    for (int i = 0; i < 512 && zero; i++) zero = h[i] == 0;
    if (zero) return 0;
    uint32_t sum = 0;
    for (int i = 0; i < 512; i++) sum += (i >= 148 && i < 156) ? ' ' : h[i];
    if (sum != octal((char *)h + 148, 8)) return -1;
    char prefix[156] = { 0 }, name[101] = { 0 };
    memcpy(prefix, h + 345, 155);
    memcpy(name, h, 100);
    if (prefix[0]) snprintf(e->name, sizeof(e->name), "%.40s/%.58s", prefix, name);
    else strlcpy(e->name, name, sizeof(e->name));
    e->size = octal((char *)h + 124, 12);
    e->type = h[156] ? h[156] : '0';
    // Tools often write "./name".
    if (strncmp(e->name, "./", 2) == 0) memmove(e->name, e->name + 2, strlen(e->name) - 1);
    size_t l = strlen(e->name);
    if (l && e->name[l - 1] == '/') e->name[l - 1] = 0;
    return 1;
}

static void tar_skip(FILE *f, uint32_t size)
{
    fseek(f, (size + 511) & ~511u, SEEK_CUR);
}

static bool safe_name(const char *n)
{
    if (!n[0] || n[0] == '/' || strstr(n, "..")) return false;
    for (const char *p = n; *p; p++) {
        if (!isalnum((unsigned char)*p) && !strchr("._-/", *p)) return false;
    }
    return true;
}

static uint32_t caps_list(const cJSON *arr, char *err, size_t n)
{
    uint32_t caps = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        const char *s = cJSON_GetStringValue(it);
        uint32_t c = s ? fh1_cap_from_name(s) : 0;
        if (!c) {
            snprintf(err, n, "unknown capability %s", s ? s : "?");
            return 0;
        }
        caps |= c;
    }
    return caps;
}

esp_err_t pkg_manifest_parse(const char *json, fh1_manifest_t *m, char *err, size_t n)
{
    memset(m, 0, sizeof(*m));
    cJSON *j = cJSON_Parse(json);
    if (!j) {
        snprintf(err, n, "manifest.json is not valid JSON");
        return ESP_FAIL;
    }
    esp_err_t ret = ESP_FAIL;
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(j, "id"));
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(j, "name"));
    const char *ver = cJSON_GetStringValue(cJSON_GetObjectItem(j, "version"));
    const char *sdk = cJSON_GetStringValue(cJSON_GetObjectItem(j, "sdk"));
    const char *entry = cJSON_GetStringValue(cJSON_GetObjectItem(j, "entry"));
    int major = -1, minor = -1;
    bool id_ok = id && strlen(id) >= 3 && strlen(id) < sizeof(m->id) && islower((unsigned char)id[0]);
    for (const char *p = id; id_ok && *p; p++) id_ok = islower((unsigned char)*p) || isdigit((unsigned char)*p) || strchr("._-", *p);
    if (!id_ok) snprintf(err, n, "bad or missing id (lowercase, digits, . _ -)");
    else if (!name || !name[0] || strlen(name) >= sizeof(m->name)) snprintf(err, n, "bad or missing name");
    else if (!ver || strlen(ver) >= sizeof(m->version)) snprintf(err, n, "bad or missing version");
    else if (!sdk || sscanf(sdk, "%d.%d", &major, &minor) != 2) snprintf(err, n, "bad or missing sdk (\"major.minor\")");
    else if (major != FH1_SDK_MAJOR || minor > FH1_SDK_MINOR)
        snprintf(err, n, "built for SDK %d.%d; this firmware has %d.%d", major, minor, FH1_SDK_MAJOR, FH1_SDK_MINOR);
    else if (entry && strcmp(entry, "app.elf") != 0) snprintf(err, n, "entry must be app.elf");
    else {
        strlcpy(m->id, id, sizeof(m->id));
        const char *pub = cJSON_GetStringValue(cJSON_GetObjectItem(j, "publisher"));
        if (pub) strlcpy(m->publisher, pub, sizeof(m->publisher));
        strlcpy(m->name, name, sizeof(m->name));
        strlcpy(m->version, ver, sizeof(m->version));
        err[0] = 0;
        m->needs = caps_list(cJSON_GetObjectItem(j, "needs"), err, n);
        if (!err[0]) m->wants = caps_list(cJSON_GetObjectItem(j, "wants"), err, n);
        const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(j, "kind"));
        const char *where = cJSON_GetStringValue(cJSON_GetObjectItem(j, "where"));
        m->assets = kind && strcmp(kind, "assets") == 0;
        m->on_sd = where && strcmp(where, "sd") == 0;
        m->nuses = 0;
        const cJSON *uses = cJSON_GetObjectItem(j, "uses"), *u;
        cJSON_ArrayForEach(u, uses) {
            if (!cJSON_IsString(u) || m->nuses >= FH1_USES_MAX) continue;
            strlcpy(m->uses[m->nuses++], u->valuestring, sizeof(m->uses[0]));
        }
        if (!err[0]) ret = ESP_OK;
    }
    cJSON_Delete(j);
    return ret;
}

esp_err_t pkg_peek(const char *path, fh1_manifest_t *m, char *err, size_t n)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, n, "cannot open %s", path);
        return ESP_FAIL;
    }
    tar_entry_t e;
    esp_err_t ret = ESP_FAIL;
    snprintf(err, n, "no manifest.json in the package");
    int r;
    while ((r = tar_next(f, &e)) == 1) {
        if (e.type == '0' && strcmp(e.name, "manifest.json") == 0 && e.size < 4096) {
            char *buf = calloc(1, e.size + 1);
            if (buf && fread(buf, 1, e.size, f) == e.size) ret = pkg_manifest_parse(buf, m, err, n);
            free(buf);
            break;
        }
        tar_skip(f, e.size);
    }
    if (r < 0) snprintf(err, n, "not a valid package (tar)");
    fclose(f);
    return ret;
}

typedef struct {
    char name[100];
    uint8_t sha[32];
} file_sum_t;

static void hex(const uint8_t *b, char *out)
{
    for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", b[i]);
}

// Unpack every file into dir, hashing as it goes.
static esp_err_t extract(const char *path, const char *dir, file_sum_t *sums, int *nsums, char *err, size_t n)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, n, "cannot open the package");
        return ESP_FAIL;
    }
    uint8_t *buf = malloc(4096);
    tar_entry_t e;
    esp_err_t ret = ESP_OK;
    int r;
    *nsums = 0;
    while (ret == ESP_OK && (r = tar_next(f, &e)) == 1) {
        if (!safe_name(e.name)) {
            snprintf(err, n, "unsafe file name in the package: %.40s", e.name);
            ret = ESP_FAIL;
            break;
        }
        char out[200];
        snprintf(out, sizeof(out), "%s/%s", dir, e.name);
        if (e.type == '5') {
            fh1_mkdirs(out);
            continue;
        }
        if (e.type != '0') {
            tar_skip(f, e.size);
            continue;
        }
        if (*nsums >= MAX_FILES) {
            snprintf(err, n, "too many files in the package");
            ret = ESP_FAIL;
            break;
        }
        char *slash = strrchr(out, '/');
        *slash = 0;
        fh1_mkdirs(out);
        *slash = '/';
        FILE *o = fopen(out, "wb");
        if (!o) {
            snprintf(err, n, "cannot write %.40s", e.name);
            ret = ESP_FAIL;
            break;
        }
        psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
        psa_hash_setup(&op, PSA_ALG_SHA_256);
        uint32_t left = e.size;
        while (left && ret == ESP_OK) {
            size_t chunk = left < 4096 ? left : 4096;
            if (fread(buf, 1, chunk, f) != chunk || fwrite(buf, 1, chunk, o) != chunk) {
                snprintf(err, n, "read or write error (storage full?)");
                ret = ESP_FAIL;
            }
            psa_hash_update(&op, buf, chunk);
            left -= chunk;
        }
        fclose(o);
        size_t hl;
        psa_hash_finish(&op, sums[*nsums].sha, 32, &hl);
        strlcpy(sums[*nsums].name, e.name, sizeof(sums[0].name));
        (*nsums)++;
        fseek(f, ((e.size + 511) & ~511u) - e.size, SEEK_CUR);
    }
    if (ret == ESP_OK && r < 0) {
        snprintf(err, n, "not a valid package (tar)");
        ret = ESP_FAIL;
    }
    free(buf);
    fclose(f);
    return ret;
}

// Every file except SHA256SUMS must be listed there with a matching checksum, and every listed file must exist.
static esp_err_t verify(const char *dir, const file_sum_t *sums, int nsums, char *err, size_t n)
{
    char p[160];
    snprintf(p, sizeof(p), "%s/SHA256SUMS", dir);
    FILE *f = fopen(p, "r");
    if (!f) {
        snprintf(err, n, "no SHA256SUMS in the package");
        return ESP_FAIL;
    }
    bool seen[MAX_FILES] = { 0 };
    char line[200];
    esp_err_t ret = ESP_OK;
    while (ret == ESP_OK && fgets(line, sizeof(line), f)) {
        char want[65], name[110];
        if (sscanf(line, "%64s %109s", want, name) != 2) continue;
        const char *nm = name[0] == '*' ? name + 1 : name;
        int i;
        for (i = 0; i < nsums && strcmp(sums[i].name, nm) != 0; i++) {}
        char got[65];
        if (i == nsums) {
            snprintf(err, n, "listed but missing: %.40s", nm);
            ret = ESP_FAIL;
        } else {
            hex(sums[i].sha, got);
            if (strcasecmp(got, want) != 0) {
                snprintf(err, n, "checksum mismatch: %.40s", nm);
                ret = ESP_FAIL;
            }
            seen[i] = true;
        }
    }
    fclose(f);
    for (int i = 0; ret == ESP_OK && i < nsums; i++) {
        if (!seen[i] && strcmp(sums[i].name, "SHA256SUMS") != 0) {
            snprintf(err, n, "not in SHA256SUMS: %.40s", sums[i].name);
            ret = ESP_FAIL;
        }
    }
    return ret;
}

static const char *renamed(const char *id);
static void manifest_renew(const char *dir, const char *id);

// An installed folder's manifest: false if it has none (or it does not parse).
static bool read_manifest(const char *dir, fh1_manifest_t *m)
{
    char p[128], err[64];
    snprintf(p, sizeof(p), "%s/manifest.json", dir);
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    char *buf = malloc(4096);
    const size_t len = buf ? fread(buf, 1, 4095, f) : 0;
    fclose(f);
    if (!buf) return false;
    buf[len] = 0;
    const bool ok = pkg_manifest_parse(buf, m, err, sizeof(err)) == ESP_OK;
    free(buf);
    return ok;
}

esp_err_t pkg_install(const char *path, fh1_manifest_t *m, char *err, size_t n)
{
    psa_crypto_init();
    if (pkg_peek(path, m, err, n) != ESP_OK) return ESP_FAIL;
    // A package from before the short ids installs under its new one (the app and its data stay one).
    const char *to = renamed(m->id), *was = to ? m->id : NULL;
    char old_id[sizeof(m->id)];
    if (to) {
        strlcpy(old_id, m->id, sizeof(old_id));
        was = old_id;
        strlcpy(m->id, to, sizeof(m->id));
        if (!m->publisher[0]) strlcpy(m->publisher, "org.fh1", sizeof(m->publisher));
    }
    struct stat st;
    fh1_fs_info_t fi;
    fh1_sd_info_t sd;
    fh1_sd_info(&sd);
    // An asset package goes where it asks (the SD card, when there is one) and is shared by every app.
    const bool to_sd = m->assets && m->on_sd && sd.mounted;
    const char *base = !m->assets ? FH1_APPS_DIR : to_sd ? FH1_SD_PATH "/assets" : FH1_INT_PATH "/assets";
    if (to_sd) {
        if (stat(path, &st) == 0 && (uint64_t)st.st_size * 11 / 10 > sd.free) {
            snprintf(err, n, "not enough space on the SD card");
            return ESP_FAIL;
        }
    } else if (stat(path, &st) == 0 && fh1_int_info(&fi) == ESP_OK && (uint64_t)st.st_size * 11 / 10 > fi.total - fi.used) {
        snprintf(err, n, "not enough internal storage");
        return ESP_FAIL;
    }
    char tmp[96], dest[96];
    fh1_mkdirs(base);
    snprintf(tmp, sizeof(tmp), "%s/.tmp-%s", base, m->id);
    snprintf(dest, sizeof(dest), "%s/%s", base, m->id);
    // Ids are short: two publishers may pick the same one. An app installed from another publisher stays.
    fh1_manifest_t old;
    if (read_manifest(dest, &old) && old.publisher[0] && m->publisher[0] && strcmp(old.publisher, m->publisher) != 0) {
        snprintf(err, n, "%s is installed from another publisher (%s)", m->id, old.publisher);
        return ESP_FAIL;
    }
    fh1_rmtree(tmp);
    fh1_mkdirs(tmp);
    file_sum_t *sums = calloc(MAX_FILES, sizeof(file_sum_t));
    int nsums = 0;
    esp_err_t ret = sums ? extract(path, tmp, sums, &nsums, err, n) : ESP_ERR_NO_MEM;
    if (ret == ESP_OK) ret = verify(tmp, sums, nsums, err, n);
    free(sums);
    char elf[128];
    snprintf(elf, sizeof(elf), "%s/app.elf", tmp);
    if (ret == ESP_OK && !m->assets && stat(elf, &st) != 0) {
        snprintf(err, n, "no app.elf in the package");
        ret = ESP_FAIL;
    }
    if (ret == ESP_OK) {
        fh1_rmtree(dest);
        if (rename(tmp, dest) != 0) {
            snprintf(err, n, "could not move the app into place");
            ret = ESP_FAIL;
        }
    }
    if (ret != ESP_OK) fh1_rmtree(tmp);
    else {
        if (was) {
            manifest_renew(dest, m->id);
            ESP_LOGI(TAG, "installed %s %s (a package of its old id, %s)", m->id, m->version, was);
        } else ESP_LOGI(TAG, "installed %s %s", m->id, m->version);
    }
    return ret;
}

esp_err_t pkg_uninstall(const char *id)
{
    char p[96];
    struct stat st;
    snprintf(p, sizeof(p), FH1_APPS_DIR "/%s", id);
    esp_err_t ret = fh1_rmtree(p);
    // An asset package: wherever it is.
    snprintf(p, sizeof(p), FH1_SD_PATH "/assets/%s", id);
    if (stat(p, &st) == 0) ret = fh1_rmtree(p);
    snprintf(p, sizeof(p), FH1_INT_PATH "/assets/%s", id);
    if (stat(p, &st) == 0) ret = fh1_rmtree(p);
    fh1_users_remove_app_data(id);          // every user's data of it
    ESP_LOGI(TAG, "uninstalled %s", id);
    return ret;
}

// ── Ids before 0.6 carried their publisher ("org.fh1.isotest"); they are short now ("taticas-da-ilha"), the publisher
// in the manifest. Once, at start: installed apps and asset packages, their manifests (their id, the asset packages
// they use) and every user's data folders move to the new ids, so nothing is lost; a folder already there under
// the new id (a package installed meanwhile) is kept, and the old app's folder is dropped. ──

static const struct { const char *from, *to; } s_renamed[] = {
    { "org.fh1.isotest", "taticas-da-ilha" },
    { "org.fh1.duel", "duel" },
    { "org.fh1.hello", "hello" },
    { "org.fh1.assets.rpg-sfx", "rpg-sfx" },
};
#define NRENAMED (sizeof(s_renamed) / sizeof(s_renamed[0]))

static const char *renamed(const char *id)
{
    for (size_t i = 0; i < NRENAMED; i++)
        if (strcmp(id, s_renamed[i].from) == 0) return s_renamed[i].to;
    return NULL;
}

// base/from moved to base/to: true if it moved. When base/to is there already, base/from goes if drop_old.
static bool move_folder(const char *base, const char *from, const char *to, bool drop_old)
{
    char a[160], b[160];
    struct stat st;
    snprintf(a, sizeof(a), "%s/%s", base, from);
    snprintf(b, sizeof(b), "%s/%s", base, to);
    if (stat(a, &st) != 0) return false;
    if (stat(b, &st) == 0) {
        if (drop_old) fh1_rmtree(a);
        return false;
    }
    if (rename(a, b) != 0) {
        ESP_LOGW(TAG, "could not move %s to %s", a, b);
        return false;
    }
    ESP_LOGI(TAG, "moved %s to %s", a, b);
    return true;
}

// A moved app's manifest: its new id, and the new ids of the asset packages it uses.
static void manifest_renew(const char *dir, const char *id)
{
    char p[160];
    snprintf(p, sizeof(p), "%s/manifest.json", dir);
    FILE *f = fopen(p, "rb");
    if (!f) return;
    char *buf = malloc(4096);
    const size_t len = buf ? fread(buf, 1, 4095, f) : 0;
    fclose(f);
    if (!buf) return;
    buf[len] = 0;
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    if (!j) return;
    cJSON_ReplaceItemInObject(j, "id", cJSON_CreateString(id));
    if (!cJSON_GetObjectItem(j, "publisher")) cJSON_AddStringToObject(j, "publisher", "org.fh1");
    cJSON *u;
    cJSON_ArrayForEach(u, cJSON_GetObjectItem(j, "uses")) {
        const char *to = cJSON_IsString(u) ? renamed(u->valuestring) : NULL;
        if (to) cJSON_SetValuestring(u, to);
    }
    char *out = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!out) return;
    f = fopen(p, "wb");
    if (f) {
        fputs(out, f);
        fclose(f);
    }
    cJSON_free(out);
}

void pkg_migrate_ids(void)
{
    static const char *const homes[] = { FH1_APPS_DIR, FH1_INT_PATH "/assets", FH1_SD_PATH "/assets" };
    for (size_t i = 0; i < NRENAMED; i++) {
        const char *from = s_renamed[i].from, *to = s_renamed[i].to;
        for (size_t k = 0; k < sizeof(homes) / sizeof(homes[0]); k++) {
            if (!move_folder(homes[k], from, to, true)) continue;
            char dir[160];
            snprintf(dir, sizeof(dir), "%s/%s", homes[k], to);
            manifest_renew(dir, to);
        }
        move_folder(FH1_DATA_DIR, from, to, false);                 // the unit's data (before users)
        DIR *d = opendir(FH1_INT_PATH "/users");                    // every user's
        struct dirent *e;
        while (d && (e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            char base[96];
            snprintf(base, sizeof(base), FH1_INT_PATH "/users/%.40s/data", e->d_name);
            move_folder(base, from, to, false);
        }
        if (d) closedir(d);
    }
}
